/* SPDX-License-Identifier: MIT */
/* Host tests for the freestanding C in src/common. No kext, no SDK. */
#include <stdio.h>
#include <string.h>
#include "vg_types.h"
#include "vg_discovery.h"
#include "vg_vfct.h"
#include "vg_modes.h"
#include "vg_bootargs.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void test_discovery(void) {
    uint8_t b[256];
    memset(b, 0, sizeof(b));
    const uint32_t ip_off = 0x60;
    const uint32_t die_off = ip_off + 80;
    const uint32_t ip_entry = 9;
    const uint32_t total = die_off + 4 + ip_entry;

    put32(b, 0x28211407u);
    put16(b + 4, 1);          /* binary major */
    put16(b + 6, 0);          /* minor */
    put16(b + 10, (uint16_t)total);
    /* table_info[0] */
    put16(b + 12, (uint16_t)ip_off);
    put16(b + 14, 0);         /* checksum, filled below */
    put16(b + 16, (uint16_t)(4 + 4 + 9));
    /* ip_discovery_header */
    put32(b + ip_off, 0x53445049u);
    put16(b + ip_off + 4, 3);       /* ip table version */
    put16(b + ip_off + 6, (uint16_t)(80 + 4 + ip_entry));
    put16(b + ip_off + 12, 1);      /* num_dies */
    put16(b + ip_off + 14, 0);      /* die id */
    put16(b + ip_off + 16, (uint16_t)die_off);
    /* die_header */
    put16(b + die_off, 0);
    put16(b + die_off + 2, 1);
    /* ip_v3: DMU, inst 0, 1 base, 3.0.1 */
    uint8_t *e = b + die_off + 4;
    put16(e, 271);
    e[2] = 0; e[3] = 1;
    e[4] = 3; e[5] = 0; e[6] = 1; e[7] = 0;
    put32(e + 8, 0x1234u);

    uint16_t ipsum = vg_byte_sum(b + ip_off, 80 + 4 + ip_entry);
    put16(b + 14, ipsum);
    uint16_t binsum = vg_byte_sum(b + 10, total - 10);
    put16(b + 8, binsum);

    struct vg_disc d;
    int rc = vg_disc_parse(b, sizeof(b), &d);
    CHECK(rc == VG_DISC_OK);
    CHECK(d.bin_major == 1);
    CHECK(d.ip_version == 3);
    CHECK(d.num_dies == 1);
    CHECK(d.nips == 1);
    CHECK(d.flags == 0);
    const struct vg_ip *ip = vg_disc_find(&d, VG_HWID_DMU, 0);
    CHECK(ip != NULL);
    if (ip) {
        CHECK(ip->major == 3 && ip->minor == 0 && ip->revision == 1);
        CHECK(ip->base[0] == 0x1234u);
        uint32_t dw = 0;
        CHECK(vg_disc_reg(&d, VG_HWID_DMU, 0, 0, 0x1b4cu, &dw) == 0);
        CHECK(dw == 0x1234u + 0x1b4cu);
    }
    /* Corrupt the binary signature: must be rejected. */
    b[0] = 0;
    CHECK(vg_disc_parse(b, sizeof(b), &d) == VG_DISC_E_SIGNATURE);
}

static void test_vfct(void) {
    uint8_t t[128];
    memset(t, 0, sizeof(t));
    memcpy(t, "VFCT", 4);
    put32(t + 4, 0x4cu + 28u + 4u);
    put32(t + 0x34, 0x4cu);
    uint8_t *r = t + 0x4c;
    put32(r, 0); put32(r + 4, 0); put32(r + 8, 0);
    put16(r + 12, 0x1002); put16(r + 14, 0x163f);
    put32(r + 24, 4);
    r[28] = 0x55; r[29] = 0xaa; r[30] = 0x55; r[31] = 0xaa;

    struct vg_vbios out;
    CHECK(vg_vfct_find(t, sizeof(t), 0, 0, 0, 0x1002, 0x163f, &out) == 0);
    CHECK(out.len == 4 && out.data[0] == 0x55 && out.data[3] == 0xaa);
    CHECK(vg_vfct_find(t, sizeof(t), 0, 0, 0, 0x1002, 0x1681, &out) != 0);
    CHECK(vg_vfct_find(NULL, 0, 0, 0, 0, 0x1002, 0x163f, &out) != 0);
}

static void test_modes(void) {
    struct vg_mode m;
    vg_mode_default(&m);
    CHECK(m.width == 800 && m.height == 1280);
    CHECK(m.stride == 3200);
    CHECK(m.aperture_bytes == 800u * 4u * 1280u);
    CHECK(vg_refresh_1616(&m) == (60u << 16));

    uint8_t edid[18];
    memset(edid, 0, sizeof(edid));
    put16(edid, 8019);       /* 80.19 MHz in 10 kHz units */
    edid[2] = 0x20; edid[3] = 0xf0; edid[4] = 0x30;   /* 800 + 240 blank */
    edid[5] = 0x00; edid[6] = 0x0a; edid[7] = 0x50;   /* 1280 + 10 blank */
    edid[8] = 48; edid[9] = 40; edid[10] = 0x06; edid[11] = 0x00;
    struct vg_mode em;
    CHECK(vg_mode_from_edid(edid, &em) == 0);
    CHECK(em.width == 800);
    CHECK(em.height == 1280);
    CHECK(em.pixel_clock_hz == 80190000u);
    CHECK(em.h_total == 1040 && em.v_total == 1290);
}

static void test_args(void) {
    const char *a = "-vgfbown -v  vgfb-width=1024 vgfb-offset=0x100000 foo=bar";
    CHECK(vg_arg_has(a, "-vgfbown") == 1);
    CHECK(vg_arg_has(a, "-vgfboff") == 0);
    CHECK(vg_arg_has(a, "-v") == 1);
    uint32_t v = 0;
    CHECK(vg_arg_u32(a, "vgfb-width", &v) == 0 && v == 1024);
    CHECK(vg_arg_u32(a, "vgfb-offset", &v) == 0 && v == 0x100000u);
    CHECK(vg_arg_u32(a, "nope", &v) != 0);
}

int main(void) {
    test_discovery();
    test_vfct();
    test_modes();
    test_args();
    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all VanGoghGPU host tests passed\n");
    return 0;
}
