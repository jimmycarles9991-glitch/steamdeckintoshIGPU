#!/usr/bin/env python3
"""Check every VG_* register constant against the pinned Linux reference tree."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REGS = ROOT / "src/common/vg_regs.h"
LINUX = ROOT.parent / "linux-vg-ref"

# VG constant -> (kind, Linux name, relative path)
MAP = {
    "VG_MM_MM_INDEX": ("c", "mmMM_INDEX", "drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c"),
    "VG_MM_MM_DATA": ("c", "mmMM_DATA", "drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c"),
    "VG_MM_MM_INDEX_HI": ("c", "mmMM_INDEX_HI", "drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c"),
    "VG_MM_RCC_CONFIG_MEMSIZE": ("c", "mmRCC_CONFIG_MEMSIZE", "drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c"),
    "VG_MM_MP0_C2PMSG_33": ("c", "mmMP0_SMN_C2PMSG_33", "drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c"),
    "VG_MM_IP_DISCOVERY_VER": ("c", "mmIP_DISCOVERY_VERSION", "drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c"),
    "VG_DCN_HUBP0_SURF_CONFIG": ("h", "mmHUBP0_DCSURF_SURFACE_CONFIG", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_HUBPREQ0_SURF_PITCH": ("h", "mmHUBPREQ0_DCSURF_SURFACE_PITCH", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_HUBPREQ0_FLIP_INTR": ("h", "mmHUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_HUBPREQ0_SURF_INUSE": ("h", "mmHUBPREQ0_DCSURF_SURFACE_INUSE", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_HUBPREQ0_SURF_INUSE_HI": ("h", "mmHUBPREQ0_DCSURF_SURFACE_INUSE_HIGH", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_OTG0_H_TOTAL": ("h", "mmOTG0_OTG_H_TOTAL", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_OTG0_H_BLANK": ("h", "mmOTG0_OTG_H_BLANK_START_END", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_OTG0_H_SYNC_A": ("h", "mmOTG0_OTG_H_SYNC_A", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_OTG0_V_TOTAL": ("h", "mmOTG0_OTG_V_TOTAL", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_OTG0_V_BLANK": ("h", "mmOTG0_OTG_V_BLANK_START_END", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_OTG0_V_SYNC_A": ("h", "mmOTG0_OTG_V_SYNC_A", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_OTG0_CONTROL": ("h", "mmOTG0_OTG_CONTROL", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
    "VG_DCN_OTG0_FRAME_COUNT": ("h", "mmOTG0_OTG_STATUS_FRAME_COUNT", "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"),
}

def main():
    if not LINUX.is_dir():
        print(f"Linux reference tree missing: {LINUX}")
        print("run tools/fetch_linux_ref.sh first")
        return 2
    text = REGS.read_text()
    bad = 0
    for name, (kind, lname, rel) in MAP.items():
        m = re.search(rf"#define\s+{name}\s+0x([0-9a-fA-F]+)u", text)
        if not m:
            print(f"{name}: not defined in vg_regs.h")
            bad += 1
            continue
        value = int(m.group(1), 16)
        path = LINUX / rel
        if not path.is_file():
            print(f"{name}: reference file missing: {rel}")
            bad += 1
            continue
        pat = rf"#define\s+{lname}\s+0x([0-9a-fA-F]+)"
        if kind == "c":
            pat = rf"#define\s+{lname}\s+0x([0-9a-fA-F]+)"
        found = re.search(pat, path.read_text())
        if not found:
            print(f"{name}: {lname} not found in {rel}")
            bad += 1
            continue
        ref = int(found.group(1), 16)
        if ref != value:
            print(f"{name}: ours 0x{value:x}, Linux 0x{ref:x} ({rel})")
            bad += 1
    if bad:
        print(f"{bad} register mismatch(es)")
        return 1
    print(f"all {len(MAP)} register constants match Linux v6.12")
    return 0

if __name__ == "__main__":
    sys.exit(main())
