# VanGogh hardware facts

Every fact here is traceable to the Linux v6.12 tree fetched into
`/home/tom/Downloads/steamdeckintosh/linux-vg-ref`. Paths are relative to that
tree. Nothing in this file is guessed.

## Identity

| Item | Value | Source |
| --- | --- | --- |
| PCI vendor / device | `1002:163f` | ArchWiki, Steam Deck (LCD/Jupiter); `lspci` on the Deck |
| Codename | VANGOGH (`CHIP_VANGOGH`) | `drivers/gpu/drm/amd/amdgpu/amdgpu_device.c:139` |
| Marketing name | AMD Custom GPU 0405 | `clipsandcheese.com` hardware analysis |
| Architecture | RDNA2, 8 CUs / 512 FP32 lanes, up to 1.6 GHz | Chester Lam, *Van Gogh, AMD's Steam Deck APU* |
| GC (graphics) IP | GC 10.3.x (Linux `gfx_v10_0.c`, `golden_settings_gc_10_3_vangogh`) | `drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c:3425` |
| Display IP | **DCN 3.0.1** (`dcn/dcn_3_0_1_offset.h`) | `drivers/gpu/drm/amd/display/dc/resource/dcn301/dcn301_resource.c:66` |
| NBIO | 7.2.0 (`nbio/nbio_7_2_0_offset.h`) | same file, line 73 |
| PSP | 11.0 (`psp_v11_0.c`, `vangogh_asd.bin`, `vangogh_toc.bin`) | `drivers/gpu/drm/amd/amdgpu/psp_v11_0.c` |
| SDMA | 5.2 (`sdma_v5_2.c`, `vangogh_sdma.bin`) | `drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c:52` |
| Display firmware | `amdgpu/vangogh_dmcub.bin` | `drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c:124` |
| SMU | DCN301 SMU (`clk_mgr/dcn301/dcn301_smu.c`) | `drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/` |
| Panel (LCD model) | 1280×800 eDP, 60 Hz | Valve spec; confirm with `tools/collect.sh` |
| Memory | 16 GB LPDDR5 shared (APU), GPU carveout set by BIOS | Valve spec; measure with the probe |

## Why DCN 3.0.1 matters

DCN 3.0.1 is *not* DCN 3.1.2. Radeonized's Rembrandt framebuffer uses
`dcn_3_1_2_offset.h`; those offsets cannot be used blindly here. The blocks
that matter to a GOP-takeover framebuffer are mostly read-only (OTG frame
count, HUBP surface address, DMMODE), and their register numbers are listed in
`docs/REGISTERS.md` with the exact 3.0.1 header they came from.

A full modeset from scratch would need DML 3.01 bandwidth calculations
(`drivers/gpu/drm/amd/display/dc/dml/dcn301/`), a bounding box
(`dcn3_01_soc`, `dcn3_01_ip` in `dcn301_resource.c`), PLL setup, DMCUB
bring-up and link training. This project deliberately does **not** do a
modeset: it assumes the EFI GOP has already lit the panel and merely exposes
the same scanout memory to WindowServer.

## IP discovery

Linux identifies blocks by hardware ID (`drivers/gpu/drm/amd/include/soc15_hw_ip.h`):

| HW ID | Block |
| --- | --- |
| 11 | GC |
| 34 | MMHUB |
| 41 | HDP |
| 42 / 43 | SDMA0 / SDMA1 |
| 255 | MP0 (PSP) |
| 271 | DMU (display) |
| 272 | DIO |

A VanGogh IP-discovery blob is version 4 or later in practice (`ip_v4` with
optional 64-bit base addresses) — see
`drivers/gpu/drm/amd/include/discovery.h:128`. `src/common/vg_discovery.c`
parses versions 1–4 and verifies table checksums the same way
`amdgpu_discovery_table_check()` does.

`mmIP_DISCOVERY_VERSION` is at fixed dword offset `0x16A00` in the MMIO BAR;
the blob itself is reached through the `MM_INDEX`/`MM_INDEX_HI`/`MM_DATA`
window (`amdgpu_discovery.c:118-122`). The probe kext does exactly that, with
reads only.

## VBIOS

APUs have no ROM to read the VBIOS from. Linux gets it from the ACPI `VFCT`
table (`drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c:347-400`). The layout is in
`drivers/gpu/drm/amd/include/atombios.h:9278-9296`:

```
offset 0x00: ACPI header (36 bytes)
offset 0x24: 16-byte UUID
offset 0x34: u32 VBIOSImageOffset
offset 0x38: u32 Lib1ImageOffset
offset 0x4c: repeated { bus u32, device u32, function u32, vendor u16,
                       device u16, ssvid u16, ssid u16, revision u32,
                       image_length u32, image bytes... }
```

`src/common/vg_vfct.c` parses that. It is used by the probe to report whether
a usable ROM is present, and later by an accelerated driver (not implemented)
to supply `ATY,bin_image`.

## Open questions the probe answers

1. Which BAR is the register window on this machine (Linux uses BAR2 for MMIO
   on APUs, but Apple's IOKit view may differ)?
2. How large is BAR0, and is the GOP framebuffer at offset 0?
3. What are the actual DCN/GC IP versions and base addresses?
4. Is there a VFCT table with a matching `1002:163f` image?
5. What is the panel's real pixel clock and blanking (EDID)?

Until these are answered with real numbers from a Deck, the framebuffer
kext's default geometry is a documented guess.
