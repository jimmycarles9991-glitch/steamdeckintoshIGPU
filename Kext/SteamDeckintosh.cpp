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
#include "VanGoghMemoryPlan.h"
#include "VanGoghScanout.h"
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>

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
// MMHUB 2.3.0 (discovery HWID 34), base segment 1. Checked against Linux's
// mmhub_2_3_0_offset.h by tools/verify_dcn301_registers.py.
constexpr UInt16 kMmhubHwId = 34;
constexpr UInt8 kMmhubSegment = 1;
constexpr UInt32 kMcVmFbLocationBase = 0x08ec;
constexpr UInt32 kMcVmFbLocationTop = 0x08ed;
constexpr UInt32 kPanelSourceWidth = 1280;
constexpr UInt32 kPanelSourceHeight = 800;
constexpr UInt32 kPanelSourcePitch = kPanelSourceWidth * 4;
constexpr IOByteCount kPanelSourceBytes =
    static_cast<IOByteCount>(kPanelSourcePitch) * kPanelSourceHeight;
// macOS's unaccelerated WindowServer comfortably covers one 1280x800 panel from
// a 128 MiB aperture. The GPU really reports 4 GiB of shared system memory; this
// is the amount advertised to IOGraphics, not a hardware partition.
constexpr UInt32 kDefaultVramExposureMiB = 128;
}

class VanGoghDriver : public IOService {
    OSDeclareDefaultStructors(VanGoghDriver)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    void releaseDeviceResources();
    void inspectDCN301Rotation();
    void inspectDCN301Timing();
    void inspectDCN301PipeRouting();
    void inspectVanGoghVMID0();
    bool allocatePanelScanout();
    void publishVramCapacity(IOService *provider, UInt32 reportedMiB);
    bool readMemoryWindow(UInt32 *vramMiB, UInt64 *fbBase, UInt64 *fbTop,
                          IODeviceMemory **bar0);
    VanGoghScanout::LiveStatus readLiveSurface(UInt8 hubp,
                                               VanGoghScanout::LiveSurface *surface);
    bool adoptLiveScanout(IOService *provider);
    bool framebufferReady() const;
    // Opt-in VRAM scanout route (see README). Each stage needs its own
    // provider/personality property and nothing below runs by default.
    bool boolProperty(IOService *provider, const char *key) const;
    bool numberProperty(IOService *provider, const char *key, UInt64 *value) const;
    void planVramScanout(IOService *provider);
    bool writeVramPattern();
    void programVramScanout(IOService *provider);
    bool applyPlane(UInt8 hubp, const VanGoghScanout::PlaneImage &image, UInt32 flipControl);
    void restoreVramScanout(bool force);
    bool readHubpRegister(UInt8 hubp, VanGoghDCN301::Register reg, UInt32 *value) const;
    bool writeHubpRegister(UInt8 hubp, VanGoghDCN301::Register reg, UInt32 value) const;
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

    struct ScanoutRestore {
        VanGoghScanout::Baseline original;
        UInt32 originalAddressLow;
        UInt32 originalAddressHigh;
        VanGoghScanout::PlaneImage programmed;
    };
    IOMemoryMap *m_vramWindowMap{nullptr};
    IOPhysicalAddress m_bar0Physical{0};
    VanGoghMemoryPlan::Output m_vramPlan{};
    VanGoghScanout::Geometry m_geom{};
    bool m_vramPatternWritten{false};
    UInt32 m_scanoutRestoreMask{0};
    ScanoutRestore m_scanoutRestore[VanGoghDCN301::kHUBPCount]{};
    bool m_liveScanoutAdopted{false};
    UInt64 m_liveVramOffset{0};
    UInt64 m_liveApertureBytes{0};
    UInt64 m_exposedVramBytes{0};
};

OSDefineMetaClassAndStructors(VanGoghDriver, IOService)

IOService *VanGoghDriver::probe(IOService *provider, SInt32 *score) {
    IOService *matched = IOService::probe(provider, score);
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, matched);
    if (pci == nullptr) {
        IOLog("VanGoghDriver: probe rejected non-PCI provider\n");
        return nullptr;
    }

    const UInt16 vendor = pci->configRead16(kIOPCIConfigVendorID);
    const UInt16 device = pci->configRead16(kIOPCIConfigDeviceID);
    if (vendor != 0x1002 || device != 0x163F) {
        IOLog("VanGoghDriver: probe rejected PCI %04x:%04x\n", vendor, device);
        return nullptr;
    }
    if (score != nullptr) *score = 1000;
    IOLog("VanGoghDriver: probe matched Steam Deck Van Gogh %04x:%04x\n", vendor, device);
    return matched;
}

bool VanGoghDriver::start(IOService *provider) {
    IOLog("VanGoghDriver: start entered\n");
    if (!IOService::start(provider)) {
        IOLog("VanGoghDriver: IOService::start failed\n");
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

    // A zero-I/O path for confirming OpenCore injection, PCI matching, and
    // IOKit service publication before enabling any BAR or GPU access.
    if (boolProperty(m_pciDevice, "VanGoghDriver,probe-only")) {
        const UInt8 revision = m_pciDevice->configRead8(kIOPCIConfigRevisionID);
        setProperty("model", "AMD Van Gogh (Steam Deck) probe-only");
        setProperty("VanGoghDriver,PCI-vendor-id", vendor, 32);
        setProperty("VanGoghDriver,PCI-device-id", device, 32);
        setProperty("VanGoghDriver,revision-id", revision, 8);
        setProperty("VanGoghDriver,attached", true);
        setProperty("VanGoghDriver,probe-only", true);
        m_pciDevice->setProperty("model", "AMD Van Gogh (Steam Deck) probe-only");
        m_pciDevice->setProperty("VanGoghDriver,attached", true);
        m_pciDevice->setProperty("VanGoghDriver,probe-only", true);
        registerService();
        IOLog("VanGoghDriver: probe-only start succeeded for %04x:%04x rev %02x; no BAR/GPU access attempted\n",
              vendor, device, revision);
        return true;
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

    m_pciDevice->setProperty("model", "AMD Van Gogh (Steam Deck)");
    m_pciDevice->setProperty("VanGoghDriver,PCI-vendor-id", vendor, 32);
    m_pciDevice->setProperty("VanGoghDriver,PCI-device-id", device, 32);
    m_pciDevice->setProperty("VanGoghDriver,revision-id", revision, 8);
    UInt32 vramMegabytes = 0;
    const bool vramSizeValid =
        readRegister32(kRccConfigMemsizeRegister, &vramMegabytes) &&
        vramMegabytes > 0 && vramMegabytes <= VanGoghMemoryPlan::kMaxVramBytes / VanGoghMemoryPlan::kMiB;
    publishVramCapacity(m_pciDevice, vramSizeValid ? vramMegabytes : 0);
    if (vramSizeValid) {
        IOLog("VanGoghDriver: GPU AMD Van Gogh (Steam Deck), PCI %04x:%04x, %u MiB shared VRAM reported, %llu MiB exposed to IOGraphics\n",
              vendor, device, vramMegabytes,
              static_cast<unsigned long long>(m_exposedVramBytes >> 20));
    } else {
        IOLog("VanGoghDriver: RCC_CONFIG_MEMSIZE readback invalid; advertising only the requested IOGraphics aperture\n");
    }

    setProperty("model", "AMD Van Gogh Integrated Graphics");
    setProperty("VanGoghDriver,PCI-vendor-id", vendor, 32);
    setProperty("VanGoghDriver,PCI-device-id", device, 32);
    setProperty("VanGoghDriver,revision-id", revision, 8);
    if (vramSizeValid) setProperty("VanGoghDriver,VRAM-MiB", vramMegabytes, 32);
    setProperty("VanGoghDriver,register-aperture-size",
                (UInt64)m_registerLength, 64);
    setProperty("VanGoghDriver,attached", true);
    m_pciDevice->setProperty("VanGoghDriver,attached", true);
    loadIPDiscovery(m_pciDevice);
    inspectVanGoghVMID0();
    inspectDCN301Rotation();
    inspectDCN301Timing();
    inspectDCN301PipeRouting();
    applyConfiguredDCN301Rotation(m_pciDevice);
    if (boolProperty(m_pciDevice, "VanGoghDriver,vram-scanout")) {
        planVramScanout(m_pciDevice);
        programVramScanout(m_pciDevice);
    }
    // The default route: the display engine is already scanning a linear
    // firmware/OpenCore surface. Expose exactly that memory to IOGraphics so
    // WindowServer has a working, unaccelerated framebuffer without any DCN write.
    if (!framebufferReady()) {
        if (!adoptLiveScanout(m_pciDevice) &&
            !boolProperty(m_pciDevice, "VanGoghDriver,vram-scanout") &&
            !allocatePanelScanout()) {
            IOLog("VanGoghDriver: no usable live scanout and no contiguous linear panel buffer; framebuffer remains disabled\n");
        }
    }
    validatePSPFirmware(m_pciDevice);
    OSData *panelEdid = OSData::withBytes(kSteamDeckPanelEDID, sizeof(kSteamDeckPanelEDID));
    if (panelEdid != nullptr) {
        setProperty("VanGoghFramebuffer,edid", panelEdid);
        panelEdid->release();
    }
    registerService();
    IOLog("VanGoghDriver: start completed; PCI identity is published, framebuffer-ready=%d\n",
          framebufferReady() ? 1 : 0);
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

// Publish the VRAM amount that IOGraphics should believe it has. The hardware
// reports 4 GiB of shared LPDDR5; a 128 MiB window is enough for one panel and
// keeps the unaccelerated framebuffer away from firmware/TMR allocations.
void VanGoghDriver::publishVramCapacity(IOService *provider, UInt32 reportedMiB) {
    UInt64 requestedMiB = kDefaultVramExposureMiB;
    UInt64 configured = 0;
    if (numberProperty(provider, "VanGoghDriver,vram-mb", &configured) &&
        configured >= 16 && configured <= VanGoghMemoryPlan::kMaxVramBytes / VanGoghMemoryPlan::kMiB) {
        requestedMiB = configured;
    }
    if (reportedMiB != 0 && requestedMiB > reportedMiB) requestedMiB = reportedMiB;
    if (requestedMiB == 0) requestedMiB = kDefaultVramExposureMiB;

    m_exposedVramBytes = requestedMiB * VanGoghMemoryPlan::kMiB;
    const UInt32 exposedMiB = static_cast<UInt32>(requestedMiB);
    if (m_pciDevice != nullptr) {
        m_pciDevice->setProperty("VRAM,totalMB", exposedMiB, 32);
        if (reportedMiB != 0)
            m_pciDevice->setProperty("VanGoghDriver,VRAM-reported-MiB", reportedMiB, 32);
    }
    setProperty("VRAM,totalMB", exposedMiB, 32);
    setProperty("VanGoghDriver,VRAM-exposed-MiB", exposedMiB, 32);
    setProperty("VanGoghDriver,vram-exposure-source",
                reportedMiB != 0 ? "RCC readback + requested cap" : "requested only");
}

bool VanGoghDriver::readMemoryWindow(UInt32 *vramMiB, UInt64 *fbBase, UInt64 *fbTop,
                                     IODeviceMemory **bar0) {
    if (vramMiB == nullptr || fbBase == nullptr || fbTop == nullptr || bar0 == nullptr ||
        m_pciDevice == nullptr || m_ipDiscovery == nullptr || m_registerMap == nullptr)
        return false;
    UInt32 memsize = 0, baseReg = 0, topReg = 0;
    if (!readRegister32(kRccConfigMemsizeRegister, &memsize) ||
        !readIPRegister32(kMmhubHwId, 0, kMmhubSegment, kMcVmFbLocationBase, &baseReg) ||
        !readIPRegister32(kMmhubHwId, 0, kMmhubSegment, kMcVmFbLocationTop, &topReg))
        return false;
    if (memsize == 0 || memsize == UINT32_MAX ||
        baseReg == UINT32_MAX || topReg == UINT32_MAX)
        return false;
    IODeviceMemory *memory =
        m_pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (memory == nullptr || memory->getLength() < PAGE_SIZE) return false;
    *vramMiB = memsize;
    *fbBase = VanGoghMemoryPlan::fbBaseFromRegister(baseReg);
    *fbTop = VanGoghMemoryPlan::fbTopFromRegister(topReg);
    *bar0 = memory;
    return true;
}

VanGoghScanout::LiveStatus VanGoghDriver::readLiveSurface(
    UInt8 hubp, VanGoghScanout::LiveSurface *surface) {
    using R = VanGoghDCN301::Register;
    UInt32 cfg = 0, tiling = 0, viewport = 0, pitch = 0, control = 0, low = 0, high = 0;
    if (!readHubpRegister(hubp, R::SurfaceConfig, &cfg) ||
        !readHubpRegister(hubp, R::TilingConfig, &tiling) ||
        !readHubpRegister(hubp, R::ViewportDimension, &viewport) ||
        !readHubpRegister(hubp, R::SurfacePitch, &pitch) ||
        !readHubpRegister(hubp, R::SurfaceControl, &control) ||
        !readHubpRegister(hubp, R::SurfaceAddressLow, &low) ||
        !readHubpRegister(hubp, R::SurfaceAddressHigh, &high)) {
        return VanGoghScanout::LiveStatus::Unreadable;
    }
    return VanGoghScanout::describeLiveSurface(cfg, tiling, viewport, pitch, control,
                                               low, high, surface);
}

bool VanGoghDriver::framebufferReady() const {
    OSBoolean *ready = OSDynamicCast(OSBoolean, getProperty("VanGoghFramebuffer,ready"));
    return ready == kOSBooleanTrue;
}

// Adopt the surface the boot firmware/OpenCore left scanning on the internal
// panel. This is deliberately read-only: it validates that the plane is linear,
// uncompressed ARGB8888, translates its MC address into the CPU-visible BAR0
// window, and publishes that window as the framebuffer aperture. macOS then
// draws directly into the memory the display engine is already reading.
bool VanGoghDriver::adoptLiveScanout(IOService *provider) {
    if (m_liveScanoutAdopted) return true;
    if (provider != nullptr &&
        provider->getProperty("VanGoghDriver,adopt-live-scanout") != nullptr &&
        !boolProperty(provider, "VanGoghDriver,adopt-live-scanout")) {
        IOLog("VanGoghDriver: live scanout adoption disabled by property\n");
        return false;
    }
    if (m_pciDevice == nullptr || m_ipDiscovery == nullptr || m_registerMap == nullptr) {
        IOLog("VanGoghDriver: live scanout adoption needs IP discovery and BAR5 mapping\n");
        return false;
    }

    UInt32 vramMiB = 0;
    UInt64 fbBase = 0, fbTop = 0;
    IODeviceMemory *bar0 = nullptr;
    if (!readMemoryWindow(&vramMiB, &fbBase, &fbTop, &bar0)) {
        IOLog("VanGoghDriver: cannot read VRAM size / MC FB window; not adopting live scanout\n");
        return false;
    }
    const UInt64 vramBytes = static_cast<UInt64>(vramMiB) * VanGoghMemoryPlan::kMiB;
    if (fbTop < fbBase || fbTop - fbBase + 1 < vramBytes) {
        IOLog("VanGoghDriver: MC FB window %llx-%llx does not cover %u MiB; not adopting live scanout\n",
              static_cast<unsigned long long>(fbBase), static_cast<unsigned long long>(fbTop), vramMiB);
        return false;
    }

    // A HUBP address is only an MC address while VMID0 translation is off
    // (firmware/OpenCore). Resolving an active page table is GMC bring-up work.
    // If the context register cannot be read we also refuse: guessing could
    // expose a GPU virtual address as if it were memory the CPU can draw into.
    UInt32 vmid0Control = 0;
    if (!readIPRegister32(kMmhubHwId, 0, kMmhubSegment, 0x0740, &vmid0Control)) {
        IOLog("VanGoghDriver: cannot read MMHUB VMID0 context control; refusing to adopt the live HUBP address as MC\n");
        return false;
    }
    if ((vmid0Control & 1u) != 0) {
        IOLog("VanGoghDriver: MMHUB VMID0 translation is enabled (control %08x); refusing to treat the live HUBP address as MC\n",
              vmid0Control);
        return false;
    }

    // Prefer the HUBP linked to the internal panel. If the topology readback was
    // inconclusive, or the panel pipe does not expose a full-panel surface, fall
    // back to every HUBP so an unusual firmware routing still gets a chance.
    UInt32 passes[2] = {m_panelHUBPMask, (1u << VanGoghDCN301::kHUBPCount) - 1u};
    const unsigned passCount =
        (m_panelHUBPMask != 0 && passes[1] != passes[0]) ? 2u : 1u;
    if (m_panelHUBPMask == 0) passes[0] = passes[1];

    for (unsigned pass = 0; pass < passCount; ++pass) {
        const UInt32 candidates = passes[pass];
        const bool knownPanel = pass == 0 && m_panelHUBPMask != 0;
        for (UInt8 hubp = 0; hubp < VanGoghDCN301::kHUBPCount; ++hubp) {
            if ((candidates & (1u << hubp)) == 0) continue;
            VanGoghScanout::LiveSurface surface{};
            const VanGoghScanout::LiveStatus status = readLiveSurface(hubp, &surface);
            if (status != VanGoghScanout::LiveStatus::Ok) {
                IOLog("VanGoghDriver: HUBP%u live surface not adoptable: %s\n",
                      hubp, VanGoghScanout::liveStatusName(status));
                continue;
            }

            const UInt64 surfaceBytes =
                static_cast<UInt64>(surface.pitchBytes) * surface.heightPixels;
            const VanGoghMemoryPlan::Translation translation =
                VanGoghMemoryPlan::translateMcAddress(surface.gpuAddress, surfaceBytes,
                                                      fbBase, vramBytes, bar0->getLength());
            if (!translation.ok) {
                IOLog("VanGoghDriver: HUBP%u MC 0x%llx %ux%u pitch %u: %s\n", hubp,
                      static_cast<unsigned long long>(surface.gpuAddress),
                      surface.widthPixels, surface.heightPixels, surface.pitchBytes,
                      VanGoghMemoryPlan::translationStatusName(translation.status));
                continue;
            }

            const UInt64 apertureBytes =
                (surfaceBytes + PAGE_SIZE - 1) & ~static_cast<UInt64>(PAGE_SIZE - 1);
            if (apertureBytes > translation.maxBytes) {
                IOLog("VanGoghDriver: HUBP%u surface needs %llu bytes but only %llu remain in BAR0\n",
                      hubp, static_cast<unsigned long long>(apertureBytes),
                      static_cast<unsigned long long>(translation.maxBytes));
                continue;
            }

            const bool rotated = VanGoghScanout::isRotated270(surface);
            const UInt32 displayWidth = rotated ? surface.heightPixels : surface.widthPixels;
            const UInt32 displayHeight = rotated ? surface.widthPixels : surface.heightPixels;
            // Only the captured 800x1280@60 eDP panel has a timing and EDID here.
            // Skip any other live pipe (for example an external monitor) instead of
            // exposing a framebuffer with the wrong timing.
            if (displayWidth != 800 || displayHeight != 1280) {
                IOLog("VanGoghDriver: HUBP%u live mode %ux%u is not the 800x1280 panel; not adopting it\n",
                      hubp, displayWidth, displayHeight);
                continue;
            }

            UInt64 exposure = m_exposedVramBytes != 0 ? m_exposedVramBytes
                                                      : static_cast<UInt64>(kDefaultVramExposureMiB) *
                                                            VanGoghMemoryPlan::kMiB;
            if (exposure > vramBytes) exposure = vramBytes;
            if (exposure > bar0->getLength()) exposure = bar0->getLength();
            // The reported VRAM window starts at BAR0 offset 0, so it must also
            // cover a live surface that happens to sit high in the aperture.
            const UInt64 needed = translation.vramOffset + apertureBytes;
            if (exposure < needed) exposure = needed;
            if (exposure > bar0->getLength()) exposure = bar0->getLength();
            exposure &= ~static_cast<UInt64>(PAGE_SIZE - 1);
            m_exposedVramBytes = exposure;
            // Adoption may have widened the window so that the live surface is
            // covered; keep the advertised System Information values consistent.
            const UInt32 exposedMiB = static_cast<UInt32>(exposure >> 20);
            setProperty("VRAM,totalMB", exposedMiB, 32);
            setProperty("VanGoghDriver,VRAM-exposed-MiB", exposedMiB, 32);
            if (m_pciDevice != nullptr)
                m_pciDevice->setProperty("VRAM,totalMB", exposedMiB, 32);

            const UInt64 aperturePhysical =
                static_cast<UInt64>(bar0->getPhysicalAddress()) + translation.vramOffset;

            setProperty("VanGoghFramebuffer,scanout-physical", aperturePhysical, 64);
            setProperty("VanGoghFramebuffer,scanout-bytes", apertureBytes, 64);
            setProperty("VanGoghFramebuffer,width", surface.widthPixels, 32);
            setProperty("VanGoghFramebuffer,height", surface.heightPixels, 32);
            setProperty("VanGoghFramebuffer,pitch", surface.pitchBytes, 32);
            setProperty("VanGoghFramebuffer,refresh", 60, 32);
            setProperty("VanGoghFramebuffer,display-width", displayWidth, 32);
            setProperty("VanGoghFramebuffer,display-height", displayHeight, 32);
            setProperty("VanGoghFramebuffer,rotation-programmed", rotated);
            setProperty("VanGoghFramebuffer,linear-scanout", true);
            setProperty("VanGoghFramebuffer,gpu-address-mapped", true);
            setProperty("VanGoghFramebuffer,surface-programmed", true);
            setProperty("VanGoghFramebuffer,vram-physical",
                        static_cast<UInt64>(bar0->getPhysicalAddress()), 64);
            setProperty("VanGoghFramebuffer,vram-bytes", exposure, 64);
            setProperty("VanGoghFramebuffer,vram-mb",
                        static_cast<UInt32>(exposure >> 20), 32);
            setProperty("VanGoghFramebuffer,model", "AMD Van Gogh (Steam Deck)");
            setProperty("VanGoghDriver,scanout-source", "firmware-live");
            setProperty("VanGoghDriver,scanout-live-adopted", true);
            setProperty("VanGoghDriver,scanout-live-hubp", hubp, 8);
            setProperty("VanGoghDriver,scanout-live-panel-known", knownPanel);
            setProperty("VanGoghDriver,scanout-vram-offset", translation.vramOffset, 64);
            setProperty("VanGoghDriver,scanout-mc-address", surface.gpuAddress, 64);
            setProperty("VanGoghDriver,gpuvm-scanout-ready", false);

            m_liveScanoutAdopted = true;
            m_liveVramOffset = translation.vramOffset;
            m_liveApertureBytes = apertureBytes;

            IOLog("VanGoghDriver: adopting live %s scanout HUBP%u %ux%u pitch %u rot %u, MC 0x%llx -> BAR0+0x%llx (%llu bytes), %llu MiB VRAM exposed%s\n",
                  rotated ? "rotated-270" : "unrotated", hubp, surface.widthPixels,
                  surface.heightPixels, surface.pitchBytes, surface.rotation,
                  static_cast<unsigned long long>(surface.gpuAddress),
                  static_cast<unsigned long long>(translation.vramOffset),
                  static_cast<unsigned long long>(apertureBytes),
                  static_cast<unsigned long long>(exposure >> 20),
                  knownPanel ? "" : " (panel association unknown)");

            // Publishing ready last is what lets the VanGoghFramebuffer personality match.
            setProperty("VanGoghFramebuffer,ready", true);
            return true;
        }
    }

    IOLog("VanGoghDriver: no linear 800x1280 ARGB8888 live surface was adoptable (panel HUBP mask 0x%x); framebuffer not adopted\n",
          m_panelHUBPMask);
    return false;
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
        // Startup is observational by default. HUBP rotation is one field in
        // a larger plane/modeset transaction; changing it on the live Linux-
        // initialized or firmware-owned pipe can invalidate the active scanout.
        IOLog("VanGoghDriver: no explicit HUBP rotation request; leaving live DCN state unchanged\n");
        return;
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
                case VanGoghDCN301::Register::FlipControl:   // outside this loop's range
                case VanGoghDCN301::Register::AddrConfig:
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
    // Clear the personality gate first. The VanGoghFramebuffer nub matches on
    // this property, so a stop/start cycle must not resurrect a stale aperture.
    m_liveScanoutAdopted = false;
    m_liveVramOffset = 0;
    m_liveApertureBytes = 0;
    m_exposedVramBytes = 0;
    removeProperty("VanGoghFramebuffer,ready");
    removeProperty("VanGoghFramebuffer,linear-scanout");
    removeProperty("VanGoghFramebuffer,gpu-address-mapped");
    removeProperty("VanGoghFramebuffer,surface-programmed");

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
    restoreVramScanout(false);
    if (m_vramWindowMap != nullptr) {
        m_vramWindowMap->release();
        m_vramWindowMap = nullptr;
    }
    m_vramPatternWritten = false;
    m_vramPlan = VanGoghMemoryPlan::Output{};
    m_geom = VanGoghScanout::Geometry{};
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

// ---------------------------------------------------------------------------
// Opt-in VRAM scanout route. None of this has run on a Steam Deck.
//
//   VanGoghDriver,vram-scanout          plan a surface in VRAM, map it via BAR0
//   VanGoghDriver,vram-scanout-pattern  also fill it with a test pattern
//   VanGoghDriver,program-vram-scanout  also point the live panel HUBPs at it
//   VanGoghDriver,scanout-vram-offset   optional 64 KiB-aligned VRAM offset
//
// Properties are read from the PCI provider first (OpenCore DeviceProperties)
// and then from this service's personality. Each stage refuses to run unless
// the one before it succeeded, and every refusal is logged with a reason.
// ---------------------------------------------------------------------------

bool VanGoghDriver::boolProperty(IOService *provider, const char *key) const {
    OSObject *property = provider ? provider->getProperty(key) : nullptr;
    if (property == nullptr) property = getProperty(key);
    if (OSBoolean *b = OSDynamicCast(OSBoolean, property)) return b == kOSBooleanTrue;
    if (OSNumber *n = OSDynamicCast(OSNumber, property)) return n->unsigned64BitValue() != 0;

    // OpenCore DeviceProperties arrive as OSData, even when the plist value
    // was entered as a Boolean or Number. Decode short values as little-endian.
    OSData *data = OSDynamicCast(OSData, property);
    if (data == nullptr || data->getLength() == 0 || data->getLength() > sizeof(UInt64)) return false;
    const UInt8 *bytes = static_cast<const UInt8 *>(data->getBytesNoCopy());
    if (bytes == nullptr) return false;
    UInt64 value = 0;
    for (IOByteCount i = 0; i < data->getLength(); ++i)
        value |= static_cast<UInt64>(bytes[i]) << (i * 8);
    return value != 0;
}

bool VanGoghDriver::numberProperty(IOService *provider, const char *key, UInt64 *value) const {
    if (value == nullptr) return false;
    OSObject *property = provider ? provider->getProperty(key) : nullptr;
    if (property == nullptr) property = getProperty(key);
    if (OSNumber *n = OSDynamicCast(OSNumber, property)) {
        *value = n->unsigned64BitValue();
        return true;
    }

    OSData *data = OSDynamicCast(OSData, property);
    if (data == nullptr || data->getLength() == 0 || data->getLength() > sizeof(UInt64)) return false;
    const UInt8 *bytes = static_cast<const UInt8 *>(data->getBytesNoCopy());
    if (bytes == nullptr) return false;
    UInt64 decoded = 0;
    for (IOByteCount i = 0; i < data->getLength(); ++i)
        decoded |= static_cast<UInt64>(bytes[i]) << (i * 8);
    *value = decoded;
    return true;
}

bool VanGoghDriver::readHubpRegister(UInt8 hubp, VanGoghDCN301::Register reg, UInt32 *value) const {
    UInt32 dword = 0;
    return VanGoghDCN301::registerDword(hubp, reg, &dword) &&
           readIPRegister32(VanGoghDCN301::kDisplayIPHardwareId, VanGoghDCN301::kDisplayIPInstance,
                            VanGoghDCN301::kRegisterBaseSegment, dword, value);
}

bool VanGoghDriver::writeHubpRegister(UInt8 hubp, VanGoghDCN301::Register reg, UInt32 value) const {
    UInt32 dword = 0;
    return VanGoghDCN301::registerDword(hubp, reg, &dword) &&
           writeIPRegister32(VanGoghDCN301::kDisplayIPHardwareId, VanGoghDCN301::kDisplayIPInstance,
                             VanGoghDCN301::kRegisterBaseSegment, dword, value);
}

void VanGoghDriver::planVramScanout(IOService *provider) {
    if (m_ipDiscovery == nullptr || m_registerMap == nullptr || m_pciDevice == nullptr) {
        IOLog("VanGoghDriver: VRAM scanout needs IP discovery and BAR5; not planned\n");
        return;
    }
    UInt32 vramMiB = 0, fbBaseReg = 0, fbTopReg = 0;
    if (!readRegister32(kRccConfigMemsizeRegister, &vramMiB) ||
        !readIPRegister32(kMmhubHwId, 0, kMmhubSegment, kMcVmFbLocationBase, &fbBaseReg) ||
        !readIPRegister32(kMmhubHwId, 0, kMmhubSegment, kMcVmFbLocationTop, &fbTopReg) ||
        vramMiB == 0 || vramMiB == UINT32_MAX || fbBaseReg == UINT32_MAX || fbTopReg == UINT32_MAX) {
        IOLog("VanGoghDriver: VRAM size / MC FB location unreadable; VRAM scanout not planned\n");
        return;
    }
    const UInt32 bar0Register = kIOPCIConfigBaseAddress0;
    IODeviceMemory *bar0 = m_pciDevice->getDeviceMemoryWithRegister(bar0Register);
    if (bar0 == nullptr || bar0->getLength() == 0) {
        IOLog("VanGoghDriver: PCI BAR0 (VRAM aperture) unavailable; VRAM scanout not planned\n");
        return;
    }

    // Pick the surface from what the panel HUBP is scanning right now. This
    // is read-only; it also logs the raw values so a refusal is diagnosable.
    VanGoghScanout::Geometry geom{};
    for (UInt8 h = 0; h < VanGoghDCN301::kHUBPCount && !geom.ok; ++h) {
        if ((m_panelHUBPMask & (1u << h)) == 0) continue;
        UInt32 cfg = 0, vp = 0;
        if (!readHubpRegister(h, VanGoghDCN301::Register::SurfaceConfig, &cfg) ||
            !readHubpRegister(h, VanGoghDCN301::Register::ViewportDimension, &vp)) continue;
        IOLog("VanGoghDriver: panel HUBP%u live surface-config %08x viewport %ux%u\n", h, cfg,
              vp & 0x3FFFu, (vp >> 16) & 0x3FFFu);
        geom = VanGoghScanout::chooseGeometry(cfg, vp);
    }
    if (!geom.ok) {
        IOLog("VanGoghDriver: panel pipe is not scanning ARGB8888 at a supported size/rotation "
              "(HUBP mask %x); VRAM scanout not planned\n", m_panelHUBPMask);
        return;
    }

    VanGoghMemoryPlan::Input in;
    in.scanoutBytes = static_cast<UInt64>(geom.pitchBytes) * geom.heightPixels;
    in.vramBytes = static_cast<UInt64>(vramMiB) * VanGoghMemoryPlan::kMiB;
    in.bar0Length = bar0->getLength();
    in.fbBase = VanGoghMemoryPlan::fbBaseFromRegister(fbBaseReg);
    in.fbTop = VanGoghMemoryPlan::fbTopFromRegister(fbTopReg);
    UInt64 requested = 0;
    if (numberProperty(provider, "VanGoghDriver,scanout-vram-offset", &requested))
        in.requestedOffset = requested;

    const VanGoghMemoryPlan::Output out = VanGoghMemoryPlan::plan(in);
    if (!out.ok) {
        IOLog("VanGoghDriver: VRAM scanout plan refused: %s (vram %u MiB, bar0 %llu MiB, fb %llx-%llx)\n",
              VanGoghMemoryPlan::statusName(out.status), vramMiB,
              static_cast<unsigned long long>(in.bar0Length >> 20),
              static_cast<unsigned long long>(in.fbBase), static_cast<unsigned long long>(in.fbTop));
        return;
    }

    IODeviceMemory *window = IODeviceMemory::withSubRange(bar0, out.vramOffset, out.bytes);
    if (window == nullptr) {
        IOLog("VanGoghDriver: could not describe the BAR0 scanout window\n");
        return;
    }
    IOMemoryMap *map = window->map(kIOMapWriteCombineCache);
    window->release();
    if (map == nullptr || map->getLength() < out.bytes) {
        if (map != nullptr) map->release();
        IOLog("VanGoghDriver: could not map the BAR0 scanout window\n");
        return;
    }
    m_vramWindowMap = map;
    m_geom = geom;
    m_bar0Physical = bar0->getPhysicalAddress();
    m_vramPlan = out;
    setProperty("VanGoghDriver,vram-scanout-offset", out.vramOffset, 64);
    setProperty("VanGoghDriver,vram-scanout-mc-address", out.mcAddress, 64);
    IOLog("VanGoghDriver: VRAM scanout %ux%u pitch %u (%s) planned at VRAM+0x%llx, MC 0x%llx, %llu bytes (BAR0 phys 0x%llx)\n",
          geom.widthPixels, geom.heightPixels, geom.pitchBytes, geom.rotated ? "rotated landscape" : "native portrait",
          static_cast<unsigned long long>(out.vramOffset), static_cast<unsigned long long>(out.mcAddress),
          static_cast<unsigned long long>(out.bytes),
          static_cast<unsigned long long>(m_bar0Physical + out.vramOffset));

    if (boolProperty(provider, "VanGoghDriver,vram-scanout-pattern")) m_vramPatternWritten = writeVramPattern();
}

bool VanGoghDriver::writeVramPattern() {
    if (m_vramWindowMap == nullptr || !m_vramPlan.ok) return false;
    volatile UInt32 *px = reinterpret_cast<volatile UInt32 *>(m_vramWindowMap->getVirtualAddress());
    if (px == nullptr) return false;
    // Opaque ARGB: horizontal red ramp, vertical green ramp, white 8 px border,
    // so rotation and pitch errors are obvious by eye.
    const UInt32 w = m_geom.widthPixels, h = m_geom.heightPixels, stride = m_geom.pitchBytes / 4;
    if (!m_geom.ok || w < 16 || h < 16) return false;
    for (UInt32 y = 0; y < h; ++y) {
        for (UInt32 x = 0; x < w; ++x) {
            const bool border = x < 8 || y < 8 || x >= w - 8 || y >= h - 8;
            const UInt32 r = (x * 255) / (w - 1);
            const UInt32 g = (y * 255) / (h - 1);
            px[y * stride + x] = border ? 0xFFFFFFFFu : (0xFF000000u | (r << 16) | (g << 8) | 0x40u);
        }
    }
    OSSynchronizeIO();
    setProperty("VanGoghDriver,vram-scanout-pattern-written", true);
    IOLog("VanGoghDriver: wrote %ux%u test pattern to the VRAM scanout surface\n", w, h);
    return true;
}

bool VanGoghDriver::applyPlane(UInt8 hubp, const VanGoghScanout::PlaneImage &img, UInt32 flipControl) {
    using R = VanGoghDCN301::Register;
    bool ok = writeHubpRegister(hubp, R::FlipControl, VanGoghScanout::flipControlWithLock(flipControl, true)) &&
              writeHubpRegister(hubp, R::TilingConfig, img.tilingConfig) &&
              writeHubpRegister(hubp, R::SurfacePitch, img.surfacePitch) &&
              writeHubpRegister(hubp, R::SurfaceControl, img.surfaceControl) &&
              // Linux order: high first, then low; the low write latches the pair.
              writeHubpRegister(hubp, R::SurfaceAddressHigh, img.addressHigh) &&
              writeHubpRegister(hubp, R::SurfaceAddressLow, img.addressLow);
    OSSynchronizeIO();
    // Always release the update lock, even after a failed write.
    ok = writeHubpRegister(hubp, R::FlipControl, VanGoghScanout::flipControlWithLock(flipControl, false)) && ok;
    if (!ok) return false;

    // Wait for the flip to latch (SURFACE_FLIP_PENDING, bit 8), up to ~100 ms.
    // Do not publish a framebuffer if the display engine never acknowledged it.
    UInt32 flip = 0;
    bool flipLatched = false;
    for (int i = 0; i < 100; ++i) {
        if (readHubpRegister(hubp, R::FlipControl, &flip) && (flip & 0x100u) == 0) {
            flipLatched = true;
            break;
        }
        IOSleep(1);
    }
    if (!flipLatched) {
        IOLog("VanGoghDriver: HUBP%u surface flip did not latch within 100 ms\n", hubp);
        return false;
    }
    UInt32 tiling = 0, pitch = 0, control = 0, hi = 0, lo = 0;
    return readHubpRegister(hubp, R::TilingConfig, &tiling) && tiling == img.tilingConfig &&
           readHubpRegister(hubp, R::SurfacePitch, &pitch) && (pitch & 0x3FFFu) == (img.surfacePitch & 0x3FFFu) &&
           readHubpRegister(hubp, R::SurfaceControl, &control) && control == img.surfaceControl &&
           readHubpRegister(hubp, R::SurfaceAddressHigh, &hi) && hi == img.addressHigh &&
           readHubpRegister(hubp, R::SurfaceAddressLow, &lo) && lo == img.addressLow;
}

void VanGoghDriver::programVramScanout(IOService *provider) {
    if (!boolProperty(provider, "VanGoghDriver,program-vram-scanout")) {
        IOLog("VanGoghDriver: program-vram-scanout not requested; live DCN state unchanged\n");
        return;
    }
    if (!m_vramPlan.ok || !m_vramPatternWritten) {
        IOLog("VanGoghDriver: refusing to program HUBP: needs a planned VRAM surface with the test pattern written\n");
        return;
    }
    if (m_panelHUBPMask == 0) {
        IOLog("VanGoghDriver: refusing to program HUBP: no HUBP linked to the active internal panel\n");
        return;
    }

    using R = VanGoghDCN301::Register;
    VanGoghScanout::Request request{};
    request.gpuAddress = m_vramPlan.mcAddress;
    request.widthPixels = m_geom.widthPixels;
    request.heightPixels = m_geom.heightPixels;
    request.pitchBytes = m_geom.pitchBytes;

    // Phase 1: read and validate every target before writing any of them.
    UInt32 pending = 0;
    for (UInt8 h = 0; h < VanGoghDCN301::kHUBPCount; ++h) {
        if ((m_panelHUBPMask & (1u << h)) == 0) continue;
        ScanoutRestore &r = m_scanoutRestore[h];
        VanGoghScanout::Baseline &b = r.original;
        if (!readHubpRegister(h, R::SurfaceConfig, &b.surfaceConfig) ||
            !readHubpRegister(h, R::TilingConfig, &b.tilingConfig) ||
            !readHubpRegister(h, R::ViewportDimension, &b.viewportDimension) ||
            !readHubpRegister(h, R::SurfacePitch, &b.surfacePitch) ||
            !readHubpRegister(h, R::SurfaceControl, &b.surfaceControl) ||
            !readHubpRegister(h, R::FlipControl, &b.flipControl) ||
            !readHubpRegister(h, R::HubpControl, &b.hubpControl) ||
            !readHubpRegister(h, R::SurfaceAddressLow, &r.originalAddressLow) ||
            !readHubpRegister(h, R::SurfaceAddressHigh, &r.originalAddressHigh)) {
            IOLog("VanGoghDriver: HUBP%u baseline read failed; nothing programmed\n", h);
            return;
        }
        const VanGoghScanout::Status st = VanGoghScanout::buildLinearPlane(b, request, &r.programmed);
        if (st != VanGoghScanout::Status::Ok) {
            IOLog("VanGoghDriver: HUBP%u refused: %s (config %08x viewport %08x pitch %08x)\n", h,
                  VanGoghScanout::statusName(st), b.surfaceConfig, b.viewportDimension, b.surfacePitch);
            return;
        }
        // Every target must be in the same mode the surface was sized for.
        const VanGoghScanout::Geometry hg = VanGoghScanout::chooseGeometry(b.surfaceConfig, b.viewportDimension);
        if (!hg.ok || hg.rotated != m_geom.rotated) {
            IOLog("VanGoghDriver: HUBP%u is in a different mode than the planned surface; nothing programmed\n", h);
            return;
        }
        pending |= 1u << h;
    }
    if (pending != m_panelHUBPMask) return;

    // Phase 2: write, verify, and roll everything back on the first failure.
    for (UInt8 h = 0; h < VanGoghDCN301::kHUBPCount; ++h) {
        if ((pending & (1u << h)) == 0) continue;
        m_scanoutRestoreMask |= 1u << h;            // from here on, restore covers this HUBP
        if (!applyPlane(h, m_scanoutRestore[h].programmed, m_scanoutRestore[h].original.flipControl)) {
            IOLog("VanGoghDriver: HUBP%u plane write failed verification; rolling back\n", h);
            restoreVramScanout(true);
            return;
        }
    }

    IOSleep(50);
    for (UInt8 h = 0; h < VanGoghDCN301::kHUBPCount; ++h) {
        UInt32 ctl = 0;
        if ((pending & (1u << h)) && readHubpRegister(h, R::HubpControl, &ctl) &&
            VanGoghScanout::underflowStatus(ctl) != 0)
            IOLog("VanGoghDriver: HUBP%u reports underflow status %u after reprogramming\n", h,
                  VanGoghScanout::underflowStatus(ctl));
    }

    setProperty("VanGoghFramebuffer,scanout-physical",
                static_cast<UInt64>(m_bar0Physical + m_vramPlan.vramOffset), 64);
    setProperty("VanGoghFramebuffer,scanout-bytes",
                static_cast<UInt64>(m_geom.pitchBytes) * m_geom.heightPixels, 64);
    setProperty("VanGoghFramebuffer,width", m_geom.widthPixels, 32);
    setProperty("VanGoghFramebuffer,height", m_geom.heightPixels, 32);
    setProperty("VanGoghFramebuffer,pitch", m_geom.pitchBytes, 32);
    setProperty("VanGoghFramebuffer,refresh", 60, 32);
    setProperty("VanGoghFramebuffer,linear-scanout", true);
    setProperty("VanGoghFramebuffer,gpu-address-mapped", true);
    setProperty("VanGoghFramebuffer,surface-programmed", true);
    setProperty("VanGoghFramebuffer,rotation-programmed", m_geom.rotated);
    setProperty("VanGoghFramebuffer,display-width",
                m_geom.rotated ? m_geom.heightPixels : m_geom.widthPixels, 32);
    setProperty("VanGoghFramebuffer,display-height",
                m_geom.rotated ? m_geom.widthPixels : m_geom.heightPixels, 32);
    const UInt64 surfaceEnd = m_vramPlan.vramOffset + m_vramPlan.bytes;
    UInt64 exposure = m_exposedVramBytes != 0 ? m_exposedVramBytes : m_vramPlan.limitEnd;
    if (exposure > m_vramPlan.limitEnd) exposure = m_vramPlan.limitEnd;
    if (exposure < surfaceEnd) exposure = surfaceEnd;
    exposure &= ~static_cast<UInt64>(PAGE_SIZE - 1);
    setProperty("VanGoghFramebuffer,vram-physical", m_bar0Physical, 64);
    setProperty("VanGoghFramebuffer,vram-bytes", exposure, 64);
    setProperty("VanGoghFramebuffer,vram-mb", static_cast<UInt32>(exposure >> 20), 32);
    setProperty("VanGoghFramebuffer,model", "AMD Van Gogh (Steam Deck)");
    setProperty("VanGoghDriver,gpuvm-scanout-ready", true);
    IOLog("VanGoghDriver: programmed HUBP mask %x to scan out VRAM MC 0x%llx (%ux%u, %s)\n", pending,
          static_cast<unsigned long long>(m_vramPlan.mcAddress), m_geom.widthPixels, m_geom.heightPixels,
          m_geom.rotated ? "rotated 270" : "native portrait");
    // Publishing 'ready' last is what lets the VanGoghFramebuffer personality match.
    setProperty("VanGoghFramebuffer,ready", true);
}

void VanGoghDriver::restoreVramScanout(bool force) {
    using R = VanGoghDCN301::Register;
    for (UInt8 h = 0; h < VanGoghDCN301::kHUBPCount; ++h) {
        if ((m_scanoutRestoreMask & (1u << h)) == 0) continue;
        const ScanoutRestore &r = m_scanoutRestore[h];
        UInt32 hi = 0, lo = 0;
        const bool stillOurs = readHubpRegister(h, R::SurfaceAddressHigh, &hi) && hi == r.programmed.addressHigh &&
                               readHubpRegister(h, R::SurfaceAddressLow, &lo) && lo == r.programmed.addressLow;
        if (!force && !stillOurs) continue;     // someone else reprogrammed it; leave it alone
        writeHubpRegister(h, R::FlipControl, VanGoghScanout::flipControlWithLock(r.original.flipControl, true));
        writeHubpRegister(h, R::TilingConfig, r.original.tilingConfig);
        writeHubpRegister(h, R::SurfacePitch, r.original.surfacePitch);
        writeHubpRegister(h, R::SurfaceControl, r.original.surfaceControl);
        writeHubpRegister(h, R::SurfaceAddressHigh, r.originalAddressHigh);
        writeHubpRegister(h, R::SurfaceAddressLow, r.originalAddressLow);
        OSSynchronizeIO();
        writeHubpRegister(h, R::FlipControl, VanGoghScanout::flipControlWithLock(r.original.flipControl, false));
    }
    m_scanoutRestoreMask = 0;
}
