/* SPDX-License-Identifier: MIT */
/*
 * Register offsets for VanGogh. Every value is a dword offset unless the name
 * says BYTE. Sources are Linux v6.12 (tree at linux-vg-ref), quoted as facts;
 * no kernel source is copied here. See docs/REGISTERS.md.
 *
 * Fixed MMIO:   drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c:118-122
 * DCN 3.0.1:    drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h
 * HW IDs:       drivers/gpu/drm/amd/include/soc15_hw_ip.h
 */
#ifndef VG_REGS_H
#define VG_REGS_H

#include <stdint.h>

/* Fixed offsets in the MMIO BAR (dwords). */
#define VG_MM_MM_INDEX           0x00000u
#define VG_MM_MM_DATA            0x00001u
#define VG_MM_MM_INDEX_HI        0x00006u
#define VG_MM_RCC_CONFIG_MEMSIZE 0x00de3u
#define VG_MM_MP0_C2PMSG_33      0x16061u
#define VG_MM_IP_DISCOVERY_VER   0x16a00u

/* DCN 3.0.1, base index 2 (DCN block). Dword offsets. */
#define VG_DCN_HUBP0_SURF_CONFIG      0x05e5u
#define VG_DCN_HUBPREQ0_SURF_PITCH    0x0607u
#define VG_DCN_HUBPREQ0_FLIP_INTR     0x0620u
#define VG_DCN_HUBPREQ0_SURF_INUSE    0x0621u
#define VG_DCN_HUBPREQ0_SURF_INUSE_HI 0x0622u
#define VG_DCN_OTG0_H_TOTAL           0x1b2au
#define VG_DCN_OTG0_H_BLANK           0x1b2bu
#define VG_DCN_OTG0_H_SYNC_A          0x1b2cu
#define VG_DCN_OTG0_V_TOTAL           0x1b2fu
#define VG_DCN_OTG0_V_BLANK           0x1b36u
#define VG_DCN_OTG0_V_SYNC_A          0x1b37u
#define VG_DCN_OTG0_CONTROL           0x1b41u
#define VG_DCN_OTG0_FRAME_COUNT       0x1b4cu
#define VG_DCN_BASE_IDX               2u

/* Hardware IDs from soc15_hw_ip.h. */
#define VG_HWID_GC       11u
#define VG_HWID_MMHUB    34u
#define VG_HWID_HDP      41u
#define VG_HWID_SDMA0    42u
#define VG_HWID_SDMA1    43u
#define VG_HWID_MP0      255u
#define VG_HWID_DMU      271u
#define VG_HWID_DIO      272u

#define VG_HWID_MAX      512u
#define VG_MAX_IPS       96u
#define VG_MAX_BASES     8u

#endif
