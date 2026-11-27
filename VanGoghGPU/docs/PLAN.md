# Plan

Three phases. Each phase has an exit criterion that is observable on hardware;
do not start a phase before the previous one's criterion is met.

## P0 — Measure (exit: a dump exists)

Load the probe kext. Run `tools/collect.sh` under Linux. Record:

- BAR0/BAR2 physical addresses and sizes
- IP discovery blob (hex) or at least the parsed versions/bases
- VFCT binary and its matching VBIOS image
- EDID of the eDP panel
- `ioreg -rc VanGoghProbe` and `ioreg -rc IOFramebuffer` from macOS
- the exact macOS version and OpenCore version

Nothing in P1 is trusted until these exist. In particular, the framebuffer
offset and stride are *not* assumed to be 0 / 5120 without a measurement.

## P1 — A picture (exit: CPU-composited desktop at native size)

Take over the GOP framebuffer without touching DCN. Implemented in `src/fb/`.

Steps:

1. Boot with `-vgfboff` and confirm Apple's `IONDRVFramebuffer` is the only
   `IOFramebuffer`.
2. Boot with `-vgfbown` and the geometry from P0. Check the log line that says
   which aperture was chosen.
3. If garbage: change `vgfb-offset`, `vgfb-width`, `vgfb-height`,
   `vgfb-stride` in boot-args. No rebuild needed.
4. If two displays appear: the displacement failed; adjust `IOProbeScore`
   and/or `IOMatchCategory` in `src/fb/Info.plist`, and make
   `getConnectionCount()` return 0 when we did not win the category.
5. Once the desktop draws, add backlight via whatever PWM mechanism P0 found
   (probably DCN3 `PANEL_CNTL` / `BACKLIGHT_CNTL` — not implemented).

The desktop will be slow: WindowServer renders on the CPU. That is expected
and is not a failure of P1.

## P2 — Acceleration (not started)

Two candidate routes, in order of plausibility:

### Route A — Apple's RDNA2 stack

Apple's `AMDRadeonX6000` supports RDNA2 desktop parts, and NootRX shows a way
to add unsupported device IDs. VanGogh has GC 10.3.x and DCN 3.0.1; Apple's
personalities encode Navi 2x IP versions and framebuffer buffers. Work needed:

- enumerate `AMDRadeonX6000FB` personalities in the target macOS version
- find one whose DCN generation is closest to 3.0.1
- patch device ID matching, `AGDC`/`ATY` properties, IP-version checks and
  firmware loading; supply the VFCT ROM from P0
- accept that Metal may still fail because the shader compiler has no
  gfx1037 back end

### Route B — Native driver + Mesa

Follow RedNavi/nullarc: port `amdgpu` register logic into a kext, run RADV in
userspace, translate Metal→SPIR-V. This is the only route with a known
general solution for an unsupported GC generation, but it is months of work
and requires a trusted userspace component.

**OpenGL-only is not a shortcut**: since macOS 10.14, Apple's OpenGL is a
Metal client. Without Metal there is no Apple GL. Route B could expose
OpenGL through Mesa directly, but that is the same amount of work as Route A
plus a full userspace GL stack.

## Rules we follow

1. No boot-time writes to hardware that we cannot undo from a disarm path.
2. A framebuffer kext must be opt-in until it has survived a real boot.
3. Every register offset cites a Linux header; no magic numbers.
4. Host tests run before any kext build.
5. Published logs state *which* of several possible paths was taken, so
   "did not run" and "ran and failed" are distinguishable.
6. No firmware, VBIOS or microcode is vendored.
