# Third-party sources

These directories are fetched by `tools/fetch_deps.sh` and are **not** part of
this project's MIT license.

| Directory | Upstream | License | Used for |
| --- | --- | --- | --- |
| `MacKernelSDK/` | https://github.com/acidanthera/MacKernelSDK | Apache-2.0 | kernel headers and stub libraries for building kexts |
| `Lilu/` | https://github.com/acidanthera/Lilu | BSD-3-Clause | reserved for a future patch plugin; no plugin links it today |

The Linux v6.12 source tree used as a register reference is fetched separately
by `tools/fetch_linux_ref.sh` into `../linux-vg-ref`. It is GPL-2.0-with-exceptions
and is not vendored here. Only numeric register facts are quoted in our source.
