# Steam Deckintosh

Experimental macOS graphics-driver development for the AMD Van Gogh iGPU in the Steam Deck (PCI ID `1002:163F`).

## Current driver state

The kext now uses a `VanGoghDriver` PCI service. It validates the matched PCI ID, opens the PCI provider, enables PCI memory decoding as needed, and maps BAR5 using the OS-published resource length. It restores the prior memory-decoding state and releases the mapping on startup failure or detach. It publishes the GPU model, PCI IDs, revision, and 4096 MiB shared-memory size in IORegistry. A bounded 32-bit MMIO accessor is in place for later hardware stages.

This is not a working acceleration stack yet. The PCI service publishes the Van Gogh/Steam Deck identity and 4096 MiB shared-memory size, waits for PSP IFWI-ready, reads VRAM size, copies and validates the AMD IP discovery TMR through the BAR5 `MM_INDEX/MM_DATA` window, and validates optional PSP ASD/TOC containers without submitting them. It reads live MMHUB VMID0 and DCN301 HUBP, OTG, ODM, and MPCC state. Rotation defaults to 270 degrees on HUBPs resolved from the live panel path (HUBP0 and HUBP3 in the Linux capture); this only changes the HUBP rotation field and does not provide acceleration or a complete mode set. The kext owns a physically contiguous 1280x800 ARGB source buffer, but does not yet have a GPUVM mapping for it. The framebuffer remains guarded until the GPU address is mapped and the HUBP surface, clocks, and eDP stream are programmed and verified. The EDID path only forwards the Deck panel's captured EDID; it does not implement AUX/DDC hardware. Mode changes and backlight control are not implemented. No kext build or load has been verified on macOS. A complete Van Gogh GMC/VM and DCN pipe/eDP bring-up sequence is still required.

## Try rotating the currently active macOS display

The framebuffer transform helper requests a 270-degree rotation on the currently active built-in macOS display and waits for CoreGraphics to report the new orientation. This tests the macOS display transform independently of this kext. On macOS, from the repository root:

```sh
clang -Wall -Wextra -framework IOKit -framework ApplicationServices tools/rotate_display.c -o /tmp/rotate-display
/tmp/rotate-display 270
```

Restore the normal orientation with `/tmp/rotate-display 0`. The helper targets the built-in display and returns failure if CoreGraphics does not confirm the requested orientation within two seconds. This exercises macOS's active framebuffer transform; it does not test Van Gogh DCN initialization or this kext's guarded framebuffer service.

When loaded, the kext binds to the GPU and maps its register aperture. It may claim the PCI service and contend with another matching driver. Load it only on a recoverable macOS setup after building and reviewing the artifact. BAR addresses are runtime assigned; the driver discovers them through IOPCIFamily.

## Build

GitHub Actions builds an unsigned x86_64 bundle on a macOS runner and uploads it as an artifact. This Linux workspace cannot run Apple's `xcodebuild`. The workflow has not run until the project is pushed to a GitHub repository.

The local `VFCT.bin` is a hardware diagnostic dump, not a macOS driver resource. Do not install the current driver in OpenCore expecting graphics support.

## Linux research tools

- `python3 tools/linux_ip_inventory.py [PCI-BDF]` prints OS-assigned PCI resource ranges and numeric IP discovery entries from sysfs. It is read-only and does not require root.
- `tools/parse_ip_discovery.py BINARY --compare-sysfs PCI-BDF` validates an `amdgpu_discovery` blob's signatures, checksums, table and die bounds, then compares parsed entries with sysfs. Capturing the blob from Linux debugfs requires root; parsing a saved file does not.
- `python3 tools/linux_resolve_ip_register.py HWID INSTANCE BASE_IDX REGISTER_DWORD` calculates a direct BAR5 address from the live table and checks its full 4-byte range. Supply the base index and register dword from headers matching that IP version. It never maps or reads the register and refuses candidates outside BAR5.

Van Gogh is RDNA 2 (`gfx1033`), with GC `10.3.1`, PSP/SMU `11.5.0`, SDMA `5.2.1`, and DCN `3.0.1`. `nullarc-main` targets Phoenix/Hawk Point RDNA 3; its staged development pattern is useful, while its firmware, IP registers, memory setup, and display implementation do not apply to Van Gogh. See [hardware notes](hardware-notes.md) for the source trace, live captures, and mapping limits.
