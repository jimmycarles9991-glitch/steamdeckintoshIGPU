#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <string.h>

// This is the registry-facing framebuffer contract. It deliberately starts
// only after the parent has initialized DCN and published a real scanout.
class VanGoghFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(VanGoghFramebuffer)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    IODeviceMemory *getVRAMRange() override;
    const char *getPixelFormats() override;
    IOItemCount getDisplayModeCount() override;
    IOReturn getDisplayModes(IODisplayModeID *modes) override;
    IOReturn getInformationForDisplayMode(IODisplayModeID mode,
                                          IODisplayModeInformation *info) override;
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode,
                                         IOTimingInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                 IOPixelAperture aperture, IOPixelInformation *info) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override;
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override;
    IOReturn setStartupDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn enableController() override;
    IOItemCount getConnectionCount() override;
    IOReturn getAttributeForConnection(IOIndex connection, IOSelect attribute,
                                       uintptr_t *value) override;
    bool hasDDCConnect(IOIndex connection) override;
    IOReturn getDDCBlock(IOIndex connection, UInt32 block, IOSelect type,
                         IOOptionBits options, UInt8 *data, IOByteCount *length) override;

private:
    bool m_scanoutReady{false};
    IOPhysicalAddress m_scanoutPhysical{0};
    IOPhysicalLength m_scanoutBytes{0};
    UInt32 m_width{0};
    UInt32 m_height{0};
    UInt32 m_pitch{0};
    UInt32 m_refresh{0};
    bool m_rotationSupported{false};
    OSData *m_edid{nullptr};
};

OSDefineMetaClassAndStructors(VanGoghFramebuffer, IOFramebuffer)

namespace {
bool readNumber(IOService *service, const char *key, UInt64 *value) {
    OSNumber *number = service ? OSDynamicCast(OSNumber, service->getProperty(key)) : nullptr;
    if (number == nullptr || value == nullptr) return false;
    *value = number->unsigned64BitValue();
    return true;
}
}

bool VanGoghFramebuffer::start(IOService *provider) {
    OSBoolean *ready = provider
        ? OSDynamicCast(OSBoolean, provider->getProperty("VanGoghFramebuffer,ready"))
        : nullptr;
    UInt64 physical = 0, bytes = 0, width = 0, height = 0, pitch = 0, refresh = 0;
    OSBoolean *rotation = provider
        ? OSDynamicCast(OSBoolean, provider->getProperty("VanGoghFramebuffer,rotation-programmed"))
        : nullptr;
    OSBoolean *linearScanout = provider
        ? OSDynamicCast(OSBoolean, provider->getProperty("VanGoghFramebuffer,linear-scanout"))
        : nullptr;
    OSBoolean *gpuAddressMapped = provider
        ? OSDynamicCast(OSBoolean, provider->getProperty("VanGoghFramebuffer,gpu-address-mapped"))
        : nullptr;
    OSBoolean *surfaceProgrammed = provider
        ? OSDynamicCast(OSBoolean, provider->getProperty("VanGoghFramebuffer,surface-programmed"))
        : nullptr;
    const bool rotationProgrammed = (rotation == kOSBooleanTrue);
    if (ready != kOSBooleanTrue || linearScanout != kOSBooleanTrue ||
        gpuAddressMapped != kOSBooleanTrue || surfaceProgrammed != kOSBooleanTrue ||
        !rotationProgrammed ||
        !readNumber(provider, "VanGoghFramebuffer,scanout-physical", &physical) ||
        !readNumber(provider, "VanGoghFramebuffer,scanout-bytes", &bytes) ||
        !readNumber(provider, "VanGoghFramebuffer,width", &width) ||
        !readNumber(provider, "VanGoghFramebuffer,height", &height) ||
        !readNumber(provider, "VanGoghFramebuffer,pitch", &pitch) ||
        !readNumber(provider, "VanGoghFramebuffer,refresh", &refresh)) {
        IOLog("VanGoghFramebuffer: parent has not published a rotated linear scanout\n");
        return false;
    }

    // Cap dimensions and validate all arithmetic before exposing memory to
    // IOGraphics. The parent owns scanout allocation and DCN configuration.
    if (physical == 0 || bytes == 0 || width == 0 || width > 8192 ||
        height == 0 || height > 8192 || pitch > UINT32_MAX ||
        pitch < width * 4 || refresh == 0 || refresh > 240 ||
        height > UINT64_MAX / pitch || bytes < pitch * height ||
        physical > UINT64_MAX - bytes) {
        IOLog("VanGoghFramebuffer: refusing invalid scanout geometry or range\n");
        return false;
    }
    if (width != 1280 || height != 800 || refresh != 60) {
        IOLog("VanGoghFramebuffer: rotated Deck mode requires a 1280x800@60 source surface\n");
        return false;
    }

    m_scanoutPhysical = static_cast<IOPhysicalAddress>(physical);
    m_scanoutBytes = static_cast<IOPhysicalLength>(bytes);
    m_width = static_cast<UInt32>(width);
    m_height = static_cast<UInt32>(height);
    m_pitch = static_cast<UInt32>(pitch);
    m_refresh = static_cast<UInt32>(refresh);
    m_scanoutReady = true;

    // The live Deck state shows AR24, 1280x800, 5120-byte pitch rotated to
    // the 800x1280 eDP mode (DRM ROTATE_270; HUBP angle 3). Buffer address,
    // tiling, and DCC state are runtime-owned and must come from the parent.
    // Advertise rotation only after the parent confirms it programmed DCN.
    m_rotationSupported = true;

    // Only forward a real EDID supplied by the panel/AUX bring-up code.
    // Validate the header, declared extension count, and each 128-byte checksum.
    OSData *providedEdid = OSDynamicCast(
        OSData, provider->getProperty("VanGoghFramebuffer,edid"));
    if (providedEdid != nullptr && providedEdid->getLength() >= 128) {
        const UInt8 *raw = static_cast<const UInt8 *>(providedEdid->getBytesNoCopy());
        const UInt8 header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
        const UInt32 blocks = 1u + raw[126];
        const UInt32 expectedLength = blocks * 128u;
        bool valid = providedEdid->getLength() >= expectedLength &&
                     memcmp(raw, header, sizeof(header)) == 0;
        for (UInt32 block = 0; valid && block < blocks; ++block) {
            UInt8 checksum = 0;
            for (UInt32 i = 0; i < 128; ++i) checksum += raw[block * 128 + i];
            valid = (checksum == 0);
        }
        if (valid) {
            m_edid = OSData::withBytes(raw, expectedLength);
        } else {
            IOLog("VanGoghFramebuffer: ignoring invalid panel EDID\n");
        }
    }

    if (!IOFramebuffer::start(provider)) {
        m_scanoutReady = false;
        if (m_edid != nullptr) {
            m_edid->release();
            m_edid = nullptr;
        }
        return false;
    }
    setProperty("IOFBMemorySize", static_cast<UInt64>(m_scanoutBytes), 64);
    if (m_edid != nullptr) {
        setProperty("IODisplayEDID", m_edid);
        setProperty("EDID", m_edid);
    }
    if (m_rotationSupported) {
        IODisplayScalerInformation scalerInfo{};
        scalerInfo.version = 0;
        scalerInfo.scalerFeatures = kIOScaleCanRotate;
        // These limits describe the source surface before rotation, matching
        // horizontalScaled/verticalScaled in the detailed timing below.
        scalerInfo.maxHorizontalPixels = m_width;
        scalerInfo.maxVerticalPixels = m_height;
        OSData *scalerData = OSData::withBytes(&scalerInfo, sizeof(scalerInfo));
        if (scalerData != nullptr) {
            setProperty("IOFBScalerInfo", scalerData);
            scalerData->release();
        }
    }
    registerService();
    IOLog("VanGoghFramebuffer: registered guarded %ux%u@%u scanout\n",
          m_width, m_height, m_refresh);
    return true;
}

void VanGoghFramebuffer::stop(IOService *provider) {
    m_scanoutReady = false;
    m_rotationSupported = false;
    if (m_edid != nullptr) {
        m_edid->release();
        m_edid = nullptr;
    }
    IOFramebuffer::stop(provider);
}

IODeviceMemory *VanGoghFramebuffer::getApertureRange(IOPixelAperture aperture) {
    if (!m_scanoutReady || aperture != kIOFBSystemAperture) return nullptr;
    IODeviceMemory *range = IODeviceMemory::withRange(m_scanoutPhysical, m_scanoutBytes);
    if (range != nullptr) range->setTag(kIOMapWriteCombineCache);
    return range;
}

IODeviceMemory *VanGoghFramebuffer::getVRAMRange() {
    return getApertureRange(kIOFBSystemAperture);
}

const char *VanGoghFramebuffer::getPixelFormats() { return IO32BitDirectPixels; }
IOItemCount VanGoghFramebuffer::getDisplayModeCount() { return m_scanoutReady ? 1 : 0; }

IOReturn VanGoghFramebuffer::getDisplayModes(IODisplayModeID *modes) {
    if (!modes || !m_scanoutReady) return kIOReturnNotReady;
    modes[0] = 1;
    return kIOReturnSuccess;
}

IOReturn VanGoghFramebuffer::getInformationForDisplayMode(
    IODisplayModeID mode, IODisplayModeInformation *info) {
    if (!info || mode != 1 || !m_scanoutReady) return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    // Mode dimensions describe the physical panel timing. Pixel information
    // below describes the landscape source buffer when the DCN plane rotates.
    info->nominalWidth = m_rotationSupported ? 800 : m_width;
    info->nominalHeight = m_rotationSupported ? 1280 : m_height;
    info->refreshRate = m_refresh << 16;
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag |
                  kDisplayModeDefaultFlag | kDisplayModeBuiltInFlag;
    info->imageWidth = 100;
    info->imageHeight = 150;
    return kIOReturnSuccess;
}

IOReturn VanGoghFramebuffer::getTimingInfoForDisplayMode(
    IODisplayModeID mode, IOTimingInformation *info) {
    if (!info || mode != 1 || !m_scanoutReady) return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    if (!m_rotationSupported) return kIOReturnUnsupportedMode;

    // Deck eDP mode from the live KMS capture: 800x1280@60, totals 872x1324,
    // 69.270 MHz. The source framebuffer is 1280x800 and DCN rotates it 270°.
    info->appleTimingID = kIOTimingIDInvalid;
    info->flags = kIODetailedTimingValid | kIOScalingInfoValid;
    IODetailedTimingInformationV2 *timing = &info->detailedInfo.v2;
    timing->scalerFlags = kIOScaleRotate270;
    timing->horizontalScaled = m_width;
    timing->verticalScaled = m_height;
    timing->signalConfig = kIODigitalSignal;
    timing->pixelClock = 69270000;
    timing->horizontalActive = 800;
    timing->horizontalBlanking = 72;
    timing->horizontalSyncOffset = 32;
    timing->horizontalSyncPulseWidth = 20;
    timing->verticalActive = 1280;
    timing->verticalBlanking = 44;
    timing->verticalSyncOffset = 16;
    timing->verticalSyncPulseWidth = 2;
    timing->horizontalSyncConfig = kIOSyncPositivePolarity;
    timing->verticalSyncConfig = kIOSyncPositivePolarity;
    timing->numLinks = 1;
    return kIOReturnSuccess;
}

UInt64 VanGoghFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
    return 0; // Legacy IOFramebuffer format enumeration.
}

IOReturn VanGoghFramebuffer::getPixelInformation(
    IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *info) {
    if (!info || mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !m_scanoutReady)
        return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    strlcpy(info->pixelFormat, IO32BitDirectPixels, sizeof(info->pixelFormat));
    info->pixelType = kIORGBDirectPixels;
    info->componentCount = 3;
    info->bitsPerComponent = 8;
    info->bitsPerPixel = 32;
    info->bytesPerRow = m_pitch;
    info->activeWidth = m_width;
    info->activeHeight = m_height;
    info->componentMasks[0] = 0x00ff0000;
    info->componentMasks[1] = 0x0000ff00;
    info->componentMasks[2] = 0x000000ff;
    return kIOReturnSuccess;
}

IOReturn VanGoghFramebuffer::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) {
    if (!m_scanoutReady) return kIOReturnNotReady;
    if (mode) *mode = 1;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

IOReturn VanGoghFramebuffer::getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) {
    return getCurrentDisplayMode(mode, depth);
}

IOReturn VanGoghFramebuffer::setStartupDisplayMode(IODisplayModeID mode, IOIndex depth) {
    return (m_scanoutReady && mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupportedMode;
}

IOReturn VanGoghFramebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth) {
    // Report only the mode the parent already programmed. No DCN mode-setting
    // or panel-link training is implemented here yet.
    return (m_scanoutReady && mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupportedMode;
}

IOReturn VanGoghFramebuffer::enableController() {
    return m_scanoutReady ? kIOReturnSuccess : kIOReturnNotReady;
}

IOItemCount VanGoghFramebuffer::getConnectionCount() { return m_scanoutReady ? 1 : 0; }

IOReturn VanGoghFramebuffer::getAttributeForConnection(
    IOIndex connection, IOSelect attribute, uintptr_t *value) {
    if (connection != 0 || !m_scanoutReady) return kIOReturnBadArgument;
    switch (attribute) {
        case kConnectionFlags:
            if (value) *value = kIOConnectionBuiltIn;
            return kIOReturnSuccess;
        case kConnectionEnable:
        case kConnectionCheckEnable:
            if (value) *value = 1;
            return kIOReturnSuccess;
        case kConnectionSupportsHLDDCSense:
            return m_edid != nullptr ? kIOReturnSuccess : kIOReturnUnsupported;
        default:
            return IOFramebuffer::getAttributeForConnection(connection, attribute, value);
    }
}

bool VanGoghFramebuffer::hasDDCConnect(IOIndex connection) {
    return connection == 0 && m_scanoutReady && m_edid != nullptr;
}

IOReturn VanGoghFramebuffer::getDDCBlock(IOIndex connection, UInt32 block,
                                          IOSelect type, IOOptionBits,
                                          UInt8 *data, IOByteCount *length) {
    if (connection != 0 || !m_scanoutReady || type != kIODDCBlockTypeEDID ||
        block == 0 || data == nullptr || length == nullptr || *length < 128 ||
        m_edid == nullptr) {
        return kIOReturnUnsupported;
    }
    const IOByteCount offset = static_cast<IOByteCount>(block - 1) * 128;
    if (offset > m_edid->getLength() || 128 > m_edid->getLength() - offset)
        return kIOReturnUnsupported;
    memcpy(data, static_cast<const UInt8 *>(m_edid->getBytesNoCopy()) + offset, 128);
    *length = 128;
    return kIOReturnSuccess;
}
