/* SPDX-License-Identifier: MIT */
#ifndef VG_TYPES_H
#define VG_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint16_t vg_ld16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t vg_ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int vg_in_bounds(uint32_t off, uint32_t size, uint32_t len) {
    return off <= len && size <= len - off;
}

static inline uint16_t vg_byte_sum(const uint8_t *p, uint32_t n) {
    uint16_t s = 0;
    for (uint32_t i = 0; i < n; ++i)
        s = (uint16_t)(s + p[i]);
    return s;
}

#ifdef __cplusplus
}
#endif
#endif
