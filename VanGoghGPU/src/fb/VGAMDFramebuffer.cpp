/* SPDX-License-Identifier: MIT */
#include "VGAMDFramebuffer.hpp"

extern "C" {
#include "vg_bootargs.h"
#include "vg_modes.h"
#include "vg_regs.h"
}

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>

#define LOG(fmt, ...) IOLog("VGAMDFramebuffer: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(VGAMDFramebuffer, IOFramebuffer)

static void setNum(IOService *s, const char *key, uint64_t v) {
    if (OSNumber *n = OSNumber::withNumber(v, 64)) {
        s->setProperty(key, n);
        n->release();
    }
}

static const char *bootArgs(void) {
    char *args = PE_boot_args();
    return args ? args : "";
}

IOService *VGAMDFramebuffer::probe(IOService *provider, SInt32 *score) {
    IOPCIDevice *pciDev = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDev) return nullptr;
    const char *args = bootArgs();
    if (vg_arg_has(args, "-vgfboff")) {
        LOG("-vgfboff present; declining");
        return nullptr;
    }
    if (!vg_arg_has(args, "-vgfbown")) {
        LOG("not armed; add -vgfbown to boot-args");
        return nullptr;
    }
    if (pciDev->configRead16(kIOPCIConfigVendorID) != 0x1002 ||
        pciDev->configRead16(kIOPCIConfigDeviceID) != 0x163f)
        return nullptr;
    if (score) *score = 30000;   /* above IONDRVFramebuffer's 20000 */
    LOG("armed; claiming display at score %d", score ? *score : -1);
    return IOService::probe(provider, score);
}

bool VGAMDFramebuffer::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    mode = new vg_mode;
    if (!mode) return false;
    vg_mode_default(mode);

    const char *args = bootArgs();
    uint32_t v = 0;
    if (vg_arg_u32(args, "vgfb-width", &v)) mode->width = v;
    if (vg_arg_u32(args, "vgfb-height", &v)) mode->height = v;
    if (vg_arg_u32(args, "vgfb-hz", &v)) mode->hz = v;
    if (vg_arg_u32(args, "vgfb-stride", &v)) mode->stride = v;
    uint32_t offset = 0;
    uint32_t apertureBytes = 0;
    (void)vg_arg_u32(args, "vgfb-offset", &offset);
    if (vg_arg_u32(args, "vgfb-aperture", &apertureBytes) == 0)
        mode->aperture_bytes = apertureBytes;
    if (vg_mode_build(mode->width, mode->height, mode->hz, mode->stride, 0, mode) != 0) {
        LOG("bad geometry; using panel defaults");
        vg_mode_default(mode);
    }
    setNum(this, "VGModeWidth", mode->width);
    setNum(this, "VGModeHeight", mode->height);
    setNum(this, "VGModeStride", mode->stride);
    setNum(this, "VGModeHz", mode->hz);
    setNum(this, "VGModeAperture", mode->aperture_bytes);
    setNum(this, "VGFBOffset", offset);

    IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar0) {
        LOG("BAR0 missing; refusing to start");
        return false;
    }
    IOPhysicalAddress phys = bar0->getPhysicalSegment(0, nullptr, 0);
    IOByteCount barLen = bar0->getLength();
    setNum(this, "VGBAR0Phys", (uint64_t)phys);
    setNum(this, "VGBAR0Length", (uint64_t)barLen);
    LOG("BAR0 %#llx/%llu; aperture %#x at +%#x",
        (uint64_t)phys, (uint64_t)barLen, mode->aperture_bytes, offset);
    if (barLen < (IOByteCount)offset ||
        barLen - offset < (IOByteCount)mode->aperture_bytes) {
        LOG("BAR0 too small for the aperture; refusing to start");
        return false;
    }
    aperture = IODeviceMemory::withSubRange(bar0, offset, mode->aperture_bytes);
    if (!aperture) {
        LOG("could not create the aperture sub-range");
        return false;
    }
    vblCall = thread_call_allocate(&VGAMDFramebuffer::vblFired, this);
    setNum(this, "VGAMDVBLTimer", vblCall ? 1 : 0);
    setNum(this, "VGAMDWonPanel", wonPanel() ? 1 : 0);
    LOG("started: %ux%u @ %u Hz, class %s",
        mode->width, mode->height, mode->hz, wonPanel() ? "IOFramebuffer" : "beside");
    registerService();
    return true;
}

void VGAMDFramebuffer::stop(IOService *provider) {
    if (vblCall) {
        thread_call_cancel(vblCall);
        thread_call_free(vblCall);
        vblCall = nullptr;
    }
    vblRunning = false;
    if (aperture) {
        aperture->release();
        aperture = nullptr;
    }
    delete mode;
    mode = nullptr;
    IOService::stop(provider);
}

void VGAMDFramebuffer::free(void) {
    IOService::free();
}

bool VGAMDFramebuffer::wonPanel(void) const {
    OSObject *o = getProperty("IOMatchCategory");
    OSString *s = OSDynamicCast(OSString, o);
    return s && s->isEqualTo("IOFramebuffer");
}

IODeviceMemory *VGAMDFramebuffer::getApertureRange(IOPixelAperture a) {
    if (a != kIOFBSystemAperture || !aperture) return nullptr;
    aperture->retain();
    return aperture;
}

IODeviceMemory *VGAMDFramebuffer::getVRAMRange(void) {
    if (!aperture) return nullptr;
    aperture->retain();
    return aperture;
}

IOReturn VGAMDFramebuffer::enableController(void) {
    return kIOReturnSuccess;
}

bool VGAMDFramebuffer::isConsoleDevice(void) {
    return wonPanel();
}

IOItemCount VGAMDFramebuffer::getConnectionCount(void) {
    return wonPanel() ? 1 : 0;
}

IOReturn VGAMDFramebuffer::getAttributeForConnection(IOIndex, IOSelect, uintptr_t *) {
    return kIOReturnUnsupported;
}

IOReturn VGAMDFramebuffer::setAttributeForConnection(IOIndex, IOSelect, uintptr_t) {
    return kIOReturnUnsupported;
}

IOReturn VGAMDFramebuffer::getAppleSense(IOIndex, UInt32 *senseType,
                                          UInt32 *primary, UInt32 *extended,
                                          UInt32 *displayType) {
    if (senseType) *senseType = 0;
    if (primary) *primary = wonPanel() ? 1 : 0;
    if (extended) *extended = 0;
    if (displayType) *displayType = 0;
    return kIOReturnSuccess;
}

IOReturn VGAMDFramebuffer::getAttribute(IOSelect, uintptr_t *) {
    return kIOReturnUnsupported;
}

IOReturn VGAMDFramebuffer::setAttribute(IOSelect, uintptr_t) {
    return kIOReturnUnsupported;
}
