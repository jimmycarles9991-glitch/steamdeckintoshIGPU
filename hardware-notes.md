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
- The VFCT payload has been copied into this project as `VFCT.bin` and parsed for the display routes summarized above. Keep this firmware dump local unless you intend to share its VBIOS contents.
- The framebuffer sysfs values visible here are not a reliable live KMS mode report, so they are intentionally omitted.

The next useful Linux capture would be the relevant kernel amdgpu display initialization logs around boot and panel enable, to correlate firmware routing with the working eDP link. Do not treat Linux DRM plane modifiers as macOS framebuffer settings.
