#!/bin/sh
# Fetch only the two pinned Linux v6.12 files needed by tools/check_regs.py.
# Cloning the whole kernel tree is slow and times out on CI runners.
set -eu
dest=${1:-../linux-vg-ref}
base=https://raw.githubusercontent.com/torvalds/linux/v6.12

mkdir -p "$dest/drivers/gpu/drm/amd/amdgpu" \
         "$dest/drivers/gpu/drm/amd/include/asic_reg/dcn"

fetch() {
    url=$1
    out=$2
    if [ -s "$out" ]; then
        echo "already present: $out"
        return
    fi
    curl -fsSL --retry 3 --connect-timeout 15 --max-time 120 "$url" -o "$out"
}

fetch "$base/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c" \
      "$dest/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c"
fetch "$base/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h" \
      "$dest/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h"

echo "Linux v6.12 register reference ready at $dest"
