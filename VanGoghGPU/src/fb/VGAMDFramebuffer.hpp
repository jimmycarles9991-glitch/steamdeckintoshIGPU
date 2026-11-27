/* SPDX-License-Identifier: MIT */
#ifndef VG_AMD_FRAME_BUFFER_HPP
#define VG_AMD_FRAME_BUFFER_HPP

#include <IOKit/IOService.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <kern/thread_call.h>

class IOPCIDevice;
class IODeviceMemory;
struct vg_mode;

/*
 * VGAMDFramebuffer - expose the GOP-lit scanout as the system framebuffer.
 *
 * It writes nothing to DCN. The class name contains "AMD" because CoreDisplay
 * classifies IOFramebuffer subclasses by class name; an unclassifiable
 * framebuffer disables IOPresentment for everybody.
 *
 * Opt-in: -vgfbown enables it, -vgfboff always disables it.
 */
class VGAMDFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(VGAMDFramebuffer)

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: memory */
    virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;
    virtual IODeviceMemory *getVRAMRange(void) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: modes */
    virtual const char *getPixelFormats(void) APPLE_KEXT_OVERRIDE;
    virtual IOItemCount getDisplayModeCount(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                                  IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE;
    virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                                 IOIndex depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                         IOPixelAperture aperture,
                                         IOPixelInformation *info) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode,
                                           IOIndex *depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getStartupDisplayMode(IODisplayModeID *displayMode,
                                           IOIndex *depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setStartupDisplayMode(IODisplayModeID displayMode,
                                           IOIndex depth) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: controller and connection */
    virtual IOReturn enableController(void) APPLE_KEXT_OVERRIDE;
    virtual bool isConsoleDevice(void) APPLE_KEXT_OVERRIDE;
    virtual IOItemCount getConnectionCount(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t *value) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t value) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType,
                                   UInt32 *primary, UInt32 *extended,
                                   UInt32 *displayType) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getAttribute(IOSelect attribute, uintptr_t *value) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setAttribute(IOSelect attribute, uintptr_t value) APPLE_KEXT_OVERRIDE;

    /* IOFramebuffer: VBL (timer-driven, see VGAMDFramebufferVBL.cpp) */
    virtual IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                              OSObject *target, void *ref,
                                              void **interruptRef) APPLE_KEXT_OVERRIDE;
    virtual IOReturn unregisterInterrupt(void *interruptRef) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setInterruptState(void *interruptRef, UInt32 state) APPLE_KEXT_OVERRIDE;

private:
    struct Interrupt {
        IOSelect type;
        IOFBInterruptProc proc;
        OSObject *target;
        void *ref;
        bool enabled;
        bool used;
    };

    static const unsigned kMaxInterrupts = 4;
    static const IODisplayModeID kModeID = 100;

    IOPCIDevice *pci = nullptr;
    IODeviceMemory *aperture = nullptr;
    struct vg_mode *mode = nullptr;
    Interrupt interrupts[kMaxInterrupts];
    thread_call_t vblCall = nullptr;
    bool vblRunning = false;
    uint64_t vblFires = 0;
    uint64_t vblNextDeadline = 0;

    static void vblFired(thread_call_param_t param0, thread_call_param_t param1);
    void deliverVBL(void);
    void armVBL(void);
    bool wonPanel(void) const;
};

#endif
