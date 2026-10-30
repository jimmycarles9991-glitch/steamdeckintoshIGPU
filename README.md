# SteamDeckintosh

Experimental macOS graphics-driver research for the AMD Van Gogh iGPU in the Steam Deck (PCI vendor/device ID `1002:163F`).

## Status

The first source target is a diagnostic probe kext. It matches PCI ID `1002:163F`, logs the PCI vendor/device/revision when I/O Kit attaches it, and does not touch GPU registers. It does **not** set up the framebuffer, initialize the GPU, provide acceleration, or make the built-in panel work. Because it binds to the PCI device, do not load it on a system you rely on until testing in a recoverable setup; it may affect another driver that matches the same device.

The GitHub Actions workflow builds an unsigned x86_64 bundle on a macOS runner and uploads it as an artifact. This Linux workspace cannot run Apple's `xcodebuild`, and the workflow has not run until this folder is pushed to a GitHub repository. The local `VFCT.bin` is a hardware diagnostic dump, not a macOS driver resource.

Van Gogh is an RDNA 2 integrated GPU, and this project has no functioning macOS graphics stack for it. Full support would require GPU initialization, memory management, display-engine/framebuffer support, power management, and acceleration, plus testing on the Steam Deck. A device-ID match alone cannot supply that functionality.

## Development notes

- The probe targets Intel x86_64 and macOS 12 or later as a starting build target. The precise macOS version must be confirmed before attempting to load it.
- A successful GitHub build only proves that Xcode produced a bundle. It does not prove the bundle can be loaded, safely bind to the GPU, or work on the Deck.
- Keep this work separate from third-party driver source unless its license explicitly permits the intended use. This project currently contains no copied or adapted third-party implementation.
- Do not install this placeholder in OpenCore. It has no executable and cannot drive the GPU.

## Planned work

1. Run the GitHub Actions workflow to confirm the probe compiles.
2. Confirm the exact macOS release and boot setup before any load attempt. Kernel extensions are deprecated by Apple, and load policy varies by macOS release and security configuration.
3. Investigate Van Gogh initialization, memory management, display output, power management, and acceleration requirements. Linux's `amdgpu` initialization is a reference for the hardware sequence, not a macOS implementation.
4. Only after a safe probe test, develop and validate driver pieces incrementally on recoverable hardware.

There is no working implementation or timeline yet.
