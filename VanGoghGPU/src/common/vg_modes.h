/* SPDX-License-Identifier: MIT */
#ifndef VG_MODES_H
#define VG_MODES_H

#include <stdint.h>

/* One fixed scanout mode. Geometry comes from boot-args or EDID. */
struct vg_mode {
    uint32_t width;             /* active pixels */
    uint32_t height;            /* active lines */
    uint32_t stride;            /* bytes per row */
    uint32_t hz;                /* integer refresh, informational */
    uint32_t pixel_clock_hz;    /* derived */
    uint32_t h_total, v_total;
    uint32_t h_sync, v_sync;    /* sync pulse width in pixels/lines */
    uint32_t aperture_bytes;    /* stride * height */
};

/* Steam Deck LCD panel nominal values. */
#define VG_PANEL_WIDTH   1280u
#define VG_PANEL_HEIGHT  800u
#define VG_PANEL_HZ      60u
#define VG_BYTES_PER_PIXEL 4u

void vg_mode_default(struct vg_mode *m);

/* Fill a mode from explicit geometry. Returns 0 on success. */
int vg_mode_build(uint32_t width, uint32_t height, uint32_t hz,
                  uint32_t stride, uint32_t extra_blank, struct vg_mode *out);

/* Parse one EDID detailed timing descriptor (18 bytes). Returns 0 on success. */
int vg_mode_from_edid(const uint8_t desc[18], struct vg_mode *out);

/* refreshRate field of IODisplayModeInformation: 16.16 fixed point. */
uint32_t vg_refresh_1616(const struct vg_mode *m);

#endif
