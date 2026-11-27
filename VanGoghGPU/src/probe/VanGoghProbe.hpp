/* SPDX-License-Identifier: MIT */
#ifndef VG_PROBE_HPP
#define VG_PROBE_HPP

#include <IOKit/IOService.h>

class IOPCIDevice;
class IOMemoryMap;
struct vg_disc;

/*
 * Read-only diagnostic driver for the Steam Deck's VanGogh iGPU.
 * It maps the MMIO BAR, walks the IP discovery blob through MM_INDEX/MM_DATA
 * and publishes the parsed versions and bases in the IORegistry.
 * It never writes a register other than the two index registers.
 */
class VanGoghProbe : public IOService {
    OSDeclareDefaultStructors(VanGoghProbe)

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;

private:
    IOPCIDevice *pci = nullptr;
    IOMemoryMap *mmioMap = nullptr;
    volatile uint32_t *regs = nullptr;
    IOByteCount mmioLen = 0;

    uint32_t rd(uint32_t dw) const;
    void wr(uint32_t dw, uint32_t value) const;
    bool mmRead(uint64_t pos, uint8_t *dst, uint32_t len) const;
    void publish(const struct vg_disc *disc, uint32_t memsize);
};

#endif
