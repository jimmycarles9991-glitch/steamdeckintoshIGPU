/* SPDX-License-Identifier: MIT */
/*
 * A timer-driven VBL. This is NOT an OTG interrupt: it is deliberately simple
 * so the first framebuffer bring-up has no dependency on interrupt routing.
 * Deadlines are anchored to the previous deadline, not to "now", so callback
 * duration does not accumulate into the refresh period.
 */
#include "VGAMDFramebuffer.hpp"
#include <mach/mach_time.h>

#define LOG(fmt, ...) IOLog("VGAMDFramebuffer: " fmt "\n", ##__VA_ARGS__)

void VGAMDFramebuffer::vblFired(thread_call_param_t param0, thread_call_param_t param1) {
    VGAMDFramebuffer *self = (VGAMDFramebuffer *)param0;
    (void)param1;
    if (self) self->deliverVBL();
}

void VGAMDFramebuffer::deliverVBL(void) {
    if (!vblRunning || !mode) return;
    ++vblFires;
    for (unsigned i = 0; i < kMaxInterrupts; ++i) {
        Interrupt &in = interrupts[i];
        if (in.used && in.enabled && in.type == kIOFBVBLInterruptType && in.proc) {
            in.proc(in.target, in.ref);
        }
    }
    armVBL();
}

void VGAMDFramebuffer::armVBL(void) {
    if (!vblRunning || !vblCall || !mode || mode->hz == 0) return;
    const uint64_t period = 1000000000ull / (uint64_t)mode->hz;
    uint64_t now = mach_absolute_time();
    if (vblNextDeadline == 0 || vblNextDeadline <= now ||
        vblNextDeadline - now > period * 4u) {
        vblNextDeadline = now + period;
    } else {
        vblNextDeadline += period;
    }
    thread_call_enter_delayed(vblCall, vblNextDeadline);
}

IOReturn VGAMDFramebuffer::registerForInterruptType(IOSelect interruptType,
                                                     IOFBInterruptProc proc,
                                                     OSObject *target, void *ref,
                                                     void **interruptRef) {
    if (!proc || !interruptRef) return kIOReturnBadArgument;
    for (unsigned i = 0; i < kMaxInterrupts; ++i) {
        Interrupt &in = interrupts[i];
        if (in.used) continue;
        in.type = interruptType;
        in.proc = proc;
        in.target = target;
        in.ref = ref;
        in.enabled = false;
        in.used = true;
        *interruptRef = &in;
        LOG("registered interrupt type %#x in slot %u", (unsigned)interruptType, i);
        return kIOReturnSuccess;
    }
    return kIOReturnNoResources;
}

IOReturn VGAMDFramebuffer::unregisterInterrupt(void *interruptRef) {
    if (!interruptRef) return kIOReturnBadArgument;
    for (unsigned i = 0; i < kMaxInterrupts; ++i) {
        if (&interrupts[i] != interruptRef) continue;
        Interrupt &in = interrupts[i];
        in.proc = nullptr;
        in.target = nullptr;
        in.ref = nullptr;
        in.enabled = false;
        in.used = false;
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

IOReturn VGAMDFramebuffer::setInterruptState(void *interruptRef, UInt32 state) {
    if (!interruptRef) return kIOReturnBadArgument;
    for (unsigned i = 0; i < kMaxInterrupts; ++i) {
        if (&interrupts[i] != interruptRef) continue;
        bool want = state != 0;
        bool was = vblRunning;
        interrupts[i].enabled = want;
        vblRunning = false;
        for (unsigned j = 0; j < kMaxInterrupts; ++j)
            if (interrupts[j].used && interrupts[j].enabled) vblRunning = true;
        if (vblRunning && !was) {
            vblNextDeadline = 0;
            armVBL();
        } else if (!vblRunning && was && vblCall) {
            thread_call_cancel(vblCall);
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
