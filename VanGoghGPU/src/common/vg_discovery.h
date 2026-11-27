/* SPDX-License-Identifier: MIT */
#ifndef VG_DISCOVERY_H
#define VG_DISCOVERY_H

#include <stdint.h>
#include "vg_regs.h"

#define VG_DISC_OK           0
#define VG_DISC_E_ARG       (-1)
#define VG_DISC_E_SHORT     (-2)
#define VG_DISC_E_SIGNATURE (-3)
#define VG_DISC_E_VERSION   (-4)
#define VG_DISC_E_SIZE      (-5)
#define VG_DISC_E_NO_TABLE  (-6)
#define VG_DISC_E_SIG_IP    (-7)
#define VG_DISC_E_DIE       (-8)
#define VG_DISC_E_BOUNDS    (-9)

/* Checksum flags: a non-zero value means Linux would have rejected the blob. */
#define VG_DISC_F_BIN_SUM      (1u << 0)
#define VG_DISC_F_IP_SIG       (1u << 1)
#define VG_DISC_F_IP_SUM       (1u << 2)
#define VG_DISC_F_TABLE_BOUNDS (1u << 3)
#define VG_DISC_F_BASE_TRUNC   (1u << 4)
#define VG_DISC_F_NO_TABLE     (1u << 5)

struct vg_ip {
    uint16_t hw_id;
    uint8_t  inst;
    uint8_t  nbase;
    uint8_t  major, minor, revision;
    uint8_t  sub_revision, variant, harvest;
    uint64_t base[VG_MAX_BASES];
};

struct vg_disc {
    uint16_t bin_major, bin_minor;
    uint16_t ip_version;      /* version of the ip_discovery_header */
    uint16_t num_dies;
    uint8_t  base64;
    uint32_t nips;
    uint32_t flags;
    struct vg_ip ips[VG_MAX_IPS];
};

const char *vg_disc_strerror(int err);
int vg_disc_parse(const uint8_t *blob, uint32_t len, struct vg_disc *out);
const struct vg_ip *vg_disc_find(const struct vg_disc *d, uint16_t hw_id, uint8_t inst);
int vg_disc_reg(const struct vg_disc *d, uint16_t hw_id, uint8_t inst,
                uint8_t base_idx, uint32_t reg, uint32_t *out_dw);

#endif
