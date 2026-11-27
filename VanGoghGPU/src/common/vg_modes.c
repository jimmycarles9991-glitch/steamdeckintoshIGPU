/* SPDX-License-Identifier: MIT */
#include "vg_modes.h"
#include "vg_types.h"

void vg_mode_default(struct vg_mode *m) {
    m->width = VG_PANEL_WIDTH;
    m->height = VG_PANEL_HEIGHT;
    m->hz = VG_PANEL_HZ;
    m->stride = VG_PANEL_WIDTH * VG_BYTES_PER_PIXEL;
    /* Nominal blanking for 800x1280@60: htotal 1040, vtotal 1290 (~80.5 MHz). */
    m->h_total = 1040; m->v_total = 1290;
    m->h_sync = 40; m->v_sync = 6;
    m->pixel_clock_hz = 1040u * 1290u * VG_PANEL_HZ;
    m->aperture_bytes = m->stride * m->height;
}

int vg_mode_build(uint32_t width, uint32_t height, uint32_t hz,
                  uint32_t stride, uint32_t extra_blank, struct vg_mode *out) {
    if (!out || width == 0 || height == 0 || hz == 0 ||
        width > 8192 || height > 8192 || hz > 600) return -1;
    if (stride == 0) stride = width * VG_BYTES_PER_PIXEL;
    if (stride < width * VG_BYTES_PER_PIXEL || stride > 65536) return -1;
    if (extra_blank > 4096) return -1;
    out->width = width;
    out->height = height;
    out->hz = hz;
    out->stride = stride;
    out->h_total = width + 240u + extra_blank;
    out->v_total = height + 10u;
    out->h_sync = 40;
    out->v_sync = 6;
    out->pixel_clock_hz = out->h_total * out->v_total * hz;
    out->aperture_bytes = stride * height;
    return 0;
}

int vg_mode_from_edid(const uint8_t d[18], struct vg_mode *out) {
    if (!d || !out) return -1;
    uint32_t pclk10k = vg_ld16(d);
    uint32_t ha = (uint32_t)d[2] | ((uint32_t)(d[4] >> 4) << 8);
    uint32_t hb = (uint32_t)d[3] | ((uint32_t)(d[4] & 0xf) << 8);
    /* Byte 7: bits 7-4 are vertical active high, bits 3-0 blanking high. */
    uint32_t va = (uint32_t)d[5] | ((uint32_t)(d[7] >> 4) << 8);
    uint32_t vb = (uint32_t)d[6] | ((uint32_t)(d[7] & 0xfu) << 8);
    if (ha == 0 || va == 0 || pclk10k == 0) return -1;
    struct vg_mode m;
    m.width = ha;
    m.height = va;
    m.h_total = ha + hb;
    m.v_total = va + vb;
    m.h_sync = (uint32_t)d[9] | ((uint32_t)(d[12] >> 6) << 8);
    m.v_sync = (uint32_t)(d[10] & 0xfu) | (((uint32_t)(d[12] >> 4) & 3u) << 4);
    m.pixel_clock_hz = pclk10k * 10000u;
    m.hz = (m.h_total && m.v_total)
         ? m.pixel_clock_hz / (m.h_total * m.v_total) : 0;
    m.stride = ha * VG_BYTES_PER_PIXEL;
    m.aperture_bytes = m.stride * va;
    *out = m;
    return 0;
}

uint32_t vg_refresh_1616(const struct vg_mode *m) {
    if (!m || m->pixel_clock_hz == 0 || m->h_total == 0 || m->v_total == 0) return 0;
    uint64_t num = (uint64_t)m->pixel_clock_hz << 16;
    uint64_t den = (uint64_t)m->h_total * m->v_total;
    if (den == 0) return 0;
    uint64_t q = num / den;
    return q > 0xffffffffull ? 0xffffffffu : (uint32_t)q;
}
