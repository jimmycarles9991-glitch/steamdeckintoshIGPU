# Steam Deckintosh

Experimental macOS graphics-driver development for the AMD Van Gogh iGPU in the Steam Deck (PCI ID `1002:163F`).

## Current driver state

The kext uses a `VanGoghDriver` PCI service and a `VanGoghFramebuffer` nub in the same bundle. The PCI service validates the matched PCI ID, opens the provider, enables PCI memory decoding as needed, maps BAR5 using the OS-published resource length, and restores the prior decoding state on failure or detach. It waits for PSP IFWI-ready, copies and validates the AMD IP discovery TMR through the BAR5 `MM_INDEX/MM_DATA` window, validates optional PSP ASD/TOC containers without submitting them, and reads live MMHUB VMID0 and DCN301 HUBP/OTG/ODM/MPCC state.

The default framebuffer path is now **read-only live-scanout adoption**. If the boot firmware/OpenCore left the internal eDP pipe scanning a linear, uncompressed ARGB8888 surface, the driver translates that surface's MC address into the CPU-visible BAR0 window and publishes the exact aperture plus a 128 MiB `VRAM,totalMB` / `IOFBMemorySize` window to IOGraphics. macOS then draws into the memory the display engine is already scanning. No DCN register is written on this path.

Adoption refuses to run when `MMHUB VMID0` translation is enabled or unreadable (the HUBP address would be a GPU virtual address), when the surface is tiled or DCC-compressed (the Linux-composited layout is not CPU-usable), when MC/BAR0 bounds do not cover it, or when the live mode is not the captured 800x1280@60 eDP panel. If adoption is unavailable, the kext still publishes the PCI/GPU identity and leaves a diagnostic allocation, but does not advertise a framebuffer.

This is still not an acceleration stack. There is no GPUVM mapping, no GMC bring-up, no DCN pipe/clock/eDP modeset, no backlight, and no AUX/DDC hardware read. The built-in panel is the only supported display; an HDMI/external monitor requires DP link training, HPD, and AUX/EDID work that is not implemented. The EDID path forwards the Deck panel's captured EDID only. No kext build or load has been verified on macOS from this workspace.

### Default live-scanout adoption

| Property | Type | Default | Effect |
| --- | --- | --- | --- |
| `VanGoghDriver,adopt-live-scanout` | bool | true | Adopt the firmware/OpenCore linear ARGB8888 surface as the macOS aperture. Set false to disable. |
| `VanGoghDriver,vram-mb` | number | 128 | Amount advertised to IOGraphics via `VRAM,totalMB`/`IOFBMemorySize`, capped by the real readback and by the live surface position. |

Those properties are optional. The live-scanout path publishes `VanGoghDriver,scanout-live-adopted`, `scanout-live-hubp`, `scanout-vram-offset`, `scanout-mc-address`, and `VanGoghFramebuffer,ready`. If the log says adoption was refused, the reason is printed verbatim and the framebuffer remains dormant.

### Verify kext matching and framebuffer publication

The PCI personality uses an exact `IOPCIPrimaryMatch` for Van Gogh `1002:163F`. On boot, `VanGoghDriver` logs probe, start, adoption, and start-complete messages. Check whether the kext is loaded with `kmutil showloaded | grep com.steamdeckintosh.vangogh`, inspect the matched service with `ioreg -r -c VanGoghDriver -l`, and read boot messages with:

```sh
log show --last boot --predicate 'eventMessage CONTAINS "VanGogh"'
ioreg -r -c VanGoghFramebuffer -l
```

A service with `VanGoghDriver,attached = Yes` plus `VanGoghFramebuffer,ready = Yes` means the PCI kext started and the framebuffer nub was published. That does not mean acceleration exists. If `scanout-live-adopted` is absent, look for the `adopting live ... scanout` line or the specific refusal reason in the log.

### Load the kext with OpenCore

1. Build `SteamDeckintosh.kext` (Xcode on macOS, or the GitHub Actions artifact), copy it to `EFI/OC/Kexts/`, and add it under `Kernel -> Add`:

   ```xml
   <dict>
       <key>Arch</key>
       <string>x86_64</string>
       <key>BundlePath</key>
       <string>SteamDeckintosh.kext</string>
       <key>Comment</key>
       <string>Van Gogh iGPU / live framebuffer</string>
       <key>Enabled</key>
       <true/>
       <key>ExecutablePath</key>
       <string>Contents/MacOS/SteamDeckintosh</string>
       <key>MaxKernel</key>
       <string></string>
       <key>MinKernel</key>
       <string></string>
       <key>PlistPath</key>
       <string>Contents/Info.plist</string>
   </dict>
   ```

2. Use a recoverable setup (USB EFI, `-v keepsyms=1 debug=0x100`) and keep a known-good config. The kext is unsigned and is intended for local testing.

3. Ensure macOS will load an unsigned injected kext: set `SecureBootModel` to `Disabled` and use a SIP configuration that permits third-party kexts (a common hackintosh starting point is `csr-active-config` `0x00000000` with `SecureBootModel=Disabled`; this varies by macOS version). Also set `Kernel -> Quirks -> DisableLinkeditJettison` to `true`.

4. Reboot and check `kmutil showloaded | grep com.steamdeckintosh.vangogh`, then use the diagnostics above. `VanGoghDriver,probe-only` (below) still gives a zero-hardware-access matching check.

For a no-hardware-access load check, add `VanGoghDriver,probe-only` to the Steam Deck GPU’s OpenCore `DeviceProperties` as 4-byte little-endian data `AQAAAA==` (`01 00 00 00`). The live Linux PCI topology places GPU `04:00.0` behind bridge `00:08.1`, corresponding to this OpenCore path:

```xml
<key>DeviceProperties</key>
<dict>
    <key>Add</key>
    <dict>
        <key>PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x0)</key>
        <dict>
            <key>VanGoghDriver,probe-only</key>
            <data>AQAAAA==</data>
        </dict>
    </dict>
</dict>
```

This makes the PCI service publish its identity and return before opening the device or mapping any BARs. Remove the property to resume normal initialization. All custom OpenCore flags and numeric values are decoded from little-endian `OSData`; booleans should be encoded as `01 00 00 00` for true and `00 00 00 00` for false. The PCI path is derived from Linux's current topology; if your firmware exposes a different OpenCore path, use the GPU's actual path from your EFI/device tree.

## Try rotating the currently active macOS display

The framebuffer transform helper requests a 270-degree rotation on the currently active built-in macOS display and waits for CoreGraphics to report the new orientation. This tests the macOS display transform independently of this kext. On macOS, from the repository root:

```sh
clang -Wall -Wextra -framework IOKit -framework ApplicationServices tools/rotate_display.c -o /tmp/rotate-display
/tmp/rotate-display 270
```

Restore the normal orientation with `/tmp/rotate-display 0`. The helper targets the built-in display and returns failure if CoreGraphics does not confirm the requested orientation within two seconds. This exercises macOS's active framebuffer transform; it does not test Van Gogh DCN initialization or this kext's guarded framebuffer service.

When loaded, the kext binds to the GPU and maps its register aperture. It may claim the PCI service and contend with another matching driver. Load it only on a recoverable macOS setup after building and reviewing the artifact. BAR addresses are runtime assigned; the driver discovers them through IOPCIFamily.

## Opt-in VRAM scanout route (untested on hardware)

The live-scanout path above is the safe default and does not write DCN registers. The takeover route below plans a 1280x800 ARGB surface in VRAM that the CPU reaches through BAR0, then points the panel's live HUBPs at it. It is opt-in because a wrong address or timing write can blank or corrupt the panel. Set these on the PCI device (OpenCore `DeviceProperties`), one stage at a time:

| Property | Type | Effect |
| --- | --- | --- |
| `VanGoghDriver,vram-scanout` | bool | Plan the surface (offset, MC address) and map it via BAR0. |
| `VanGoghDriver,vram-scanout-pattern` | bool | Also fill it with a ramp/border test pattern. |
| `VanGoghDriver,program-vram-scanout` | bool | Also reprogram the panel HUBPs (needs the two above). |
| `VanGoghDriver,scanout-vram-offset` | number | Optional 64 KiB-aligned VRAM offset instead of automatic placement. |

The HUBP stage picks its surface from what the panel pipe is scanning: ARGB8888 rotated 270 within 1280x800 gives a rotated 1280x800 surface; ARGB8888 unrotated within 800x1280 (the likely firmware state) gives a native 800x1280 portrait surface with no rotation. Anything else is refused and the raw live values are logged. It validates every target before writing any, verifies by readback, and rolls back on failure and on detach. It publishes `ready` once the planes verify. Check the log lines starting `VanGoghDriver:` (`log show --predicate 'eventMessage CONTAINS "VanGogh"' --last boot`) and `ioreg -l | grep -i vangogh`.

Host tests: `make test`. Compile every kext source for macOS with a real clang (`pip install ziglang`, MacKernelSDK cloned beside the repo): `make compile-check`. Register cross-check against Linux's `dcn_3_0_1_offset.h` (defaults to `v6.6`, pass `--ref` for another tag): `make verify-registers`.

## Build

GitHub Actions builds an unsigned x86_64 bundle on a macOS runner and uploads it as an artifact. This Linux workspace cannot run Apple's `xcodebuild`. The workflow has not run until the project is pushed to a GitHub repository.

The local `VFCT.bin` is a hardware diagnostic dump, not a macOS driver resource. The default live-scanout path can provide an unaccelerated built-in-panel framebuffer, but HDMI/external displays, acceleration, backlight, and real AUX/EDID are not implemented. Install it only on a recoverable setup.

## Linux research tools

- `python3 tools/linux_ip_inventory.py [PCI-BDF]` prints OS-assigned PCI resource ranges and numeric IP discovery entries from sysfs. It is read-only and does not require root.
- `tools/parse_ip_discovery.py BINARY --compare-sysfs PCI-BDF` validates an `amdgpu_discovery` blob's signatures, checksums, table and die bounds, then compares parsed entries with sysfs. Capturing the blob from Linux debugfs requires root; parsing a saved file does not.
- `python3 tools/linux_resolve_ip_register.py HWID INSTANCE BASE_IDX REGISTER_DWORD` calculates a direct BAR5 address from the live table and checks its full 4-byte range. Supply the base index and register dword from headers matching that IP version. It never maps or reads the register and refuses candidates outside BAR5.

- `sudo tools/linux_hubp_dump.py dump --fetch --label rot270 -o rot270.json` (then again after unrotating the panel, `--label rot0`) reads, never writes, ~96 HUBP registers per plane from debugfs `amdgpu_regs`; `--fetch` downloads the matching mainline DCN301 offset header when kernel sources are not installed. `tools/linux_hubp_dump.py diff rot0.json rot270.json` lists exactly which registers rotation changes (viewport start/size, request-size and TTU settings, pitch). The kext's hardware rotation stage should be written from that diff, not from guesses.

Van Gogh is RDNA 2 (`gfx1033`), with GC `10.3.1`, PSP/SMU `11.5.0`, SDMA `5.2.1`, and DCN `3.0.1`. `nullarc-main` targets Phoenix/Hawk Point RDNA 3; its staged development pattern is useful, while its firmware, IP registers, memory setup, and display implementation do not apply to Van Gogh. See [hardware notes](hardware-notes.md) for the source trace, live captures, and mapping limits.
