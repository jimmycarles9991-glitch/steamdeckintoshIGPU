/* SPDX-License-Identifier: MIT */
#ifndef VG_VFCT_H
#define VG_VFCT_H

#include <stdint.h>

/*
 * ACPI VFCT layout from Linux v6.12
 * drivers/gpu/drm/amd/include/atombios.h:9278-9296 and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c:347-400.
 *
 * The ACPI table header is 36 bytes; UUID at 0x24; VBIOSImageOffset u32 at
 * 0x34; the image records start there. Image length is at record+24.
 */
#define VG_VFCT_UUID_OFF      0x24u
#define VG_VFCT_IMG_OFF       0x34u
#define VG_VFCT_REC_FIXED     28u
#define VG_VFCT_REC_LEN_OFF   24u

struct vg_vbios {
    uint16_t vendor, device;
    uint32_t bus, dev, func;
    const uint8_t *data;
    uint32_t len;
};

/*
 * Returns 0 and fills *out on the first record whose (bus,dev,func,vendor,
 * device) matches. Returns -1 on a malformed table or no match.
 */
int vg_vfct_find(const uint8_t *table, uint32_t len,
                 uint32_t bus, uint32_t dev, uint32_t func,
                 uint16_t vendor, uint16_t device, struct vg_vbios *out);

#endif
