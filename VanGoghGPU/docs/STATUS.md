# Status — honest version

Last updated: 2026-11-27. No hardware testing has been performed by the author.

## Implemented and host-tested

- IP-discovery binary parser (v1–v4, 32/64-bit bases, checksum flags).
- VFCT VBIOS table parser.
- Boot-argument parser (`-vgfboff`, `-vgfbown`, `vgfb-*` values).
- Panel mode table and timing arithmetic (1280×800@60 default, EDID-derived
  overrides).
- `make hosttest` on Linux and macOS, with ASan/UBSan where available.

## Implemented but untested on hardware

- `VanGoghProbe.kext`: matches `1002:163f`, reads PCI config, maps BAR0/BAR2,
  reads `mmIP_DISCOVERY_VERSION`, walks MM_INDEX/MM_DATA to fetch the discovery
  blob, parses it, reads a few fixed DCN registers and publishes everything as
  IORegistry properties. Read-only. VFCT is parsed by the host-side C library
  and extracted on Linux by `tools/collect.sh`; the kext does not reach into
  AppleACPIPlatform's private table cache.
- `VGAMDFramebuffer.kext`: an `IOFramebuffer` subclass with one fixed mode,
  BAR0 sub-range aperture, timer-driven VBL, and boot-arg overrides. It is
  opt-in (`-vgfbown`) and refuses with `-vgfboff`.

## Not implemented

- Any DCN modeset, link training, PLL programming or DMCUB loading.
- Backlight control (the GOP's setting is left alone).
- Cursor plane, gamma correctness, display sleep/wake, hotplug, external DP.
- Metal, OpenGL, OpenCL, video decode, GFX/SDMA ring bring-up.
- Lilu plugin to patch Apple's `AMDSupport`/`AMDRadeonX6000` binding.
- SIP/codesigning automation beyond docs.

## Known probable defects

1. **Framebuffer address.** The kext assumes the GOP scanout is at
   `BAR0 + vgfb-offset` (default 0). On many AMD APUs it is; on this one it
   must be measured. Wrong value = garbage or black screen. The probe logs
   BAR0 size to help.
2. **Displacement.** `VGAMDFramebuffer` matches `IONameMatch` `display` and
   `IOMatchCategory` `IOFramebuffer` with a score above Apple's
   `IONDRVFramebuffer`. If Apple's boot framebuffer does not match that way on
   this machine, both drivers may load and WindowServer will see two screens.
   Check `ioreg -rc IOFramebuffer` after booting with `-vgfboff`.
3. **VBL is a timer, not the real OTG interrupt.** It will drift under load.
   It is needed for WindowServer to drain its transaction queue.
4. **Mode timing.** 1280×800@60 is the LCD spec. The EDID may report a
   different blanking interval; `tools/collect.sh` extracts it.
5. **`unregisterInterrupt`** uses a simple slot table of four; no dynamic
   allocation in interrupt context.

## What a successful P1 test looks like

With `-vgfbown`:

```
ioreg -rc VGAMDFramebuffer     # one instance, provider is the 1002:163f PCI device
log show --last boot | grep VGAMD  # aperture, BAR0, IP versions, VFCT status
```

and the desktop appears at native resolution, drawn by CPU. If the screen is
frozen, reboot with `-vgfboff` and read the logs; do not debug from a black
screen.

## How to help

Run the probe and `tools/collect.sh`, then open an issue with:

- `ioreg -rc VanGoghProbe` output from macOS
- the `vangogh-dump/` directory produced by `tools/collect.sh` on Linux
- `log show --last boot --predicate 'eventMessage CONTAINS "VanGoghGPU"'`
