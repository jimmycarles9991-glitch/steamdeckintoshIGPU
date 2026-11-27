#!/bin/sh
# Run this ON THE DECK UNDER LINUX before loading any kext.
# It collects the facts the macOS side cannot easily obtain.
set -eu
out=${1:-vangogh-dump}
mkdir -p "$out"

dump_pci() {
    for d in /sys/bus/pci/devices/*; do
        vid=$(cat "$d/vendor" 2>/dev/null || true)
        did=$(cat "$d/device" 2>/dev/null || true)
        if [ "$vid" = "0x1002" ] && [ "$did" = "0x163f" ]; then
            echo "$d"
        fi
    done
}

pci=$(dump_pci | head -1 || true)
if [ -n "$pci" ]; then
    {
        echo "PCI path: $pci"
        lspci -nn -vv -s "${pci##*/}" || true
        echo "--- resources ---"
        cat "$pci/resource" || true
        echo "--- config ---"
        xxd -l 256 "$pci/config" || true
    } > "$out/pci.txt"
else
    echo "no 1002:163f device found" > "$out/pci.txt"
fi

if [ -r /sys/firmware/acpi/tables/VFCT ]; then
    cp /sys/firmware/acpi/tables/VFCT "$out/VFCT.bin"
    echo "VFCT copied"
else
    echo "no VFCT in /sys/firmware/acpi/tables" > "$out/VFCT.missing"
fi

for e in /sys/class/drm/card*-eDP*/edid; do
    [ -r "$e" ] && cp "$e" "$out/edid.bin"
done

{
    echo "--- kernel version ---"; uname -a
    echo "--- amdgpu messages ---"; dmesg | grep -i amdgpu || true
    echo "--- firmware ---"; ls -l /usr/lib/firmware/amdgpu/vangogh_* 2>/dev/null || true
} > "$out/kernel.txt"

echo "collected into $out/"
echo "attach this directory when reporting a probe result"
