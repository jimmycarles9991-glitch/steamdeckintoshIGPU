# Register provenance

All offsets below are **dword offsets** (except where a byte offset is stated),
from Linux v6.12 at commit `adc218676eef25575469234709c2d87185ca223a`, in the
tree at `/home/tom/Downloads/steamdeckintosh/linux-vg-ref`.

Kernel headers are GPL-2.0. We quote only the numeric facts (name + offset)
and do not copy header source into this repository.

## Fixed MMIO registers (independent of IP discovery)

From `drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c:118-122`:

| Name | Dword offset | Use |
| --- | --- | --- |
| `mmMM_INDEX` | `0x0` | index for `MM_DATA` reads |
| `mmMM_DATA` | `0x1` | data read back |
| `mmMM_INDEX_HI` | `0x6` | high bits of the index |
| `mmRCC_CONFIG_MEMSIZE` | `0xde3` | VRAM size hint |
| `mmMP0_SMN_C2PMSG_33` | `0x16061` | IFWI/PSP status |
| `mmIP_DISCOVERY_VERSION` | `0x16a00` | discovery blob presence |

`src/common/vg_regs.h` defines these as `VG_MM_*`.

## IP discovery

`drivers/gpu/drm/amd/include/discovery.h`:

- `binary_header` v0/v1: signature u32, major/minor u16, checksum u16, size u16,
  then 6 × `table_info {offset u16, checksum u16, size u16, pad u16}`.
  v2 inserts `num_tables u16, pad u16` after the first 12 bytes.
- `ip_discovery_header`: signature u32 @0, version u16 @4, size u16 @6,
  id u32 @8, num_dies u16 @12, `die_info[16]` @14, flags u16 @78.
- `ip_v4` entry: 8 fixed bytes, then `num_base_address` × 4 (or × 8 when
  `base_addr_64_bit` is set).

`drivers/gpu/drm/amd/include/soc15_hw_ip.h` hardware IDs used here:
GC 11, MMHUB 34, HDP 41, SDMA0 42, SDMA1 43, MP0 255, DMU 271, DIO 272.

## DCN 3.0.1 display registers

File: `drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h`.
These are used by `src/probe/` as *read-only* samples; `src/fb/` does not
program them.

| Register | Dword offset | Base idx | Why |
| --- | --- | --- | --- |
| `mmOTG0_OTG_STATUS_FRAME_COUNT` | `0x1b4c` | 2 | panel refresh measurement |
| `mmHUBP0_DCSURF_SURFACE_CONFIG` | `0x05e5` | 2 | current surface format |
| `mmHUBPREQ0_DCSURF_SURFACE_PITCH` | `0x0607` | 2 | current row pitch |
| `mmHUBPREQ0_DCSURF_SURFACE_INUSE[_HIGH]` | `0x0621`/`0x0622` | 2 | current scanout address |
| `mmOTG0_OTG_H_TOTAL` / `mmOTG0_OTG_V_TOTAL` | `0x1b2a` / `0x1b2f` | 2 | current timing (read only) |
| `mmOTG0_OTG_CONTROL` | `0x1b41` | 2 | pipe enable state |

The exact numbers are in `src/common/vg_regs.h` as `VG_DCN_*`; run
`python3 tools/check_regs.py` to verify them against the fetched tree.
**Do not use DCN 3.1.2 offsets from Rembrandt here** — 3.0.1 and 3.1.2 differ.

## NBIO 7.2.0

File: `drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_7_2_0_offset.h`.
Used only for read-only identification in the probe.

## Verification

```sh
python3 tools/check_regs.py
```

reads `src/common/vg_regs.h`, extracts `VG_*` offsets with their cited header
paths, and checks each one against the fetched kernel tree. It fails if the
kernel tree is missing, so a build with stale numbers is visible.
