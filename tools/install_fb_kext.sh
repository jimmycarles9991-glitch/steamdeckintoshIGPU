#!/bin/sh
# Install SteamDeckintoshFB.kext into the auxiliary kernel collection.
#
#   sudo tools/install_fb_kext.sh [path/to/SteamDeckintoshFB.kext]
#
# The framebuffer kext links against IOGraphicsFamily, which lives in
# SystemKernelExtensions.kc. OpenCore-injected kexts are linked only against
# the boot collection, so this kext must NOT be added to EFI/OC/Kexts. It is
# staged in /Library/Extensions and included in AuxiliaryKernelExtensions.kc.
#
# The UUID that matters is the one inside the collection the next boot loads,
# not the one in /Library/Extensions. Build to a scratch collection, verify
# the UUID there, back up the live collection, write it, then verify again and
# restore the backup if verification fails.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/install_fb_kext.sh" >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
src="${1:-$root/SteamDeckintoshFB.kext}"
bin="$src/Contents/MacOS/SteamDeckintoshFB"
boot=/System/Library/KernelCollections/BootKernelExtensions.kc
sys=/System/Library/KernelCollections/SystemKernelExtensions.kc
live=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
scratch=/private/var/tmp/steamdeckintosh-aux.kc

if csrutil status 2>/dev/null | grep -qi 'enabled'; then
    echo "SIP must be disabled to rebuild the auxiliary kernel collection." >&2
    echo "In OpenCore set csr-active-config FF0F0000 (not in NVRAM > Delete)." >&2
    exit 1
fi
[ -d "$src" ] || { echo "no kext at $src (pass it as the first argument)" >&2; exit 1; }
[ -f "$bin" ] || { echo "missing $bin" >&2; exit 1; }
[ -f "$boot" ] && [ -f "$sys" ] || { echo "missing $boot or $sys" >&2; exit 1; }
command -v kmutil >/dev/null 2>&1 || { echo "kmutil not found" >&2; exit 1; }
command -v dwarfdump >/dev/null 2>&1 || { echo "dwarfdump not found (install Xcode CLI tools)" >&2; exit 1; }

uuid="$(dwarfdump --uuid "$bin" | awk '{print $2}')"
[ -n "$uuid" ] || { echo "could not read a UUID out of $bin" >&2; exit 1; }
echo "installing SteamDeckintoshFB $uuid"

echo "  staging into /Library/Extensions"
rm -rf /Library/Extensions/SteamDeckintoshFB.kext
cp -R "$src" /Library/Extensions/
chown -R root:wheel /Library/Extensions/SteamDeckintoshFB.kext
codesign -f -s - /Library/Extensions/SteamDeckintoshFB.kext

echo "  dry run -> $scratch"
rm -f "$scratch"
kmutil create -n aux -a x86_64 --allow-missing-kdk \
    -B "$boot" -S "$sys" -A "$scratch" -r /Library/Extensions >/dev/null
[ -s "$scratch" ] || {
    echo "DRY RUN WROTE NOTHING. kmutil create exits 0 when it refuses, so" >&2
    echo "this is the real failure and the live collection is untouched." >&2
    exit 1
}
kmutil inspect -B "$scratch" --show-kext-uuids 2>/dev/null \
    | grep -qi "$uuid" || {
    echo "THE DRY-RUN COLLECTION DOES NOT CARRY $uuid." >&2
    echo "Whatever it does carry is not this build. Live one untouched." >&2
    kmutil inspect -B "$scratch" --show-kext-uuids 2>/dev/null \
        | grep -i steamdeckintosh >&2 || echo "  (no SteamDeckintosh kext in it at all)" >&2
    exit 1
}
echo "  dry run carries $uuid"

echo "  backing up $live"
cp "$live" "$live.bak"

echo "  writing $live"
kmutil create -n aux -a x86_64 --allow-missing-kdk \
    -B "$boot" -S "$sys" -A "$live" -r /Library/Extensions >/dev/null

if kmutil inspect -B "$live" --show-kext-uuids 2>/dev/null | grep -qi "$uuid"
then
    echo
    echo "INSTALLED. $live carries $uuid."
    echo "Reboot, then verify with:"
    echo "  kextstat | grep -i steamdeckintosh"
    echo "  ioreg -r -c VanGoghFramebuffer -l"
    rm -f "$scratch"
    exit 0
fi

echo "THE LIVE COLLECTION DOES NOT CARRY $uuid - restoring the backup." >&2
cp "$live.bak" "$live"
exit 1
