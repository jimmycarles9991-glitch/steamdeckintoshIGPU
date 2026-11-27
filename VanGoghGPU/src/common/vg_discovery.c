/* SPDX-License-Identifier: MIT */
/*
 * AMD IP discovery parser. Layouts from Linux v6.12:
 *   drivers/gpu/drm/amd/include/discovery.h
 *   drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c (amdgpu_discovery_table_check)
 * Only byte assembly and bounds checks; no kernel code is copied.
 */
#include "vg_discovery.h"
#include "vg_types.h"

#define VG_BHDR_FIXED 12u
#define VG_TINFO_SIZE 8u
#define VG_IHDR_SIZE  80u
#define VG_IHDR_FLAGS 78u
#define VG_DIE_OFF    14u
#define VG_IP_FIXED   8u
#define VG_TOTAL_TABLES 6u

struct vg_tinfo { uint16_t offset, checksum, size; };

static int table_info(const uint8_t *b, uint32_t len, uint16_t major,
                      unsigned id, struct vg_tinfo *ti) {
    uint32_t count, list;
    ti->offset = ti->checksum = ti->size = 0;
    if (major == 2) {
        if (!vg_in_bounds(VG_BHDR_FIXED, 4, len)) return -1;
        count = vg_ld16(b + VG_BHDR_FIXED);
        list = VG_BHDR_FIXED + 4;
    } else {
        count = VG_TOTAL_TABLES;
        list = VG_BHDR_FIXED;
    }
    if (id >= count || !vg_in_bounds(list + id * VG_TINFO_SIZE, VG_TINFO_SIZE, len))
        return -1;
    const uint8_t *p = b + list + id * VG_TINFO_SIZE;
    ti->offset = vg_ld16(p);
    ti->checksum = vg_ld16(p + 2);
    ti->size = vg_ld16(p + 4);
    return 0;
}

static void check_ip_table(const uint8_t *b, uint32_t len, uint16_t major,
                           struct vg_disc *d) {
    struct vg_tinfo ti;
    if (table_info(b, len, major, 0, &ti) || ti.offset == 0) {
        d->flags |= VG_DISC_F_NO_TABLE;
        return;
    }
    if (!vg_in_bounds(ti.offset, VG_IHDR_SIZE, len)) {
        d->flags |= VG_DISC_F_TABLE_BOUNDS;
        return;
    }
    /* DISCOVERY_TABLE_SIGNATURE, discovery.h:27 ('IPDS' little-endian). */
    if (vg_ld32(b + ti.offset) != 0x53445049u) {
        d->flags |= VG_DISC_F_IP_SIG;
        return;
    }
    uint32_t size = vg_ld16(b + ti.offset + 6);
    if (size < VG_IHDR_SIZE || !vg_in_bounds(ti.offset, size, len)) {
        d->flags |= VG_DISC_F_TABLE_BOUNDS;
        return;
    }
    if (vg_byte_sum(b + ti.offset, size) != ti.checksum)
        d->flags |= VG_DISC_F_IP_SUM;
}

int vg_disc_parse(const uint8_t *blob, uint32_t len, struct vg_disc *out) {
    if (!blob || !out || len < VG_BHDR_FIXED + VG_TOTAL_TABLES * VG_TINFO_SIZE)
        return VG_DISC_E_ARG;
    for (uint32_t i = 0; i < sizeof(*out); ++i)
        ((uint8_t *)out)[i] = 0;

    /* BINARY_SIGNATURE from discovery.h:26. Linux checks this exactly. */
    if (vg_ld32(blob) != 0x28211407u) return VG_DISC_E_SIGNATURE;
    out->bin_major = vg_ld16(blob + 4);
    out->bin_minor = vg_ld16(blob + 6);
    if (out->bin_major > 2) return VG_DISC_E_VERSION;
    uint32_t size = vg_ld16(blob + 10);
    /* Linux checksums from offsetof(binary_checksum)+2 == 10. */
    if (size <= 10u || size > len) return VG_DISC_E_SIZE;
    if (vg_byte_sum(blob + 10u, size - 10u) != vg_ld16(blob + 8))
        out->flags |= VG_DISC_F_BIN_SUM;

    check_ip_table(blob, len, out->bin_major, out);

    struct vg_tinfo ti;
    if (table_info(blob, len, out->bin_major, 0, &ti) || ti.offset == 0)
        return VG_DISC_E_NO_TABLE;
    if (!vg_in_bounds(ti.offset, VG_IHDR_SIZE, len)) return VG_DISC_E_BOUNDS;
    const uint8_t *ih = blob + ti.offset;
    out->ip_version = vg_ld16(ih + 4);
    out->num_dies = vg_ld16(ih + 12);
    if (out->num_dies == 0 || out->num_dies > 16) return VG_DISC_E_DIE;
    if (out->ip_version >= 4) out->base64 = (uint8_t)(blob[ti.offset + VG_IHDR_FLAGS] & 1u);

    for (uint32_t i = 0; i < out->num_dies; ++i) {
        uint32_t die_off = vg_ld16(ih + VG_DIE_OFF + i * 4 + 2);
        if (!vg_in_bounds(die_off, 4, len)) return VG_DISC_E_BOUNDS;
        uint32_t num_ips = vg_ld16(blob + die_off + 2);
        uint32_t ip_off = die_off + 4;
        for (uint32_t j = 0; j < num_ips; ++j) {
            if (!vg_in_bounds(ip_off, VG_IP_FIXED, len)) return VG_DISC_E_BOUNDS;
            const uint8_t *e = blob + ip_off;
            uint16_t hw = vg_ld16(e);
            uint8_t nbase = e[3], b7 = e[7];
            uint32_t stride = out->base64 ? 8u : 4u;
            uint32_t entry = VG_IP_FIXED + nbase * stride;
            if (!vg_in_bounds(ip_off, entry, len)) return VG_DISC_E_BOUNDS;
            if (hw >= VG_HWID_MAX || out->nips >= VG_MAX_IPS || nbase > VG_MAX_BASES) {
                if (nbase > VG_MAX_BASES) out->flags |= VG_DISC_F_BASE_TRUNC;
                ip_off += entry;
                continue;
            }
            struct vg_ip *ip = &out->ips[out->nips++];
            ip->hw_id = hw;
            ip->inst = e[2];
            ip->nbase = nbase;
            ip->major = e[4];
            ip->minor = e[5];
            ip->revision = e[6];
            if (out->ip_version >= 3) {
                ip->sub_revision = (uint8_t)(b7 & 0xfu);
                ip->variant = (uint8_t)(b7 >> 4);
            } else {
                ip->harvest = (uint8_t)(b7 & 0xfu);
            }
            for (uint32_t k = 0; k < nbase && k < VG_MAX_BASES; ++k) {
                const uint8_t *p = e + VG_IP_FIXED + k * stride;
                ip->base[k] = out->base64
                    ? (uint64_t)(vg_ld32(p) & 0x3fffffffu)
                    : (uint64_t)vg_ld32(p);
            }
            ip_off += entry;
        }
    }
    return VG_DISC_OK;
}

const struct vg_ip *vg_disc_find(const struct vg_disc *d, uint16_t hw_id, uint8_t inst) {
    if (!d) return 0;
    for (uint32_t i = 0; i < d->nips; ++i)
        if (d->ips[i].hw_id == hw_id && d->ips[i].inst == inst) return &d->ips[i];
    return 0;
}

int vg_disc_reg(const struct vg_disc *d, uint16_t hw_id, uint8_t inst,
                uint8_t base_idx, uint32_t reg, uint32_t *out_dw) {
    const struct vg_ip *ip = vg_disc_find(d, hw_id, inst);
    if (!ip || !out_dw || base_idx >= ip->nbase || base_idx >= VG_MAX_BASES) return -1;
    uint64_t b = ip->base[base_idx];
    if (b > 0xffffffffull || b + reg > 0xffffffffull) return -1;
    *out_dw = (uint32_t)b + reg;
    return 0;
}

const char *vg_disc_strerror(int err) {
    switch (err) {
    case VG_DISC_OK: return "ok";
    case VG_DISC_E_ARG: return "bad argument";
    case VG_DISC_E_SHORT: return "buffer too short";
    case VG_DISC_E_SIGNATURE: return "bad binary signature";
    case VG_DISC_E_VERSION: return "unknown binary version";
    case VG_DISC_E_SIZE: return "binary_size out of range";
    case VG_DISC_E_NO_TABLE: return "no IP discovery table";
    case VG_DISC_E_SIG_IP: return "bad IP table signature";
    case VG_DISC_E_DIE: return "bad die list";
    case VG_DISC_E_BOUNDS: return "entry out of bounds";
    default: return "unknown";
    }
}
