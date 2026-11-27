# Installing / testing

Nothing here changes the kernel collection on the macOS volume: both kexts are
loaded from the OpenCore EFI, so a bad boot can be undone from the bootloader
menu or by editing the config.plist from Linux/Windows.

## 1. Build

On a Mac with Command Line Tools:

```sh
sh tools/fetch_deps.sh
make hosttest          # must print "all host tests passed"
make probe fb
```

The resulting bundles are `build/out/VanGoghProbe.kext` and
`build/out/VGAMDFramebuffer.kext`.

## 2. First boot — probe only

1. Copy both kexts into `EFI/OC/Kexts/` and add them to `Kernel -> Add` after
   Lilu. Leave boot-args with `-vgfboff`.
2. Boot macOS. The framebuffer kext will refuse to probe; the probe kext will
   attach to `1002:163f`.
3. Collect:

   ```sh
   ioreg -rc VanGoghProbe > probe.txt
   log show --last boot --predicate 'eventMessage CONTAINS "VanGoghProbe"' > probe.log
   ```

4. Confirm `VGBAR0Length`, `VGIP_271_0_Base2`, `VGIP_11_0_Major` and
   `VGOTGFrameCnt` are present and plausible.

Also run `tools/collect.sh` on the Deck under Linux and keep its output next to
the macOS dump.

## 3. Second boot — framebuffer

Replace `-vgfboff` with `-vgfbown` in boot-args. Optional overrides:

| boot-arg | default | meaning |
| --- | --- | --- |
| `vgfb-width=800` | 800 | active pixels (native portrait) |
| `vgfb-height=1280` | 1280 | active lines |
| `vgfb-hz=60` | 60 | refresh, used for the timer VBL |
| `vgfb-stride=3200` | width×4 | bytes per row |
| `vgfb-offset=0` | 0 | byte offset into BAR0 where the scanout lives |
| `vgfb-aperture=4096000` | stride×height | mapped aperture size |

This kext does not rotate the panel; expect native 800×1280 portrait.

If the screen shows garbage, the offset/stride is wrong. Reboot with
`-vgfboff` and try another value; no rebuild is needed.

If two displays appear, the displacement of `IONDRVFramebuffer` failed. Check
`ioreg -rc IOFramebuffer` with `-vgfboff` and compare `IOProbeScore` and
`IOMatchCategory`.

## 4. Recovery

- `-vgfboff` always wins over `-vgfbown`.
- Removing both kexts from `config.plist` returns the machine to stock.
- Recovery and the macOS installer ignore EFI kexts in most configurations and
  remain usable as a way back.

## Do not do this

- Do not add `-vgfbown` to the same boot in which you change five unknowns.
- Do not remove the OpenCore entry you know boots.
- Do not expect OpenGL or Metal: this kext only exposes a CPU-drawn surface.
