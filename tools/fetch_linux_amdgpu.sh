#!/usr/bin/env bash
# Fetch the exact Linux amdgpu sources used to derive the Van Gogh readiness
# gate and register table. Read-only reference material; nothing links against
# these files. Defaults match the reference Deck's running kernel
# (6.18.44-1-lts), so the upstream stable tag is the closest source match.
set -euo pipefail

REF="${KERNEL_REF:-v6.18.44}"
BASE="https://raw.githubusercontent.com/gregkh/linux/${REF}"
OUT="${1:-${LINUX_AMDGPU_REF:-/tmp/opencode/linux-amdgpu-ref}}"
mkdir -p "$OUT"

FILES=(
  drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
  drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c
  drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c
  drivers/gpu/drm/amd/amdgpu/psp_v11_0.c
  drivers/gpu/drm/amd/amdgpu/nbio_v2_3.c
  drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c
  drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c
  drivers/gpu/drm/amd/amdgpu/soc15_common.h
  drivers/gpu/drm/amd/include/asic_reg/mp/mp_11_0_offset.h
  drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_offset.h
  drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_1_sh_mask.h
)

for path in "${FILES[@]}"; do
  name="$(basename "$path")"
  printf '%-32s ' "$name"
  if curl -fsSL --retry 3 --connect-timeout 20 "$BASE/$path" -o "$OUT/$name"; then
    echo ok
  else
    echo FAILED
    exit 1
  fi
done

echo "Linux $REF amdgpu reference sources: $OUT"
