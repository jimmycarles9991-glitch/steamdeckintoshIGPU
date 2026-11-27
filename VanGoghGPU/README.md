# VanGoghGPU

**Display bring-up for the Steam Deck LCD's VanGogh iGPU on macOS.**

Target: `1002:163f` (AMD Custom GPU 0405, VanGogh, RDNA2 / GC 10.3.x / DCN 3.0.1),
1280×800 eDP panel, booted through OpenCore.

> **Read this before installing anything.** This is a research project, not a
> finished driver. As of this commit it contains a read-only probe kext and a
> *first attempt* at a framebuffer kext that takes over the firmware-lit GOP
> scanout. It has **no Metal, no OpenGL, no 3D acceleration, and no video
> decode**. It has never been booted on real hardware by the author (who does
> not own a Deck). Treat every claim here as a hypothesis to be tested. See
> [docs/STATUS.md](docs/STATUS.md) for exactly what is and is not implemented.

## Why this is not inside NootedRed

`NootedRed-master` in this workspace is licensed under the ChefKiss Open
Knowledge License v1, which says:

> This work may not be used to train or develop artificial intelligence,
> and may not be used with artificial intelligence in any other way.

So this project does not copy from, or get committed into, that tree. It is a
new MIT-licensed tree. The technique it explores (take over the GOP framebuffer;
only read DCN registers) is described in Radeonized's own comments, which are
MIT licensed and expressly permit this kind of use. Register offsets come from
the Linux kernel source, cited in `docs/REGISTERS.md`; only numeric facts are
quoted, no GPL source is copied into this repository.

## What is here

| Path | What it is |
| --- | --- |
| `src/common/` | Freestanding C: IP-discovery parser, VFCT VBIOS parser, panel mode/timing math, boot-arg parser. Host-tested. |
| `src/probe/` | `VanGoghProbe.kext` — read-only PCI/BAR/IP-discovery diagnostic kext. This one is safe to load first. VFCT is extracted on the Linux side by `tools/collect.sh`. |
| `src/fb/` | `VGAMDFramebuffer.kext` — an `IOFramebuffer` subclass that displaces Apple's `IONDRVFramebuffer` and exposes the firmware framebuffer. **Unproven.** |
| `tools/collect.sh` | Runs on the Deck under Linux and dumps everything we need to make the framebuffer kext hardware-correct. |
| `tools/hosttest.c` | Tests that run on any Linux/macOS host, no kext needed. |
| `third_party/linux-vg-ref` | The Linux v6.12 amdgpu/display source tree fetched for reference (see below). |
| `docs/` | Hardware facts, register provenance, a phased plan, and an honest status page. |

The fetched reference kernel tree lives at
`/home/tom/Downloads/steamdeckintosh/linux-vg-ref` (536 MB, git worktree at
v6.12), because putting a full kernel checkout inside the kext source tree is
not useful. `tools/fetch_linux_ref.sh` recreates it elsewhere.

## Reality check

Getting a usable desktop on this APU means, in order:

1. **P0 — know the machine.** Load the probe, run `tools/collect.sh`, get real
   BAR sizes, IP-discovery bases, the VFCT ROM and the panel's EDID. Nothing
   below can be trusted without this.
2. **P1 — a picture.** Take over the already-lit GOP framebuffer and register
   it as an `IOFramebuffer`. This is what `src/fb/` attempts. WindowServer
   composites on the CPU, so expect a slow desktop, but windows should draw.
3. **P2 — acceleration.** Either spoof VanGogh into Apple's `AMDRadeonX6000`
   stack (unlikely: GC 10.3.x and DCN 3.0.1 are not Navi 2x), or port the
   Linux `amdgpu` + Mesa RADV route used by RedNavi/nullarc (months of work).
   OpenGL on modern macOS goes through Metal, so "OpenGL only" is not a
   shortcut unless you also ship a userspace GL driver.

We are at P1's first draft. P2 is not implemented and is not promised.

## Safety

- Build and load the **probe** first. It only reads registers.
- The framebuffer kext is off unless you add `-vgfbown` to boot-args. That is
  deliberate: it takes the panel away from Apple's boot framebuffer.
- `-vgfboff` always disables it, even with `-vgfbown` present.
- Keep a way back (SSH, another machine, Windows/Linux boot, OpenCore
  recovery). A framebuffer that takes the panel and then fails leaves a screen
  frozen on the last frame.
- Unsigned kexts require `csr-active-config` / SIP relaxed in OpenCore.

## Building

On a Mac with Xcode Command Line Tools:

```sh
sh tools/fetch_deps.sh          # MacKernelSDK + Lilu, if not already present
make hosttest                   # pure C tests, no kext, no SIP changes
make probe                      # build/out/VanGoghProbe.kext
make fb                         # build/out/VGAMDFramebuffer.kext
```

On Linux (this workspace) only `make hosttest` works:

```sh
make hosttest CC=clang
```

The kext targets use `xcrun` and Apple's kernel headers and are expected to
fail on Linux. That is not a bug in the code, it is a missing SDK.

## Installing

See [docs/INSTALL.md](docs/INSTALL.md). Short version: put both kexts in
`EFI/OC/Kexts/`, add them to `config.plist` after Lilu, keep `-vgfboff` until
you have read the probe output, then swap to `-vgfbown` for one boot.

## License

Our code is MIT. See [LICENSE](LICENSE). `third_party/` contains upstream
projects with their own licenses; `third_party/README.md` says which.

Linux register offsets quoted in this project come from GPL-2.0 kernel headers.
They are facts (numbers and names), not copied source, and the upstream tree is
fetched rather than vendored. No AMD microcode, VBIOS blob or firmware image is
included: the kext reads the machine's own VFCT table at runtime.
