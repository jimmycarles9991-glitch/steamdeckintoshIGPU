# Steam Deck hardware notes

Collected from the Linux system visible to this workspace on 2026-10-30. These identify the target; they are not a macOS framebuffer configuration.

## GPU

- System: Valve Steam Deck, board/product `Jupiter`, BIOS `F7A0131`.
- PCI function: `0000:04:00.0`, AMD Van Gogh / AMD Custom GPU 0405.
- PCI IDs: vendor/device `1002:163F`, subsystem `1002:0123`, revision `AE`.
- ACPI path reported by Linux: `\\_SB_.PCI0.GP17.VGA_`.
- Linux driver: `amdgpu`; kernel `6.18.44-1-lts`.
- VBIOS identifier: `113-AMDAerithJ-005`, fetched by Linux from the ACPI `VFCT` table.
- The local `VFCT.bin` copy is 45,188 bytes. Its matching `1002:163F` entry contains a 45,056-byte ATOMBIOS ROM (`VANGOGH`, `JupiterGener`); the extracted ROM's byte checksum is zero modulo 256.
- Linux-reported memory: 4096 MiB VRAM, 128-bit LPDDR5; the driver also creates a GTT aperture.
- Linux-reported display engine: DCN 3.0.1. Other detected blocks include GFX 10.x, SDMA 5.2, VCN 3.0, and JPEG 3.0.
- The boot log reports IP discovery for common, GMC 10, IH 5, PSP 11, SMU 11, display manager, GFX 10, SDMA 5.2, VCN 3, and JPEG 3. Linux loads the ATOM VBIOS through VFCT, initializes SMU and DMUB, then registers the amdgpu DRM device.
- The detailed boot log pins down the display bring-up baseline: Linux loads DMUB through PSP (`0x03000010`), initializes SMU successfully, starts Display Core `3.2.351` on DCN `3.0.1`, and reports DMUB hardware at the same version. It reserves a 10 MiB PSP TMR at `0xF4FE000000`. These are reference observations from Linux; the TMR address is runtime-assigned and must not be reused as a macOS constant.
- I ran RedNavi's read-only `tools/linux_ip_discovery.py 0x163f` against this Deck's live sysfs table. It confirms GC `10.3.1`, SDMA `5.2.1`, MP0/MP1 `11.5.0`, DMU `3.0.1`, VCN/UVD `3.0.2`, MMHUB `2.3.0`, NBIF `7.2.0`, and UMC `10.0.0` across four instances. Key discovery bases are GC `0x1260 / 0xA000 / 0x2402C00`, MP0/MP1 `0x16000 / 0x243FC00 / 0xDC0000 / 0xE00000 / 0xE40000`, and DMU `0x12 / 0xC0 / 0x34C0 / 0x9000 / 0x2403C00`. The same values are visible in amdgpu's sysfs `ip_discovery`; offsets are not PCI BAR addresses and must be resolved through the device's IP/register mapping.
- RedNavi is a Navi 32 implementation: its tested card has GC 11.0.3, PSP/SMU 13.0.10, SDMA 6.0.3, and DCN 3.2.0. The Deck is Van Gogh with GC 10.3.1, PSP/SMU 11.5.0, SDMA 5.2.1, and DCN 3.0.1. The useful carryover is its staged discovery and Linux capture workflow; its firmware images, register offsets, display replay, and boot arguments are not transferable to Van Gogh.
- The matching upstream Linux display implementation is `drivers/gpu/drm/amd/display/dc/resource/dcn301/dcn301_resource.c`, together with `vangogh_ip_offset.h`, DCN 3.0.1 register headers, and shared `dcn30` components. That resource pool explicitly marks DCN301 as an APU, enables DMUB support, and constructs the AUX engines, panel control, link encoders, timing generators, and planes. Its fixed resource table has four timing generators/planes and four stream encoders/DDC engines; actual connector routing still comes from this Deck's VBIOS and ACPI data. This source is MIT-licensed, but it depends on a large DC framework, so it is a source map for a port rather than a drop-in kext implementation.
- The log reports 4 GiB VRAM at `0xF400000000`, 1 GiB GART, a 1024 MiB GART aperture, and 5898 MiB GTT. These are Linux runtime allocations and addresses, not safe macOS values to copy directly.
- `lspci` reports BAR0 as a 256 MiB 64-bit prefetchable memory region, BAR2 as a 2 MiB 64-bit prefetchable region, BAR4 as 256 I/O ports, and BAR5 as a 512 KiB non-prefetchable memory region. BAR5 begins at `0x80300000`, matching the amdgpu log's register-MMIO base; the driver reports a 512 KiB MMIO size. Linux assigns these addresses, so macOS must discover/map its own resources rather than hard-code them.
- PCI capabilities include MSI-X with four vectors (table and PBA in BAR5), power management, ATS, PRI, and PASID. The `lspci` report says the endpoint link is 8 GT/s x16; the kernel is bound to `amdgpu`.
- Linux applies a panel backlight quirk (`min_brightness: 0`) and uses a custom brightness curve. This supports the need to investigate brightness behavior separately from basic eDP mode setting.
- Two `Failed to add display topology, DTM TA is not initialized` messages appear, but amdgpu continues to initialize and drives both displays. They are not evidence that the DRM display path failed.

## Built-in display

- Linux DRM connector: `eDP-1`, connected and enabled.
- EDID product string: `ANX7530 U`.
- EDID preferred timing: 800x1280 at approximately 59.999 Hz (69.27 MHz pixel clock).
- Linux exposes an `amdgpu_bl1` backlight device with a 0–65535 range.

The `modetest.txt` capture confirms the live KMS mode: CRTC 80 is driving the internal 800x1280 panel at 60.00 Hz, using framebuffer 101. The reported porch/sync values are 800/832/852/872 horizontally and 1280/1296/1298/1324 vertically, with a 69.270 MHz pixel clock. The `eDP-1` connector is ID 94, encoder 93, link status Good, panel orientation Right Side Up (enum 3), and max bpc range 8–16 (current value 10). The same capture also had the user's external `DP-1` desktop monitor (ID 103) connected at 1920x1080; the Steam Deck is their main PC, and this is not a second built-in panel.

The live debugfs DRM `state` dump confirms both displays are active simultaneously: CRTC 0 drives `eDP-1` at 800x1280@60 and CRTC 1 drives `DP-1` at 1920x1080@75. The panel's KWin framebuffer is 1280x800 AR24 with rotation value 8 (`DRM_MODE_ROTATE_270`, 270 degrees counter-clockwise); the external framebuffer is 1920x1080 AB30. This establishes that panel orientation is applied in the active scanout state, not just exposed as connector metadata. The `amdgpu_dm_dtn_log` reports encoded OTG totals 871x1323 for the panel and 2079x1118 for the external monitor (registers store total minus one), matching KMS totals 872x1324 and 2080x1119. It reports link training complete for both link encoders and no underflow. The log lists HUBP instances 0–3 as clock-enabled; without the full DC state-to-plane mapping, do not infer a simple one-HUBP-per-DRM-plane assignment from that table.

The KMS dump shows primary planes with XR24/AR24 and other pixel formats, plus AMD GFX10 RBPLUS tiled/DCC modifier support and a linear modifier. These are Linux DRM plane capabilities, not macOS framebuffer personality values; they do not by themselves establish a usable macOS driver.

The Steam Deck UI commonly presents the display in landscape orientation; the EDID timing above is the panel's reported orientation. A macOS port must also establish the actual panel link, display-engine setup, backlight behavior, and rotation path.

## ACPI findings (DSDT decompiled from this Deck)

- The GPU appears at `\\_SB_.PCI0.GP17.VGA_`, matching the PCI device path Linux reported.
- `GP17.VGA._STA` returns `0x0F` (present, enabled, functioning).
- `GP17.VGA._DOD` advertises seven display output IDs: `0x00010110`, `0x00010210`, `0x00010220`, `0x00010230`, `0x00010240`, `0x00031000`, and `0x00032000`.
- The child panel is `GP17.VGA.LCD_`, with `_ADR = 0x0110`.
- `_BCL` returns 52 entries: ACPI's two default levels (`0x5A`, `0x3C`) followed by even brightness steps `0x02` through `0x64`. `_BCM` writes the requested value to the global `BRTL` field.
- The DSDT contains another `VGA`/`LCD` ACPI device under `GPP0`; the PCI path above identifies the Steam Deck Van Gogh node as the `GP17` one.
- `iasl` emitted unresolved `^^^UBTC.*` namespace errors while disassembling the DSDT by itself, but it completed and produced `/tmp/DSDT.dsl`. Those messages concern missing external ACPI namespace context; the `GP17.VGA` methods were present in the generated DSL.
- A fresh dump of the live firmware tables included the DSDT and 12 SSDTs. Each table disassembled separately. The merged `iasl -da` pass produced the DSDT but reported unresolved `UBTC` references and duplicate names while trying to resolve SSDTs against one another; the individual SSDT DSL files were still generated with `iasl -d`.
- `SSDT9` adds AMD ACPI `ATPX` and `ATIF` methods under `GP17.VGA`. `ATPX` dispatches selectors `0x00`–`0x04`, `0x08`, `0x09`, `0x0A`, and `0x0B`; `ATIF` dispatches `0x00`–`0x04`, `0x0F`, and `0x10`. `SSDT8` adds `AFN0`–`AFN8`/`AFNC` wrappers and `ATC*` helper methods for this GPU namespace. This establishes that the Deck firmware exposes AMD ACPI control interfaces as well as the panel and brightness methods; it does not establish that a macOS driver should call them or that their payloads are compatible with Apple's drivers.

## VBIOS display routes

The VFCT image contains ATOM object-info table revision 1.4. Linux's AMD display BIOS parser uses this table's `number_of_path` and per-path connector/encoder IDs and resolves the record offsets against the table start.

- The first route has device tag `0x0002` (LCD1), connector object `0x3114`, encoder object `0x211E`, connector-record offset `0x28`, and encoder-record offset `0x3A`.
- The second route has device tag `0x0008` (DFP1), connector object `0x3113`, encoder object `0x2120`, connector-record offset `0x31`, and encoder-record offset `0x41`.
- The LCD1 connector record has I2C/AUX ID `0x90` and HPD pin ID `1`; the DFP1 record has I2C/AUX ID `0x92` and HPD pin ID `3`. Both have encoder-capability records.
- The VBIOS confirms a separate LCD1 internal-panel route and DFP1 digital-display route. Combined with Linux's `eDP-1` connector and the panel EDID, this gives us the Deck's relevant firmware display topology. The object IDs and firmware records still need to be interpreted within the AMD display engine; they are not a macOS framebuffer personality by themselves.
- The master data table points `LCD_Info` to offset `0x02C8`, whose header is size `0x50`, format/content revision `3.1`. The public AMD/Linux headers available in this workspace describe other LCD info revisions, not this 3.1 layout, so I have not guessed timing, link-rate, or power-sequence fields from it. The EDID remains the reliable panel timing source in this capture.

## iGPUTalks comparison

The neighboring `iGPUTalks-main` README claims support for Radeon 660M and 700M-series APUs, but the source currently present here is only an `IOService` probe/start stub. Its `start()` method logs PCI vendor/device/revision and registers the service; it contains no AMD GPU initialization, framebuffer setup, firmware parsing, memory management, or acceleration implementation. Its personality matches AMD display-class PCI devices broadly. The Xcode project compiles only `iGPUTalks.cpp`; the referenced `MacKernelSDK` directory is absent from this checkout, and there is no bundled Lilu integration despite a small unused header referring to it. The README says BSD-3-Clause, but this checkout has no license file.

The 660M is a reasonable architectural comparison: AMD lists it as a six-core Radeon iGPU, while this Deck's Linux driver reports eight active CUs and Van Gogh-specific GFX/SDMA/VCN/DCN blocks. AMD's open ROCm architecture map distinguishes Van Gogh as `gfx1033` from Rembrandt as `gfx1035`, which is the family used for 660M-class Ryzen 6000 APUs. Shared RDNA 2 lineage makes the 660M a useful hardware reference, but support for it would not automatically establish compatibility with Van Gogh's different IP configuration, VBIOS, memory setup, and Steam Deck display wiring. The local iGPUTalks code does not yet demonstrate working support for either GPU.

## Capture limits

- The Linux kernel log confirms that Van Gogh initializes its GPU, memory, video, and display blocks successfully under `amdgpu`.
- `modetest.txt` was captured on the actual Linux host and contains live connector/CRTC/plane data.
- `amdgpu-boot-log.txt` captures the Linux initialization path and its firmware/IP block messages.
- `pci-gpu.txt` captures the PCI configuration and BAR resource ranges.
- On the live Deck, debugfs is mounted and the GPU directory is `/sys/kernel/debug/dri/0000:04:00.0/`. It exposes read-only `state`, `amdgpu_dm_dtn_log`, `amdgpu_dm_dmub_fw_state`, `amdgpu_firmware_info`, and `amdgpu_discovery` entries. The raw `amdgpu_regs*` interfaces are also present, but do not use them for a broad dump until a DCN 3.0.1 active-path register list is prepared.
- In the pasted capture, `amdgpu_dm_dmub_fw_state` returned `Operation not permitted` even from the root shell; `amdgpu_discovery` emitted binary data when read with `cat`. Use the text sysfs `ip_discovery` interface or the existing decoder for discovery data, and don't treat either failed/raw read as a decoded diagnostic.
- The VFCT payload has been copied into this project as `VFCT.bin` and parsed for the display routes summarized above. Keep this firmware dump local unless you intend to share its VBIOS contents.
- The framebuffer sysfs values visible here are not a reliable live KMS mode report, so they are intentionally omitted.

The next useful Linux capture is a carefully scoped register/state snapshot for the active eDP path, to correlate the panel route with the working KMS mode. RedNavi's `linux_collect.py` and `linux_dcn_pipe_dump.py` are not suitable for this: they use a DCN 3.2 register list and their DIG selection assumes Navi 32 wiring. A Van Gogh capture needs the DCN 3.0.1 register definitions and active path from the matching Linux driver. The upstream DCN301 resource file is the starting point for identifying the register groups; the Deck's `modetest.txt` already provides the live mode and connector state. Limit reads to registers the active path has powered; RedNavi's own bring-up notes report that broad reads of gated display blocks can stall a machine. Do not treat Linux DRM plane modifiers as macOS framebuffer settings.
