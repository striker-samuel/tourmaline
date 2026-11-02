# Optional Aftermath SDK crash capture (private diagnostics only)

Requires PR 1's `TOURMALINE_BUILD_PROFILE=diagnostics`. The NVIDIA Nsight
Aftermath SDK is **not** vendored. On Windows x64, supply your private local
SDK install and configure a fresh Release diagnostic build:

```powershell
cmake -S . -B out/build/x64-Aftermath -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DTOURMALINE_BUILD_PROFILE=diagnostics `
  -DTOURMALINE_AFTERMATH_SDK=ON `
  -DTOURMALINE_AFTERMATH_SDK_ROOT="C:/SDK/NsightAftermath"
```

When enabled, the SDK is initialized **before Vulkan instance creation**.
It writes raw `.nv-gpudmp` and `.nvdbg` blobs to
`out/aftermath-private/` relative to the process working directory. There is
no upload, telemetry, automatic public artifact publication, or embedded
source file database. Use NVIDIA's local tools/monitor to decode the dumps.
Existing `VK_EXT_device_fault` diagnostics and per-pass checkpoints remain.

`TOURMALINE_AFTERMATH=0` disables runtime crash callbacks and shader-debug
tables, even in a diagnostics build. If the SDK is absent, configure with
`TOURMALINE_AFTERMATH_SDK=OFF` to retain normal Vulkan diagnostics.

**Do not distribute** the diagnostic executable, SDK runtime, GPU crash dumps,
shader debug tables, diagnostic shader packages, or native symbols. All
Aftermath dumps are private and may expose HLSL source paths, shader metadata,
GPU addresses and disassembly. Never attach them to public bug reports or CI.

This is an opt-in developer diagnostic path, not a release-time crash-reporting
service; release builds must not link the Aftermath SDK.
