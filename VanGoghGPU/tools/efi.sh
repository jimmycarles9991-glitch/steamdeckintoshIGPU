#!/bin/sh
# Minimal OpenCore installer for the two kexts.
# It copies the bundles and prints exactly what config.plist still needs; it
# deliberately does not edit plist XML with shell tools, because a silent bad
# edit is how an EFI gets made unbootable.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
efi=${1:-/Volumes/EFI/EFI/OC}
out=$root/build/out

for k in VanGoghProbe.kext VGAMDFramebuffer.kext; do
    [ -d "$out/$k" ] || { echo "missing $out/$k - run make probe fb first"; exit 1; }
done
[ -d "$efi" ] || { echo "EFI directory not found: $efi"; exit 1; }

mkdir -p "$efi/Kexts"
for k in VanGoghProbe.kext VGAMDFramebuffer.kext; do
    rm -rf "$efi/Kexts/$k"
    cp -R "$out/$k" "$efi/Kexts/$k"
    echo "installed $efi/Kexts/$k"
done

cat <<'EOF'

Now edit EFI/OC/config.plist:
  1. Add both kexts to Kernel -> Add, in this order:
       Lilu.kext, VanGoghProbe.kext, VGAMDFramebuffer.kext
     with Enabled=true.
  2. Keep the boot-arg -vgfboff for the first test boot.
  3. Check Kernel -> Quirks -> XhciPortLimit=false and a AMD CPU patch
     matching 4 cores / 8 threads (see Opencore-Steam-Deck).
  4. Rebuild the kernel cache with the OpenCore boot-args you normally use.

To test the framebuffer, replace -vgfboff with -vgfbown for ONE boot.
Always keep a second boot path available.
EOF
