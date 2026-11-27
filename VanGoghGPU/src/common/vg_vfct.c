/* SPDX-License-Identifier: MIT */
#include "vg_vfct.h"
#include "vg_types.h"

int vg_vfct_find(const uint8_t *t, uint32_t len,
                 uint32_t bus, uint32_t dev, uint32_t func,
                 uint16_t vendor, uint16_t device, struct vg_vbios *out) {
    if (!t || !out || len < VG_VFCT_IMG_OFF + 4) return -1;
    uint32_t off = vg_ld32(t + VG_VFCT_IMG_OFF);
    if (off < VG_VFCT_IMG_OFF || off >= len) return -1;

    while (off < len) {
        if (!vg_in_bounds(off, VG_VFCT_REC_FIXED, len)) return -1;
        const uint8_t *r = t + off;
        uint32_t img_len = vg_ld32(r + VG_VFCT_REC_LEN_OFF);
        if (img_len == 0 || !vg_in_bounds(off + VG_VFCT_REC_FIXED, img_len, len)) return -1;
        if (vg_ld32(r) == bus && vg_ld32(r + 4) == dev && vg_ld32(r + 8) == func &&
            vg_ld16(r + 12) == vendor && vg_ld16(r + 14) == device) {
            out->bus = bus; out->dev = dev; out->func = func;
            out->vendor = vendor; out->device = device;
            out->data = r + VG_VFCT_REC_FIXED;
            out->len = img_len;
            return 0;
        }
        off += VG_VFCT_REC_FIXED + img_len;
    }
    return -1;
}
