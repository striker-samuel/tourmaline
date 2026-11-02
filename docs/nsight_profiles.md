# Tourmaline Nsight build profiles

Create **separate out-of-source Ninja build directories**, one per profile:

```sh
cmake -S . -B out/build/x64-Shipping -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_BUILD_PROFILE=shipping
cmake -S . -B out/build/x64-Dev -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_BUILD_PROFILE=dev
cmake -S . -B out/build/x64-Diagnostics -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_BUILD_PROFILE=diagnostics
```

| Profile | Optimized code | Optick runtime | Nsight Vulkan pass labels | Shader debug info | Distribution |
| --- | --- | --- | --- | --- | --- |
| shipping | yes | no | no | no | allowed after artifact checks |
| dev | yes | no | yes | no | internal |
| diagnostics | yes | yes | yes | yes | **private only** |
| custom | existing controls | existing controls | opt-in | existing controls | audit manually |

`dev` retains fast iteration and annotated command buffers without shader
debug line maps. `diagnostics` uses existing `-O3 -Zi -fspv-debug=vulkan`
shader compilation and is only for internal investigation. In Nsight Graphics,
open a GPU trace or frame capture using `tools/perf_harness.py` and the
`Benchmark` target. The labels use `VK_EXT_debug_utils` when available,
including when Vulkan validation is disabled.

## Shipping security boundary

Ship binaries, packed assets and compiled shaders **only** from the shipping
build tree using an allowlist. Do not ship PDB / split DWARF symbols, `.nvdbg`,
`.nv-gpudmp`, `.opt`, GPU traces, crash logs, source HLSL, CMake caches, or
private debugging SPIR-V. Check **both** loose SPIR-V and `engine-shaders.pak`.
Keep internal symbol files separate from public CI artifacts. Windows builds
should inspect release PE debug-directory references and Linux builds should
strip/copy debug symbols as a private packaging step.

Stripping shader debug metadata reduces exposed source names and line mapping,
but SPIR-V bytecode itself is reverse-engineerable. For particularly sensitive
algorithms, client-side shaders cannot provide complete source secrecy.
