/* SPDX-License-Identifier: MIT */
#include "VanGoghProbe.hpp"

extern "C" {
#include "vg_discovery.h"
#include "vg_regs.h"
}

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

#define LOG(fmt, ...) IOLog("VanGoghProbe: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(VanGoghProbe, IOService)

static void setNum(IOService *s, const char *key, uint64_t v) {
    if (OSNumber *n = OSNumber::withNumber(v, 64)) {
        s->setProperty(key, n);
        n->release();
    }
}

IOService *VanGoghProbe::probe(IOService *provider, SInt32 *score) {
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;
    uint16_t v = pci->configRead16(kIOPCIConfigVendorID);
    uint16_t d = pci->configRead16(kIOPCIConfigDeviceID);
    LOG("probe vendor=%04x device=%04x rev=%02x",
        v, d, pci->configRead8(kIOPCIConfigRevisionID));
    if (v != 0x1002 || d != 0x163f) return nullptr;
    if (score) *score = 40000;
    return IOService::probe(provider, score);
}

bool VanGoghProbe::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    setNum(this, "VGVendor", pci->configRead16(kIOPCIConfigVendorID));
    setNum(this, "VGDevice", pci->configRead16(kIOPCIConfigDeviceID));
    setNum(this, "VGRevision", pci->configRead8(kIOPCIConfigRevisionID));

    IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IODeviceMemory *bar2 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    IOByteCount bar0len = bar0 ? bar0->getLength() : 0;
    IOByteCount bar2len = bar2 ? bar2->getLength() : 0;
    IOPhysicalAddress bar0phys = bar0 ? bar0->getPhysicalSegment(0, nullptr, 0) : 0;
    IOPhysicalAddress bar2phys = bar2 ? bar2->getPhysicalSegment(0, nullptr, 0) : 0;
    setNum(this, "VGBAR0Phys", (uint64_t)bar0phys);
    setNum(this, "VGBAR0Length", (uint64_t)bar0len);
    setNum(this, "VGBAR2Phys", (uint64_t)bar2phys);
    setNum(this, "VGBAR2Length", (uint64_t)bar2len);
    LOG("BAR0 %#llx/%llu BAR2 %#llx/%llu",
        (uint64_t)bar0phys, (uint64_t)bar0len,
        (uint64_t)bar2phys, (uint64_t)bar2len);

    if (!bar2 || bar2len < 0x100000) {
        LOG("no usable MMIO BAR2; stopping after PCI properties");
        registerService();
        return true;
    }
    mmioMap = bar2->map();
    if (!mmioMap) {
        LOG("BAR2 map failed");
        registerService();
        return true;
    }
    mmioLen = mmioMap->getLength();
    regs = (volatile uint32_t *)mmioMap->getAddress();
    if (!regs) {
        LOG("BAR2 map address is null");
        registerService();
        return true;
    }

    uint32_t ver = rd(VG_MM_IP_DISCOVERY_VER);
    setNum(this, "VGIPDiscoveryVersionReg", ver);
    LOG("mmIP_DISCOVERY_VERSION=%#x memsize=%#x", ver, rd(VG_MM_RCC_CONFIG_MEMSIZE));
    setNum(this, "VGRCCConfigMemsize", rd(VG_MM_RCC_CONFIG_MEMSIZE));

    /* Linux allocates 10 KiB for the discovery blob (DISCOVERY_TMR_SIZE). */
    const uint32_t kBlob = 10u * 1024u;
    uint8_t *blob = (uint8_t *)IOMalloc(kBlob);
    if (!blob) {
        LOG("IOMalloc for discovery blob failed");
        registerService();
        return true;
    }
    bool ok = mmRead(0, blob, kBlob);
    setNum(this, "VGDiscoveryReadOK", ok ? 1 : 0);
    if (ok) {
        struct vg_disc disc;
        int rc = vg_disc_parse(blob, kBlob, &disc);
        LOG("discovery parse: %s (flags %#x)", vg_disc_strerror(rc), disc.flags);
        if (rc == VG_DISC_OK) {
            publish(&disc, rd(VG_MM_RCC_CONFIG_MEMSIZE));
            /* Read-only DCN samples through the DMU base. No modeset. */
            uint32_t dcn = 0;
            if (vg_disc_reg(&disc, VG_HWID_DMU, 0, 2, 0, &dcn) == 0 ||
                vg_disc_reg(&disc, VG_HWID_DMU, 0, 0, 0, &dcn) == 0) {
                setNum(this, "VGDCUBaseDW", dcn);
                static const struct { const char *name; uint32_t reg; } samples[] = {
                    { "VGOTGHTotal",   VG_DCN_OTG0_H_TOTAL },
                    { "VGOTGVTotal",   VG_DCN_OTG0_V_TOTAL },
                    { "VGOTGControl",  VG_DCN_OTG0_CONTROL },
                    { "VGOTGFrameCnt", VG_DCN_OTG0_FRAME_COUNT },
                    { "VGHUBPSurfCfg", VG_DCN_HUBP0_SURF_CONFIG },
                    { "VGHUBPSurfPch", VG_DCN_HUBPREQ0_SURF_PITCH },
                };
                for (auto &s : samples) setNum(this, s.name, rd(dcn + s.reg));
                LOG("DCN base dword %#x; OTG frame count %#x", dcn,
                    rd(dcn + VG_DCN_OTG0_FRAME_COUNT));
            }
        }
    }
    IOFree(blob, kBlob);
    registerService();
    return true;
}

void VanGoghProbe::stop(IOService *provider) {
    if (mmioMap) {
        mmioMap->release();
        mmioMap = nullptr;
        regs = nullptr;
    }
    IOService::stop(provider);
}

void VanGoghProbe::free(void) {
    IOService::free();
}

uint32_t VanGoghProbe::rd(uint32_t dw) const {
    if (!regs || dw >= mmioLen / 4) return 0xffffffffu;
    return regs[dw];
}

void VanGoghProbe::wr(uint32_t dw, uint32_t value) const {
    if (!regs || dw >= mmioLen / 4) return;
    /* The only registers this driver is allowed to write. */
    if (dw != VG_MM_MM_INDEX && dw != VG_MM_MM_INDEX_HI) return;
    regs[dw] = value;
}

bool VanGoghProbe::mmRead(uint64_t pos, uint8_t *dst, uint32_t len) const {
    if (!regs || (len & 3u)) return false;
    uint32_t hi = 0xffffffffu;
    for (uint32_t i = 0; i < len; i += 4) {
        uint64_t p = pos + i;
        uint32_t h = (uint32_t)(p >> 31);
        wr(VG_MM_MM_INDEX, ((uint32_t)p) | 0x80000000u);
        if (h != hi) {
            wr(VG_MM_MM_INDEX_HI, h);
            hi = h;
        }
        uint32_t v = rd(VG_MM_MM_DATA);
        dst[i]     = (uint8_t)v;
        dst[i + 1] = (uint8_t)(v >> 8);
        dst[i + 2] = (uint8_t)(v >> 16);
        dst[i + 3] = (uint8_t)(v >> 24);
    }
    return true;
}

void VanGoghProbe::publish(const struct vg_disc *d, uint32_t memsize) const {
    setNum(this, "VGDiscoveryBinMajor", d->bin_major);
    setNum(this, "VGDiscoveryBinMinor", d->bin_minor);
    setNum(this, "VGDiscoveryIPVersion", d->ip_version);
    setNum(this, "VGDiscoveryNumDies", d->num_dies);
    setNum(this, "VGDiscoveryBase64", d->base64);
    setNum(this, "VGDiscoveryFlags", d->flags);
    setNum(this, "VGDiscoveryIPs", d->nips);
    setNum(this, "VGMemorySizeReg", memsize);

    char key[64];
    for (uint32_t i = 0; i < d->nips; ++i) {
        const struct vg_ip *ip = &d->ips[i];
        snprintf(key, sizeof(key), "VGIP_%u_%u_Major", ip->hw_id, ip->inst);
        setNum(this, key, ip->major);
        snprintf(key, sizeof(key), "VGIP_%u_%u_Minor", ip->hw_id, ip->inst);
        setNum(this, key, ip->minor);
        snprintf(key, sizeof(key), "VGIP_%u_%u_Rev", ip->hw_id, ip->inst);
        setNum(this, key, ip->revision);
        for (uint8_t b = 0; b < ip->nbase && b < VG_MAX_BASES; ++b) {
            snprintf(key, sizeof(key), "VGIP_%u_%u_Base%u", ip->hw_id, ip->inst, b);
            setNum(this, key, ip->base[b]);
        }
    }
}
