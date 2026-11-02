#!/usr/bin/env bash
# Compile every Kext/*.cpp for x86_64 macOS against Acidanthera's MacKernelSDK.
# Uses Zig's bundled clang when installed, otherwise system clang. This catches
# source errors without Xcode; it does not link or sign the kext.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${MACKERNELSDK:-$ROOT/MacKernelSDK}"
[ -d "$SDK/Headers" ] || { echo "MacKernelSDK headers not found at $SDK" >&2; exit 2; }

export ZIG_GLOBAL_CACHE_DIR="${TMPDIR:-/tmp}/zig-global"
export ZIG_LOCAL_CACHE_DIR="${TMPDIR:-/tmp}/zig-local"
OUT="${TMPDIR:-/tmp}/kext-objs"
mkdir -p "$OUT" "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"

if python3 -m ziglang version >/dev/null 2>&1; then
  COMPILER=(python3 -m ziglang c++)
  TARGET=x86_64-macos.12.0
elif command -v clang++ >/dev/null 2>&1; then
  COMPILER=(clang++)
  TARGET=x86_64-apple-macos12.0
else
  echo "Need python3 -m ziglang or clang++ to compile the kext sources" >&2
  exit 2
fi

rc=0
for f in "$ROOT"/Kext/*.cpp; do
  n=$(basename "$f" .cpp)
  if "${COMPILER[@]}" -c -Wall -Werror -target "$TARGET" -std=gnu++14 -fno-rtti -fno-exceptions \
       -nostdinc -nostdinc++ -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE \
       -I"$SDK/Headers" "$f" -o "$OUT/$n.o"; then
    echo "ok   $n"
  else
    echo "FAIL $n"
    rc=1
  fi
done
exit "$rc"
