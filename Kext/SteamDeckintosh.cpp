#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include "SteamDeckPanelEDID.h"
#include "VanGoghDCN301.h"
#include "VanGoghFirmware.h"
#include "VanGoghIPDiscovery.h"

namespace {
constexpr UInt32 kPciConfigCommandRegister = 0x04;
constexpr UInt16 kPciCommandMemorySpaceEnable = 1u << 1;
constexpr UInt32 kMmIndexRegister = 0x0000;
constexpr UInt32 kMmDataRegister = 0x0004;
constexpr UInt32 kMmIndexHighRegister = 0x0018;
constexpr UInt32 kRccConfigMemsizeRegister = 0x0DE3 * sizeof(UInt32);
constexpr UInt32 kMp0C2Pmsg33Register = 0x16061 * sizeof(UInt32);
constexpr UInt32 kDriverScratch0Register = 0x94 * sizeof(UInt32);
constexpr UInt32 kDriverScratch1Register = 0x95 * sizeof(UInt32);
constexpr UInt32 kDriverScratch2Register = 0x96 * sizeof(UInt32);
constexpr UInt32 kBinarySignature = 0x28211407;
constexpr UInt64 kDiscoveryTmrOffset = 64 * 1024;
constexpr UInt32 kDiscoveryTmrSize = 10 * 1024;
constexpr UInt32 kMaxDiscoverySize = 64 * 1024;
constexpr UInt32 kPanelSourceWidth = 1280;
constexpr UInt32 kPanelSourceHeight = 800;
constexpr UInt32 kPanelSourcePitch = kPanelSourceWidth * 4;
constexpr IOByteCount kPanelSourceBytes =
    static_cast<IOByteCount>(kPanelSourcePitch) * kPanelSourceHeight;
}

class VanGoghDriver : public IOService {
    OSDeclareDefaultStructors(VanGoghDriver)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    void releaseDeviceResources();
    void inspectDCN301Rotation();
    void inspectDCN301Timing();
    void inspectDCN301PipeRouting();
    void inspectVanGoghVMID0();
    bool allocatePanelScanout();
    void applyConfiguredDCN301Rotation(IOService *provider);
    bool programDCN301Rotation(UInt8 hubp, UInt8 angle);
    void validatePSPFirmware(IOService *provider);
    void loadIPDiscovery(IOService *provider);
    bool parseIPDiscovery(const UInt8 *data, IOByteCount length);
    bool readVramDwords(UInt64 offset, UInt32 *data, UInt32 byteCount);
    bool readRegister32(IOByteCount byteOffset, UInt32 *value) const;
    bool writeRegister32(IOByteCount byteOffset, UInt32 value) const;
    bool readIPRegister32(UInt16 hwId, UInt8 instance, UInt8 segment,
                          UInt32 registerDword, UInt32 *value) const;
    bool writeIPRegister32(UInt16 hwId, UInt8 instance, UInt8 segment,
                           UInt32 registerDword, UInt32 value) const;

    IOPCIDevice *m_pciDevice{nullptr};
    IOMemoryMap *m_registerMap{nullptr};
    IOLock *m_vramAccessLock{nullptr};
    IOBufferMemoryDescriptor *m_scanoutMemory{nullptr};
    bool m_scanoutPrepared{false};
    VanGoghIPDiscovery::Table *m_ipDiscovery{nullptr};
    IOPhysicalAddress m_registerPhysicalAddress{0};
    IOPhysicalLength m_registerLength{0};
    bool m_memoryEnableWasSet{false};
    bool m_memoryEnableChanged{false};
    bool m_providerOpened{false};
    UInt32 m_rotationRestoreMask{0};
    UInt8 m_panelOTG{0xff};
    UInt32 m_panelOPPMask{0};
    UInt32 m_panelHUBPMask{0};
    UInt32 m_rotationOriginalConfig[VanGoghDCN301::kHUBPCount]{};
    UInt32 m_rotationProgrammedConfig[VanGoghDCN301::kHUBPCount]{};
};

OSDefineMetaClassAndStructors(VanGoghDriver, IOService)

bool VanGoghDriver::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }

    m_pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (m_pciDevice == nullptr) {
        IOLog("VanGoghDriver: provider is not an IOPCIDevice\n");
        IOService::stop(provider);
        return false;
    }

    const UInt16 vendor = m_pciDevice->configRead16(kIOPCIConfigVendorID);
    const UInt16 device = m_pciDevice->configRead16(kIOPCIConfigDeviceID);
    if (vendor != 0x1002 || device != 0x163F) {
        IOLog("VanGoghDriver: refusing unexpected PCI device %04x:%04x\n", vendor, device);
        m_pciDevice = nullptr;
        IOService::stop(provider);
        return false;
    }

    if (!m_pciDevice->open(this)) {
        IOLog("VanGoghDriver: could not open PCI provider\n");
        m_pciDevice = nullptr;
        IOService::stop(provider);
        return false;
    }
    m_providerOpened = true;

    // BAR5 is the host's 512 KiB register aperture. Use the OS-published
    // descriptor and mapping; addresses are assigned at boot and are not fixed.
    const UInt32 bar5Register = kIOPCIConfigBaseAddress0 + 5 * sizeof(UInt32);
    IODeviceMemory *bar5 = m_pciDevice->getDeviceMemoryWithRegister(bar5Register);
    if (bar5 == nullptr || bar5->getLength() < sizeof(UInt32)) {
        IOLog("VanGoghDriver: PCI BAR5 register aperture is unavailable\n");
        releaseDeviceResources();
        IOService::stop(provider);
        return false;
    }

    m_registerPhysicalAddress = bar5->getPhysicalAddress();
    m_registerLength = bar5->getLength();
    // Preserve the PCI command state and restore it if this driver enabled
    // memory decoding solely to map the BAR.
    const UInt16 pciCommand = m_pciDevice->configRead16(kPciConfigCommandRegister);
    m_memoryEnableWasSet = (pciCommand & kPciCommandMemorySpaceEnable) != 0;
    m_pciDevice->setMemoryEnable(true);
    m_memoryEnableChanged = !m_memoryEnableWasSet;
    m_registerMap = m_pciDevice->mapDeviceMemoryWithRegister(bar5Register);
    if (m_registerMap == nullptr || m_registerMap->getLength() < m_registerLength) {
        IOLog("VanGoghDriver: could not map the complete PCI BAR5 register aperture\n");
        releaseDeviceResources();
        IOService::stop(provider);
        return false;
    }
    m_vramAccessLock = IOLockAlloc();
    if (m_vramAccessLock == nullptr) {
        IOLog("VanGoghDriver: could not allocate VRAM access lock\n");
        releaseDeviceResources();
        IOService::stop(provider);
        return false;
    }

    const UInt8 revision = m_pciDevice->configRead8(kIOPCIConfigRevisionID);
    IOLog("VanGoghDriver: attached to Van Gogh %04x:%04x rev %02x; BAR5 mapped at 0x%llx (%llu bytes)\n",
          vendor, device, revision, (unsigned long long)m_registerPhysicalAddress,
          (unsigned long long)m_registerLength);
    IOLog("VanGoghDriver: GPU initialization and display/compute services are not enabled yet\n");

    m_pciDevice->setProperty("model", "AMD 0405");
    const UInt32 vramMegabytes = 4096;
    OSData *vramProperty = OSData::withBytes(&vramMegabytes, sizeof(vramMegabytes));
    if (vramProperty != nullptr) {
        m_pciDevice->setProperty("VRAM,totalMB", vramProperty);
        vramProperty->release();
    }

    setProperty("model", "AMD Van Gogh Integrated Graphics");
    setProperty("VanGoghDriver,register-aperture-size",
                (UInt64)m_registerLength, 64);
    loadIPDiscovery(m_pciDevice);
    inspectVanGoghVMID0();
    inspectDCN301Rotation();
    inspectDCN301Timing();
    inspectDCN301PipeRouting();
    applyConfiguredDCN301Rotation(m_pciDevice);
    if (!allocatePanelScanout()) {
        IOLog("VanGoghDriver: no contiguous linear panel buffer; framebuffer remains disabled\n");
    }
    validatePSPFirmware(m_pciDevice);
    OSData *panelEdid = OSData::withBytes(kSteamDeckPanelEDID, sizeof(kSteamDeckPanelEDID));
    if (panelEdid != nullptr) {
        setProperty("VanGoghFramebuffer,edid", panelEdid);
        panelEdid->release();
    }
    registerService();
    return true;
}

// Capture MMHUB 2.3 VMID0's live page-table and aperture state. The registers
// are read-only here; VMID0 setup and GART ownership belong to GMC bring-up.
void VanGoghDriver::inspectVanGoghVMID0() {
    if (m_ipDiscovery == nullptr || m_registerMap == nullptr) return;
    VanGoghIPDiscovery::Entry mmhub{};
    if (!VanGoghIPDiscovery::find(m_ipDiscovery, 34, 0, &mmhub) ||
        mmhub.major != 2 || mmhub.minor != 3 || mmhub.revision != 0) {
        IOLog("VanGoghDriver: MMHUB is not the captured 2.3.0 block; skipping VMID0 readback\n");
        return;
    }
    constexpr UInt32 kRegisters[] = {
        0x0740, // MMVM_CONTEXT0_CNTL
        0x0940, 0x0941, // page-table base low/high
        0x0942, 0x0943, // logical page-table start low/high
        0x0944, 0x0945, // logical page-table end low/high
        0x08f1, 0x08f2  // system aperture low/high
    };
    UInt32 values[sizeof(kRegisters) / sizeof(kRegisters[0])]{};
    for (UInt32 i = 0; i < sizeof(kRegisters) / sizeof(kRegisters[0]); ++i) {
        if (!readIPRegister32(34, 0, 1, kRegisters[i], &values[i])) {
            IOLog("VanGoghDriver: MMHUB VMID0 register read failed at dword %x\n", kRegisters[i]);
            return;
        }
    }
    const bool enabled = (values[0] & 1u) != 0;
    setProperty("VanGoghGMC,VMID0-context-enabled", enabled);
    setProperty("VanGoghGMC,VMID0-page-table-base-low", values[1], 32);
    setProperty("VanGoghGMC,VMID0-page-table-base-high", values[2], 32);
    setProperty("VanGoghGMC,VMID0-context-control", values[0], 32);
    OSData *state = OSData::withBytes(values, sizeof(values));
    if (state != nullptr) {
        setProperty("VanGoghGMC,VMID0-state", state);
        state->release();
    }
    IOLog("VanGoghDriver: MMHUB2.3 VMID0 context %s control=%08x PT-base=%08x:%08x range=%08x:%08x-%08x:%08x aperture=%08x:%08x\n",
          enabled ? "enabled" : "disabled", values[0], values[2], values[1],
          values[4], values[3], values[6], values[5], values[8], values[7]);
}

// Read the MPC compositing chains and ODM input selection to identify the live
// eDP HUBP instances. Register addresses come from the DCN301 header: ODM
// segment 2, MPC segment 3.
void VanGoghDriver::inspectDCN301PipeRouting() {
    if (m_ipDiscovery == nullptr || m_registerMap == nullptr) return;
    constexpr UInt32 kODM0DataSourceSelect = 0x1acb;
    constexpr UInt32 kODMStride = 0x10;
    constexpr UInt32 kMPCCCount = 4;
    constexpr UInt32 kMPCCStride = 0x20;
    UInt32 odmSources[4]{};
    UInt32 mpccState[kMPCCCount][5]{};
    UInt32 odmValid = 0;
    UInt32 mpccValid = 0;
    m_panelOPPMask = 0;
    m_panelHUBPMask = 0;
    for (UInt8 odm = 0; odm < 4; ++odm) {
        if (readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                             VanGoghDCN301::kDisplayIPInstance, 2,
                             kODM0DataSourceSelect + odm * kODMStride,
                             &odmSources[odm]) && odmSources[odm] != UINT32_MAX) {
            odmValid |= 1u << odm;
            const UInt32 source = odmSources[odm];
            IOLog("VanGoghDriver: DCN301 ODM%u inputs=%u outputs=%u sources=%x,%x,%x,%x raw=%08x\n",
                  odm, (source & 0x3u) + 1u, ((source >> 2) & 0x3u) + 1u,
                  (source >> 8) & 0xfu, (source >> 12) & 0xfu,
                  (source >> 16) & 0xfu, (source >> 20) & 0xfu, source);
        }
    }
    if (m_panelOTG < 4 && (odmValid & (1u << m_panelOTG)) != 0) {
        const UInt32 source = odmSources[m_panelOTG];
        const UInt32 inputCount = (source & 0x3u) + 1u;
        constexpr UInt8 kSourceShifts[] = {8, 12, 16, 20};
        for (UInt32 input = 0; input < inputCount; ++input) {
            const UInt32 opp = (source >> kSourceShifts[input]) & 0xfu;
            if (opp < 4) m_panelOPPMask |= 1u << opp;
        }
    }
    for (UInt8 mpcc = 0; mpcc < kMPCCCount; ++mpcc) {
        bool valid = true;
        const UInt32 base = static_cast<UInt32>(mpcc) * kMPCCStride;
        constexpr UInt8 kMPCCRegisters[] = {0x00, 0x01, 0x02, 0x03, 0x0d};
        for (UInt8 reg = 0; reg < sizeof(kMPCCRegisters) / sizeof(kMPCCRegisters[0]); ++reg) {
            if (!readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                                  VanGoghDCN301::kDisplayIPInstance, 3,
                                  base + kMPCCRegisters[reg], &mpccState[mpcc][reg])) {
                valid = false;
                break;
            }
        }
        if (valid) {
            mpccValid |= 1u << mpcc;
            const UInt32 status = mpccState[mpcc][4];
            const UInt32 dpp = mpccState[mpcc][0] & 0xfu;
            const UInt32 opp = mpccState[mpcc][2] & 0xfu;
            const bool disabled = (status & 0x4u) != 0;
            if (opp < 4 && (m_panelOPPMask & (1u << opp)) != 0 &&
                !disabled && dpp < 4)
                m_panelHUBPMask |= 1u << dpp; // DCN301 resource pool pairs DPP/HUBP by instance.
            IOLog("VanGoghDriver: DCN301 MPCC%u top=%x bottom=%x opp=%x control=%08x status=%08x\n",
                  mpcc, mpccState[mpcc][0] & 0xfu, mpccState[mpcc][1] & 0xfu,
                  mpccState[mpcc][2] & 0xfu, mpccState[mpcc][3], status);
        }
    }
    setProperty("VanGoghDCN301,ODM-valid-mask", odmValid, 32);
    setProperty("VanGoghDCN301,MPCC-valid-mask", mpccValid, 32);
    setProperty("VanGoghDCN301,panel-OTG", m_panelOTG, 8);
    setProperty("VanGoghDCN301,panel-OPP-mask", m_panelOPPMask, 32);
    setProperty("VanGoghDCN301,panel-HUBP-mask", m_panelHUBPMask, 32);
    OSData *odmData = OSData::withBytes(odmSources, sizeof(odmSources));
    if (odmData != nullptr) {
        setProperty("VanGoghDCN301,ODM-data-sources", odmData);
        odmData->release();
    }
    OSData *mpccData = OSData::withBytes(mpccState, sizeof(mpccState));
    if (mpccData != nullptr) {
        setProperty("VanGoghDCN301,MPCC-routing", mpccData);
        mpccData->release();
    }
    IOLog("VanGoghDriver: DCN301 pipe-routing readback ODM mask %x, MPCC mask %x\n",
          odmValid, mpccValid);
    IOLog("VanGoghDriver: inferred internal-panel OTG %u HUBP mask %x\n",
          m_panelOTG, m_panelHUBPMask);
    IOLog("VanGoghDriver: internal-panel OPP mask %x from ODM source selection\n",
          m_panelOPPMask);
}

bool VanGoghDriver::allocatePanelScanout() {
    if (m_scanoutMemory != nullptr) return true;
    // This is a plain ARGB source surface for a later HUBP mode-set. Allocate
    // enough storage at a page boundary; do not advertise it as scanout-ready
    // until its DCN address, format, pitch, and rotation have all been verified.
    m_scanoutMemory = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryPhysicallyContiguous | kIODirectionInOut,
        kPanelSourceBytes, PAGE_SIZE);
    if (m_scanoutMemory == nullptr || m_scanoutMemory->getLength() < kPanelSourceBytes) {
        if (m_scanoutMemory != nullptr) {
            m_scanoutMemory->release();
            m_scanoutMemory = nullptr;
        }
        return false;
    }
    if (m_scanoutMemory->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        m_scanoutMemory->release();
        m_scanoutMemory = nullptr;
        IOLog("VanGoghDriver: could not prepare panel buffer for device access\n");
        return false;
    }
    m_scanoutPrepared = true;
    IOByteCount segmentLength = 0;
    const IOPhysicalAddress physical = m_scanoutMemory->getPhysicalSegment(0, &segmentLength);
    if (physical == 0 || segmentLength < kPanelSourceBytes ||
        physical > UINT64_MAX - kPanelSourceBytes) {
        if (m_scanoutPrepared) m_scanoutMemory->complete(kIODirectionInOut);
        m_scanoutPrepared = false;
        m_scanoutMemory->release();
        m_scanoutMemory = nullptr;
        IOLog("VanGoghDriver: contiguous panel buffer has no single usable physical segment\n");
        return false;
    }
    void *bytes = m_scanoutMemory->getBytesNoCopy();
    if (bytes == nullptr) {
        if (m_scanoutPrepared) m_scanoutMemory->complete(kIODirectionInOut);
        m_scanoutPrepared = false;
        m_scanoutMemory->release();
        m_scanoutMemory = nullptr;
        return false;
    }
    bzero(bytes, kPanelSourceBytes);
    setProperty("VanGoghFramebuffer,scanout-physical", static_cast<UInt64>(physical), 64);
    setProperty("VanGoghFramebuffer,scanout-bytes", static_cast<UInt64>(kPanelSourceBytes), 64);
    setProperty("VanGoghFramebuffer,width", kPanelSourceWidth, 32);
    setProperty("VanGoghFramebuffer,height", kPanelSourceHeight, 32);
    setProperty("VanGoghFramebuffer,pitch", kPanelSourcePitch, 32);
    setProperty("VanGoghFramebuffer,refresh", 60, 32);
    setProperty("VanGoghFramebuffer,linear-scanout", true);
    setProperty("VanGoghFramebuffer,gpu-address-mapped", false);
    setProperty("VanGoghFramebuffer,surface-programmed", false);
    setProperty("VanGoghDriver,gpuvm-scanout-ready", false);
    setProperty("VanGoghDriver,scanout-buffer-allocated", true);
    IOLog("VanGoghDriver: allocated %ux%u linear ARGB source buffer at physical 0x%llx (%llu bytes); no GPUVM mapping, DCN not programmed\n",
          kPanelSourceWidth, kPanelSourceHeight,
          static_cast<unsigned long long>(physical),
          static_cast<unsigned long long>(kPanelSourceBytes));
    return true;
}

// DCN 3.0.1 OTG register layout from AMD's dcn_3_0_1_offset.h. This stage is
// read-only: capture live timing state before any future mode-set implementation
// is allowed to touch a running eDP link.
void VanGoghDriver::inspectDCN301Timing() {
    if (m_ipDiscovery == nullptr || m_registerMap == nullptr) return;
    constexpr UInt32 kOTG0HTotal = 0x1b2a;
    constexpr UInt32 kOTGInstanceStride = 0x80;
    constexpr UInt32 kTimingRegisters[] = {
        0x00, // H_TOTAL
        0x01, // H_BLANK_START_END
        0x02, // H_SYNC_A
        0x05, // V_TOTAL
        0x0c, // V_BLANK_START_END
        0x0d, // V_SYNC_A
        0x0e, // V_SYNC_A_CNTL
        0x17, // OTG_CONTROL
    };
    UInt32 activeMask = 0;
    UInt32 panelTimingMask = 0;
    UInt32 packedTotals[4]{};
    UInt32 controlValues[4]{};
    m_panelOTG = 0xff;
    for (UInt8 otg = 0; otg < 4; ++otg) {
        UInt32 values[sizeof(kTimingRegisters) / sizeof(kTimingRegisters[0])]{};
        bool valid = true;
        const UInt32 base = kOTG0HTotal + static_cast<UInt32>(otg) * kOTGInstanceStride;
        for (UInt32 i = 0; i < sizeof(kTimingRegisters) / sizeof(kTimingRegisters[0]); ++i) {
            if (!readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                                  VanGoghDCN301::kDisplayIPInstance,
                                  VanGoghDCN301::kRegisterBaseSegment,
                                  base + kTimingRegisters[i], &values[i])) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        const UInt32 control = values[7];
        if ((control & 1u) == 0 || values[0] == 0 || values[3] == 0 ||
            values[0] == 0xffffffffu || values[3] == 0xffffffffu) continue;
        activeMask |= 1u << otg;
        // Van Gogh's captured internal panel is 800x1280 with register totals
        // 871x1323 (the reported mode totals include the hardware +1 encoding).
        if ((values[0] & 0xffffu) == 871u && (values[3] & 0xffffu) == 1323u)
            panelTimingMask |= 1u << otg;
        packedTotals[otg] = (values[0] & 0xffffu) | ((values[3] & 0xffffu) << 16);
        controlValues[otg] = control;
        IOLog("VanGoghDriver: DCN301 OTG%u active totals %ux%u blank H=%08x V=%08x sync H=%08x V=%08x polarity=%08x control=%08x\n",
              otg, values[0] & 0xffffu, values[3] & 0xffffu,
              values[1], values[4], values[2], values[5], values[6], control);
    }
    if (panelTimingMask != 0 && (panelTimingMask & (panelTimingMask - 1u)) == 0) {
        for (UInt8 otg = 0; otg < 4; ++otg) {
            if ((panelTimingMask & (1u << otg)) != 0) m_panelOTG = otg;
        }
    }
    setProperty("VanGoghDCN301,OTG-active-mask", activeMask, 32);
    setProperty("VanGoghDCN301,panel-timing-candidate-mask", panelTimingMask, 32);
    OSData *totals = OSData::withBytes(packedTotals, sizeof(packedTotals));
    if (totals != nullptr) {
        setProperty("VanGoghDCN301,OTG-totals", totals);
        totals->release();
    }
    OSData *controls = OSData::withBytes(controlValues, sizeof(controlValues));
    if (controls != nullptr) {
        setProperty("VanGoghDCN301,OTG-controls", controls);
        controls->release();
    }
}

void VanGoghDriver::applyConfiguredDCN301Rotation(IOService *provider) {
    OSNumber *request = provider
        ? OSDynamicCast(OSNumber, provider->getProperty("VanGoghDriver,program-hubp-rotation"))
        : nullptr;
    if (request != nullptr) {
        const UInt64 encoded = request->unsigned64BitValue();
        const UInt8 hubp = static_cast<UInt8>((encoded >> 8) & 0xffu);
        const UInt8 angle = static_cast<UInt8>(encoded & 0xffu);
        if ((encoded & ~0xffffull) != 0 || hubp >= VanGoghDCN301::kHUBPCount || angle > 3) {
            IOLog("VanGoghDriver: invalid configured HUBP rotation request 0x%llx\n",
                  static_cast<unsigned long long>(encoded));
            return;
        }
        if (programDCN301Rotation(hubp, angle)) {
            setProperty("VanGoghFramebuffer,rotation-programmed", angle == 3);
            inspectDCN301Rotation();
        }
        return;
    }

    OSNumber *mask = provider
        ? OSDynamicCast(OSNumber, provider->getProperty("VanGoghDriver,program-hubp-rotation-mask"))
        : nullptr;
    OSNumber *angleProperty = provider
        ? OSDynamicCast(OSNumber, provider->getProperty("VanGoghDriver,program-hubp-rotation-angle"))
        : nullptr;
    if (mask == nullptr) mask = OSDynamicCast(OSNumber, getProperty("VanGoghDriver,program-hubp-rotation-mask"));
    if (angleProperty == nullptr)
        angleProperty = OSDynamicCast(OSNumber, getProperty("VanGoghDriver,program-hubp-rotation-angle"));
    UInt32 requestedMask = 0;
    UInt32 requestedAngle = 3;
    if (mask != nullptr || angleProperty != nullptr) {
        if (mask == nullptr || angleProperty == nullptr ||
            (mask->unsigned32BitValue() & ~0x0fu) != 0 ||
            angleProperty->unsigned32BitValue() > 3) {
            IOLog("VanGoghDriver: invalid configured HUBP rotation mask/angle\n");
            return;
        }
        requestedMask = mask->unsigned32BitValue();
        requestedAngle = angleProperty->unsigned32BitValue();
    } else {
        requestedMask = m_panelHUBPMask;
    }
    if (requestedMask == 0) {
        IOLog("VanGoghDriver: no HUBP surfaces were linked to the active internal-panel OPP; skipping rotation\n");
        return;
    }
    const UInt8 angle = static_cast<UInt8>(requestedAngle);
    UInt32 programmedMask = 0;
    for (UInt8 hubp = 0; hubp < VanGoghDCN301::kHUBPCount; ++hubp) {
        if ((requestedMask & (1u << hubp)) != 0 && programDCN301Rotation(hubp, angle))
            programmedMask |= 1u << hubp;
    }
    if (programmedMask != requestedMask) {
        for (UInt8 hubp = 0; hubp < VanGoghDCN301::kHUBPCount; ++hubp) {
            if ((programmedMask & (1u << hubp)) == 0) continue;
            UInt32 registerDword = 0, current = 0;
            if (VanGoghDCN301::surfaceConfigRegisterDword(hubp, &registerDword) &&
                readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                                 VanGoghDCN301::kDisplayIPInstance,
                                 VanGoghDCN301::kRegisterBaseSegment,
                                 registerDword, &current) &&
                current == m_rotationProgrammedConfig[hubp]) {
                writeIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                                  VanGoghDCN301::kDisplayIPInstance,
                                  VanGoghDCN301::kRegisterBaseSegment,
                                  registerDword, m_rotationOriginalConfig[hubp]);
            }
            m_rotationRestoreMask &= ~(1u << hubp);
        }
        IOLog("VanGoghDriver: split HUBP rotation request incomplete (requested %x, programmed %x); rolled back\n",
              requestedMask, programmedMask);
        return;
    }
    if (programmedMask == requestedMask) {
        setProperty("VanGoghFramebuffer,rotation-programmed", angle == 3);
        inspectDCN301Rotation();
    }
}

bool VanGoghDriver::programDCN301Rotation(UInt8 hubp, UInt8 angle) {
    if (hubp >= VanGoghDCN301::kHUBPCount || angle > 3 || m_ipDiscovery == nullptr)
        return false;
    UInt32 registerDword = 0, original = 0;
    if (!VanGoghDCN301::surfaceConfigRegisterDword(hubp, &registerDword) ||
        !readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                          VanGoghDCN301::kDisplayIPInstance,
                          VanGoghDCN301::kRegisterBaseSegment,
                          registerDword, &original) || original == UINT32_MAX ||
        VanGoghDCN301::pixelFormatFromSurfaceConfig(original) == 0) {
        IOLog("VanGoghDriver: refusing rotation request for HUBP%u without a populated surface\n", hubp);
        return false;
    }

    const UInt32 programmed = (original & ~0x300u) | (static_cast<UInt32>(angle) << 8);
    if (!writeIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                           VanGoghDCN301::kDisplayIPInstance,
                           VanGoghDCN301::kRegisterBaseSegment,
                           registerDword, programmed)) {
        IOLog("VanGoghDriver: failed writing HUBP%u rotation field\n", hubp);
        return false;
    }
    OSSynchronizeIO();

    UInt32 verify = 0;
    if (!readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                          VanGoghDCN301::kDisplayIPInstance,
                          VanGoghDCN301::kRegisterBaseSegment,
                          registerDword, &verify) || verify != programmed) {
        writeIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                          VanGoghDCN301::kDisplayIPInstance,
                          VanGoghDCN301::kRegisterBaseSegment,
                          registerDword, original);
        IOLog("VanGoghDriver: HUBP%u rotation readback mismatch; restored previous surface config\n", hubp);
        return false;
    }

    m_rotationOriginalConfig[hubp] = original;
    m_rotationProgrammedConfig[hubp] = programmed;
    m_rotationRestoreMask |= 1u << hubp;
    setProperty("VanGoghDCN301,HUBP-rotation-programmed", angle, 8);
    IOLog("VanGoghDriver: programmed and verified HUBP%u rotation angle %u\n", hubp, angle);
    return true;
}

void VanGoghDriver::inspectDCN301Rotation() {
    if (m_ipDiscovery == nullptr) return;

    VanGoghIPDiscovery::Entry dcn{};
    if (!VanGoghIPDiscovery::find(m_ipDiscovery,
                                  VanGoghDCN301::kDisplayIPHardwareId,
                                  VanGoghDCN301::kDisplayIPInstance, &dcn) ||
        dcn.major != 3 || dcn.minor != 0 || dcn.revision != 1) {
        IOLog("VanGoghDriver: no matching DCN 3.0.1 discovery entry; skipping HUBP readback\n");
        return;
    }

    VanGoghDCN301::SurfaceState surfaces[VanGoghDCN301::kHUBPCount]{};
    UInt32 rotationValues = 0;
    UInt32 validMask = 0;
    UInt32 linearUncompressedMask = 0;
    UInt32 vmidValues = 0;
    UInt32 hubpControlValues[VanGoghDCN301::kHUBPCount]{};
    for (UInt8 hubp = 0; hubp < VanGoghDCN301::kHUBPCount; ++hubp) {
        UInt32 registerDword = 0;
        if (VanGoghDCN301::registerDword(hubp, VanGoghDCN301::Register::SurfaceConfig,
                                         &registerDword) &&
            readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                             VanGoghDCN301::kDisplayIPInstance,
                             VanGoghDCN301::kRegisterBaseSegment,
                             registerDword, &surfaces[hubp].surfaceConfig)) {
            surfaces[hubp].valid |= 1u << 0;
        }
        if ((surfaces[hubp].valid & 1u) == 0 ||
            VanGoghDCN301::pixelFormatFromSurfaceConfig(surfaces[hubp].surfaceConfig) == 0 ||
            surfaces[hubp].surfaceConfig == UINT32_MAX) {
            continue;
        }

        validMask |= 1u << hubp;
        const UInt8 rotation = VanGoghDCN301::rotationFromSurfaceConfig(
            surfaces[hubp].surfaceConfig);
        rotationValues |= static_cast<UInt32>(rotation) << (hubp * 2);
        for (UInt8 reg = static_cast<UInt8>(VanGoghDCN301::Register::TilingConfig);
             reg <= static_cast<UInt8>(VanGoghDCN301::Register::HubpControl); ++reg) {
            UInt32 value = 0;
            const VanGoghDCN301::Register which = static_cast<VanGoghDCN301::Register>(reg);
            if (!VanGoghDCN301::registerDword(hubp, which, &registerDword) ||
                !readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                                  VanGoghDCN301::kDisplayIPInstance,
                                  VanGoghDCN301::kRegisterBaseSegment,
                                  registerDword, &value)) {
                continue;
            }
            switch (which) {
                case VanGoghDCN301::Register::TilingConfig:
                    surfaces[hubp].tilingConfig = value; surfaces[hubp].valid |= 1u << 1; break;
                case VanGoghDCN301::Register::ViewportDimension:
                    surfaces[hubp].viewportDimension = value; surfaces[hubp].valid |= 1u << 2; break;
                case VanGoghDCN301::Register::SurfacePitch:
                    surfaces[hubp].surfacePitch = value; surfaces[hubp].valid |= 1u << 3; break;
                case VanGoghDCN301::Register::SurfaceAddressLow:
                    surfaces[hubp].surfaceAddressLow = value; surfaces[hubp].valid |= 1u << 4; break;
                case VanGoghDCN301::Register::SurfaceAddressHigh:
                    surfaces[hubp].surfaceAddressHigh = value; surfaces[hubp].valid |= 1u << 5; break;
                case VanGoghDCN301::Register::SurfaceControl:
                    surfaces[hubp].surfaceControl = value; surfaces[hubp].valid |= 1u << 6; break;
                case VanGoghDCN301::Register::VmidSettings:
                    surfaces[hubp].vmidSettings = value; surfaces[hubp].valid |= 1u << 7; break;
                case VanGoghDCN301::Register::HubpControl:
                    surfaces[hubp].hubpControl = value; surfaces[hubp].valid |= 1u << 8; break;
                case VanGoghDCN301::Register::SurfaceConfig:
                    break;
            }
        }
        if (VanGoghDCN301::linearUncompressedLayout(&surfaces[hubp])) {
            linearUncompressedMask |= 1u << hubp;
        }
        if ((surfaces[hubp].valid & (1u << 7)) != 0) {
            vmidValues |= (surfaces[hubp].vmidSettings & 0xfu) << (hubp * 4);
        }
        if ((surfaces[hubp].valid & (1u << 8)) != 0) {
            hubpControlValues[hubp] = surfaces[hubp].hubpControl;
        }
        if ((surfaces[hubp].valid & 0x1FFu) == 0x1FFu) {
            IOLog("VanGoghDriver: HUBP%u format %u rot %u swizzle %u DCC %u pitch %u viewport %ux%u address-reg %08x:%08x VMID %u hubp-control %08x\n",
                  hubp,
                  VanGoghDCN301::pixelFormatFromSurfaceConfig(surfaces[hubp].surfaceConfig),
                  rotation,
                  VanGoghDCN301::swizzleModeFromTilingConfig(surfaces[hubp].tilingConfig),
                  VanGoghDCN301::dccEnabled(surfaces[hubp].surfaceControl),
                  VanGoghDCN301::pitchPixelsFromSurfacePitch(surfaces[hubp].surfacePitch),
                  VanGoghDCN301::viewportWidth(surfaces[hubp].viewportDimension),
                  VanGoghDCN301::viewportHeight(surfaces[hubp].viewportDimension),
                  surfaces[hubp].surfaceAddressHigh, surfaces[hubp].surfaceAddressLow,
                  surfaces[hubp].vmidSettings & 0xfu, surfaces[hubp].hubpControl);
        } else {
            IOLog("VanGoghDriver: HUBP%u surface register read incomplete (mask %02x)\n",
                  hubp, surfaces[hubp].valid);
        }
    }

    if (validMask == 0) {
        IOLog("VanGoghDriver: DCN 3.0.1 HUBP surface state unavailable in BAR5\n");
        return;
    }
    setProperty("VanGoghDCN301,HUBP-rotation-valid-mask",
                static_cast<UInt64>(validMask), 32);
    setProperty("VanGoghDCN301,HUBP-rotation-values",
                static_cast<UInt64>(rotationValues), 32);
    setProperty("VanGoghDCN301,HUBP-linear-uncompressed-mask",
                static_cast<UInt64>(linearUncompressedMask), 32);
    setProperty("VanGoghDCN301,HUBP-VMID-values",
                static_cast<UInt64>(vmidValues), 32);
    OSData *hubpControlData = OSData::withBytes(hubpControlValues, sizeof(hubpControlValues));
    if (hubpControlData != nullptr) {
        setProperty("VanGoghDCN301,HUBP-control-values", hubpControlData);
        hubpControlData->release();
    }
    OSData *surfaceData = OSData::withBytes(surfaces, sizeof(surfaces));
    if (surfaceData != nullptr) {
        setProperty("VanGoghDCN301,HUBP-surface-state", surfaceData);
        surfaceData->release();
    }
    IOLog("VanGoghDriver: DCN301 HUBP readback mask %x rotation %08x linear/uncompressed %x\n",
          validMask, rotationValues, linearUncompressedMask);
}

void VanGoghDriver::validatePSPFirmware(IOService *provider) {
    OSData *asd = provider
        ? OSDynamicCast(OSData, provider->getProperty("VanGoghDriver,psp-asd"))
        : nullptr;
    OSData *toc = provider
        ? OSDynamicCast(OSData, provider->getProperty("VanGoghDriver,psp-toc"))
        : nullptr;
    if (asd == nullptr && toc == nullptr) return;
    if (asd == nullptr || toc == nullptr) {
        IOLog("VanGoghDriver: PSP ASD/TOC validation requires both images; no firmware submitted\n");
        setProperty("VanGoghDriver,psp-firmware-validated", false);
        return;
    }

    VanGoghFirmware::ImageInfo asdInfo{}, tocInfo{};
    const bool valid = VanGoghFirmware::parseImage(
                           static_cast<const UInt8 *>(asd->getBytesNoCopy()), asd->getLength(),
                           VanGoghFirmware::ImageKind::ASD, &asdInfo) &&
                       VanGoghFirmware::parseImage(
                           static_cast<const UInt8 *>(toc->getBytesNoCopy()), toc->getLength(),
                           VanGoghFirmware::ImageKind::TOC, &tocInfo);
    setProperty("VanGoghDriver,psp-firmware-validated", valid);
    if (!valid) {
        IOLog("VanGoghDriver: PSP ASD/TOC container validation failed; no firmware submitted\n");
        return;
    }

    setProperty("VanGoghDriver,psp-asd-version", asdInfo.firmwareVersion, 32);
    setProperty("VanGoghDriver,psp-toc-version", tocInfo.firmwareVersion, 32);
    IOLog("VanGoghDriver: validated PSP ASD and TOC containers (ASD %08x, TOC %08x); no firmware submitted\n",
          asdInfo.firmwareVersion, tocInfo.firmwareVersion);
}

void VanGoghDriver::stop(IOService *provider) {
    releaseDeviceResources();
    IOService::stop(provider);
}

void VanGoghDriver::releaseDeviceResources() {
    if (m_rotationRestoreMask != 0 && m_ipDiscovery != nullptr && m_registerMap != nullptr) {
        for (UInt8 hubp = 0; hubp < VanGoghDCN301::kHUBPCount; ++hubp) {
            if ((m_rotationRestoreMask & (1u << hubp)) == 0) continue;
            UInt32 registerDword = 0, current = 0;
            if (VanGoghDCN301::surfaceConfigRegisterDword(hubp, &registerDword) &&
                readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                                 VanGoghDCN301::kDisplayIPInstance,
                                 VanGoghDCN301::kRegisterBaseSegment,
                                 registerDword, &current) &&
                current == m_rotationProgrammedConfig[hubp]) {
                writeIPRegister32(VanGoghDCN301::kDisplayIPHardwareId,
                                  VanGoghDCN301::kDisplayIPInstance,
                                  VanGoghDCN301::kRegisterBaseSegment,
                                  registerDword, m_rotationOriginalConfig[hubp]);
            }
        }
        m_rotationRestoreMask = 0;
    }
    if (m_scanoutMemory != nullptr) {
        if (m_scanoutPrepared) m_scanoutMemory->complete(kIODirectionInOut);
        m_scanoutPrepared = false;
        m_scanoutMemory->release();
        m_scanoutMemory = nullptr;
    }
    if (m_ipDiscovery != nullptr) {
        IOFree(m_ipDiscovery, sizeof(*m_ipDiscovery));
        m_ipDiscovery = nullptr;
    }
    if (m_vramAccessLock != nullptr) {
        IOLockFree(m_vramAccessLock);
        m_vramAccessLock = nullptr;
    }
    if (m_registerMap != nullptr) {
        m_registerMap->release();
        m_registerMap = nullptr;
    }
    m_registerLength = 0;
    m_registerPhysicalAddress = 0;

    if (m_memoryEnableChanged && m_pciDevice != nullptr) {
        m_pciDevice->setMemoryEnable(m_memoryEnableWasSet);
    }
    m_memoryEnableChanged = false;
    m_memoryEnableWasSet = false;

    if (m_providerOpened && m_pciDevice != nullptr) {
        m_pciDevice->close(this);
    }
    m_providerOpened = false;
    m_pciDevice = nullptr;
}

void VanGoghDriver::loadIPDiscovery(IOService *provider) {
    OSData *blob = provider
        ? OSDynamicCast(OSData, provider->getProperty("VanGoghDriver,ip-discovery"))
        : nullptr;
    if (blob != nullptr && blob->getLength() != 0) {
        if (parseIPDiscovery(static_cast<const UInt8 *>(blob->getBytesNoCopy()), blob->getLength())) {
            return;
        }
        IOLog("VanGoghDriver: supplied IP discovery blob rejected; trying live TMR\n");
    }

    UInt32 ready = 0;
    for (UInt32 i = 0; i < 2000; ++i) {
        if (!readRegister32(kMp0C2Pmsg33Register, &ready)) break;
        if (ready & 0x80000000u) break;
        IOSleep(1);
    }
    if ((ready & 0x80000000u) == 0) {
        IOLog("VanGoghDriver: PSP IFWI-ready bit not set; skipping TMR discovery read\n");
        return;
    }

    UInt32 vramMegabytes = 0;
    if (!readRegister32(kRccConfigMemsizeRegister, &vramMegabytes) ||
        vramMegabytes == 0 || vramMegabytes == 0xffffffffu ||
        static_cast<UInt64>(vramMegabytes) * 1024 * 1024 <= kDiscoveryTmrOffset) {
        IOLog("VanGoghDriver: RCC_CONFIG_MEMSIZE is invalid; skipping TMR discovery read\n");
        return;
    }

    const UInt64 vramBytes = static_cast<UInt64>(vramMegabytes) * 1024 * 1024;
    UInt64 tmrOffset = vramBytes - kDiscoveryTmrOffset;
    UInt32 signature = 0;
    if (!readVramDwords(tmrOffset, &signature, sizeof(signature))) return;
    UInt32 tmrSize = kDiscoveryTmrSize;

    // Linux uses DRIVER_SCRATCH_0..2 when the default top-of-VRAM TMR has no
    // discovery signature. These are read only; the GPU is not reconfigured.
    if (signature != kBinarySignature) {
        UInt32 offsetLow = 0, offsetHigh = 0, candidateSize = 0;
        if (!readRegister32(kDriverScratch0Register, &offsetLow) ||
            !readRegister32(kDriverScratch1Register, &offsetHigh) ||
            !readRegister32(kDriverScratch2Register, &candidateSize) ||
            candidateSize < 64 || candidateSize > kMaxDiscoverySize ||
            (candidateSize & 3) != 0) {
            IOLog("VanGoghDriver: no valid IP discovery signature in default TMR\n");
            return;
        }
        tmrOffset = (static_cast<UInt64>(offsetHigh) << 32) | offsetLow;
        tmrSize = candidateSize;
        if (tmrOffset > vramBytes || tmrSize > vramBytes - tmrOffset) {
            IOLog("VanGoghDriver: scratch TMR range exceeds reported VRAM\n");
            return;
        }
        if (!readVramDwords(tmrOffset, &signature, sizeof(signature)) ||
            signature != kBinarySignature) {
            IOLog("VanGoghDriver: scratch TMR pointer has no IP discovery signature\n");
            return;
        }
    }

    UInt8 *buffer = static_cast<UInt8 *>(IOMalloc(tmrSize));
    if (buffer == nullptr) {
        IOLog("VanGoghDriver: no memory for %u-byte IP discovery TMR copy\n", tmrSize);
        return;
    }
    const bool copied = readVramDwords(tmrOffset, reinterpret_cast<UInt32 *>(buffer), tmrSize);
    const bool parsed = copied && parseIPDiscovery(buffer, tmrSize);
    IOFree(buffer, tmrSize);
    if (!parsed) {
        IOLog("VanGoghDriver: live IP discovery TMR failed validation\n");
        return;
    }
    IOLog("VanGoghDriver: copied and validated IP discovery from VRAM offset 0x%llx\n",
          static_cast<unsigned long long>(tmrOffset));
}

bool VanGoghDriver::parseIPDiscovery(const UInt8 *data, IOByteCount length) {
    if (m_ipDiscovery != nullptr) {
        IOFree(m_ipDiscovery, sizeof(*m_ipDiscovery));
        m_ipDiscovery = nullptr;
    }
    m_ipDiscovery = static_cast<VanGoghIPDiscovery::Table *>(IOMallocZero(sizeof(*m_ipDiscovery)));
    if (m_ipDiscovery == nullptr || !VanGoghIPDiscovery::parse(data, length, m_ipDiscovery)) {
        IOLog("VanGoghDriver: malformed IP discovery blob\n");
        if (m_ipDiscovery != nullptr) {
            IOFree(m_ipDiscovery, sizeof(*m_ipDiscovery));
            m_ipDiscovery = nullptr;
        }
        return false;
    }

    VanGoghIPDiscovery::Entry gc{}, sdma{}, vcn{}, psp{}, dmu{};
    const bool expectedVanGogh =
        VanGoghIPDiscovery::find(m_ipDiscovery, 11, 0, &gc) &&
        VanGoghIPDiscovery::find(m_ipDiscovery, 42, 0, &sdma) &&
        VanGoghIPDiscovery::find(m_ipDiscovery, 12, 0, &vcn) &&
        VanGoghIPDiscovery::find(m_ipDiscovery, 255, 0, &psp) &&
        VanGoghIPDiscovery::find(m_ipDiscovery, 271, 0, &dmu) &&
        gc.major == 10 && gc.minor == 3 && gc.revision == 1 &&
        sdma.major == 5 && sdma.minor == 2 && sdma.revision == 1 &&
        vcn.major == 3 && vcn.minor == 0 && vcn.revision == 2 &&
        psp.major == 11 && psp.minor == 5 && psp.revision == 0 &&
        dmu.major == 3 && dmu.minor == 0 && dmu.revision == 1;
    if (!expectedVanGogh) {
        IOLog("VanGoghDriver: valid discovery blob does not match captured Van Gogh IP versions\n");
        IOFree(m_ipDiscovery, sizeof(*m_ipDiscovery));
        m_ipDiscovery = nullptr;
        return false;
    }

    setProperty("VanGoghDriver,ip-discovery-table-version",
                static_cast<UInt64>(m_ipDiscovery->ipTableVersion), 16);
    setProperty("VanGoghDriver,ip-discovery-entry-count",
                static_cast<UInt64>(m_ipDiscovery->entryCount), 16);
    IOLog("VanGoghDriver: validated Van Gogh IP discovery table v%u (%u entries)\n",
          m_ipDiscovery->ipTableVersion, m_ipDiscovery->entryCount);
    return true;
}

bool VanGoghDriver::readRegister32(IOByteCount byteOffset, UInt32 *value) const {
    if (value == nullptr || m_registerMap == nullptr || (byteOffset & 3) != 0 ||
        byteOffset > m_registerLength || sizeof(UInt32) > m_registerLength - byteOffset) {
        return false;
    }

    const volatile UInt32 *registers =
        reinterpret_cast<const volatile UInt32 *>(m_registerMap->getVirtualAddress());
    *value = registers[byteOffset / sizeof(UInt32)];
    OSSynchronizeIO();
    return true;
}

bool VanGoghDriver::writeRegister32(IOByteCount byteOffset, UInt32 value) const {
    if (m_registerMap == nullptr || (byteOffset & 3) != 0 ||
        byteOffset > m_registerLength || sizeof(UInt32) > m_registerLength - byteOffset) {
        return false;
    }
    volatile UInt32 *registers =
        reinterpret_cast<volatile UInt32 *>(m_registerMap->getVirtualAddress());
    registers[byteOffset / sizeof(UInt32)] = value;
    OSSynchronizeIO();
    return true;
}

bool VanGoghDriver::readVramDwords(UInt64 offset, UInt32 *data, UInt32 byteCount) {
    if (data == nullptr || byteCount == 0 || (offset & 3) != 0 || (byteCount & 3) != 0 ||
        offset > UINT64_MAX - byteCount || m_vramAccessLock == nullptr) {
        return false;
    }

    IOLockLock(m_vramAccessLock);
    UInt32 savedIndex = 0, savedIndexHigh = 0;
    const bool savedState = readRegister32(kMmIndexRegister, &savedIndex) &&
                            readRegister32(kMmIndexHighRegister, &savedIndexHigh);
    bool success = savedState;
    UInt32 cachedHigh = UINT32_MAX;
    for (UInt32 byteOffset = 0; success && byteOffset < byteCount; byteOffset += 4) {
        const UInt64 position = offset + byteOffset;
        const UInt32 indexHigh = static_cast<UInt32>(position >> 31);
        success = writeRegister32(kMmIndexRegister,
                                  static_cast<UInt32>(position) | 0x80000000u);
        if (success && indexHigh != cachedHigh) {
            success = writeRegister32(kMmIndexHighRegister, indexHigh);
            cachedHigh = indexHigh;
        }
        UInt32 value = 0;
        if (success) success = readRegister32(kMmDataRegister, &value);
        if (success) data[byteOffset / sizeof(UInt32)] = value;
    }
    const bool restoredHigh = savedState && writeRegister32(kMmIndexHighRegister, savedIndexHigh);
    const bool restoredIndex = savedState && writeRegister32(kMmIndexRegister, savedIndex);
    IOLockUnlock(m_vramAccessLock);
    return success && restoredHigh && restoredIndex;
}

bool VanGoghDriver::readIPRegister32(UInt16 hwId, UInt8 instance, UInt8 segment,
                                     UInt32 registerDword, UInt32 *value) const {
    if (m_ipDiscovery == nullptr || value == nullptr) return false;
    VanGoghIPDiscovery::Entry entry{};
    UInt32 byteOffset = 0;
    if (!VanGoghIPDiscovery::find(m_ipDiscovery, hwId, instance, &entry) ||
        !VanGoghIPDiscovery::registerByteOffset(
            &entry, segment, registerDword, m_registerLength, &byteOffset)) {
        return false;
    }
    return readRegister32(byteOffset, value);
}

bool VanGoghDriver::writeIPRegister32(UInt16 hwId, UInt8 instance, UInt8 segment,
                                      UInt32 registerDword, UInt32 value) const {
    if (m_ipDiscovery == nullptr) return false;
    VanGoghIPDiscovery::Entry entry{};
    UInt32 byteOffset = 0;
    if (!VanGoghIPDiscovery::find(m_ipDiscovery, hwId, instance, &entry) ||
        !VanGoghIPDiscovery::registerByteOffset(
            &entry, segment, registerDword, m_registerLength, &byteOffset)) {
        return false;
    }
    return writeRegister32(byteOffset, value);
}
