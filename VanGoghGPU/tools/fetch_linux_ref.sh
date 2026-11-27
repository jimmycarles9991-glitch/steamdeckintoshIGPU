#!/bin/sh
# Fetch the pinned Linux v6.12 amdgpu/display source used as a register reference.
# The tree is ~500 MB with the sparse checkout; it is intentionally NOT vendored.
set -eu
dest=${1:-../linux-vg-ref}
if [ -d "$dest/.git" ]; then
    echo "already present: $dest"
    exit 0
fi
git clone --depth 1 --branch v6.12 --filter=blob:none --sparse \
    https://github.com/torvalds/linux.git "$dest"
cd "$dest"
git sparse-checkout set \
    drivers/gpu/drm/amd/amdgpu \
    drivers/gpu/drm/amd/include \
    drivers/gpu/drm/amd/display \
    include/drm
echo "Linux reference ready at $dest"
