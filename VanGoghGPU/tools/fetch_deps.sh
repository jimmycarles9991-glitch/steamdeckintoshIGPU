#!/bin/sh
# Fetch the two Apple-side build dependencies. They keep their own licenses.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/third_party"

if [ ! -d "$root/third_party/MacKernelSDK/Headers" ]; then
    git clone --depth 1 https://github.com/acidanthera/MacKernelSDK.git \
        "$root/third_party/MacKernelSDK"
fi
if [ ! -d "$root/third_party/Lilu" ]; then
    git clone --depth 1 https://github.com/acidanthera/Lilu.git \
        "$root/third_party/Lilu"
fi
echo "dependencies ready in $root/third_party"
