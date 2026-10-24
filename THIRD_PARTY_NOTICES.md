# Third-Party Notices

This project (Tourmaline) is released under the MIT license (see `LICENSE`).

Third-party code under `external/` comes in two forms:

- **Unmodified upstream libraries are pinned git submodules** (see
  `.gitmodules`) — each carries its own upstream license in its checkout.
  Every pin was verified byte-exact (EOL-normalized for nlohmann/json)
  against the previously vendored content before the conversion.
- **Locally trimmed/patched trees, unpinnable snapshots, binary packages and
  single-header vendors are vendored** (plain files in this repo); each keeps
  its license file in-tree (linked below), and those licenses apply to the
  respective subdirectories.

All of them are permissive (MIT / Apache-2.0 / BSD / zlib / public domain)
and are compatible with this project's MIT release. No copyleft (GPL/LGPL)
code is linked, vendored or submoduled.

## Git submodules (unmodified upstream, pinned)

| Component | Path | License | Pinned commit | Notes |
|---|---|---|---|---|
| [GLM](https://github.com/g-truc/glm) | `external/glm` | [Happy Bunny (MIT-like)](external/glm/copying.txt) | `2d4c4b4d` | Math |
| [nlohmann/json](https://github.com/nlohmann/json) | `external/json` | [MIT](external/json/LICENSE.MIT) | `96c1b52f` | JSON (also required by tinygltf) |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | `external/meshoptimizer` | [MIT](external/meshoptimizer/LICENSE.md) | `4a6532b7` | Mesh optimization |
| [DirectXTex](https://github.com/microsoft/DirectXTex) | `external/DirectXTex` | [MIT](external/DirectXTex/LICENSE) | `868198cb` | Only the BC block codec TUs compile (BC6H/BC7 payload) |
| [volk](https://github.com/zeux/volk) | `external/volk` | [MIT](external/volk/LICENSE.md) | `eaa61418` | Vulkan loader |
| [SPIRV-Reflect](https://github.com/KhronosGroup/SPIRV-Reflect) | `external/SPIRV-Reflect` | [Apache-2.0](external/SPIRV-Reflect/LICENSE) | `e489e162` | CPU reflection for push/spec readback |
| [nanosvg](https://github.com/memononen/nanosvg) | `external/nanosvg` | [zlib](external/nanosvg/LICENSE.txt) | `239e102e` | Editor SVG icons |

## Vendored libraries (compiled into the build)

Kept in-tree rather than submoduled because they are locally modified, carry
a hand-written integration, or are an unpinnable snapshot:

| Component | Path | License | Notes |
|---|---|---|---|
| [Embree](https://github.com/embree/embree) | `external/embree` | [Apache-2.0](external/embree/LICENSE.txt) | Ray-tracing backend for GI/lightmap bakes. Locally trimmed from upstream master (2026-10-01 era, still versioned 4.4.1): docs/tutorials/tests/superbuild removed, `ADD_SUBDIRECTORY(tests)` + `INCLUDE(CPack)` cut from the root CMakeLists; the ARM NEON SIMD headers stay in-tree. Build forces static lib, internal tasking, triangle-only |
| [Dear ImGui](https://github.com/ocornut/imgui) | `external/imgui` | [MIT](external/imgui/LICENSE.txt) | Editor/UI; hand-written CMakeLists.txt (upstream ships none) |
| [LZ4](https://github.com/lz4/lz4) | `external/lz4` | BSD-2-Clause (license text in the [`lz4.h`](external/lz4/lz4.h) header block) | Block codec for cooked pak blobs; hand-written minimal CMakeLists.txt (`lz4.{h,c}` only) |
| [Optick](https://github.com/bombomby/optick) | `external/optick` | [MIT](external/optick/LICENSE) | CPU/GPU profiler (optional, `ENABLE_OPTICK`). Locally patched: `src/optick_gpu.vulkan.cpp` (query-pool resolve/submit order fix — recording into a submitted command buffer trips validation) + a cmake_minimum_required bump |
| [SDL3](https://github.com/libsdl-org/SDL) 3.4.16 | `external/SDL3-3.4.16` | [zlib](external/SDL3-3.4.16/LICENSE.txt) | Windowing/input/audio (vendored prebuilt dev package: include/ + lib/ + cmake/) |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | `external/SQLiteCpp` | [MIT](external/SQLiteCpp/LICENSE.txt) | RAII wrapper over sqlite3 (editor artifact DB). Unpinnable mixed snapshot (content-identical on 25/26 files; LICENSE/README and a few headers at older states) |
| SQLite3 (amalgamation) | `external/SQLiteCpp/sqlite3` | Public Domain | `sqlite3.{h,c}` |
| [Box3D](https://github.com/erincatto/box3d) | `external/box3d` | [MIT](external/box3d/LICENSE) | Vendored for upcoming physics work (not yet wired into targets; unpinnable snapshot) |
| [xatlas](https://github.com/jpcy/xatlas) | `external/xatlas` | [MIT](external/xatlas/LICENSE) | Lightmap UV2 chart+pack; hand-written CMakeLists.txt builds only the self-contained core (bundled OpenFBX license in-tree) |
| [zlib](https://zlib.net) | `external/zlib` | [zlib](external/zlib/LICENSE) | CRC/reference only (not wired into the build); unpinnable develop-branch snapshot |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | `external/VulkanMemoryAllocator` | MIT (see header notice) | `vk_mem_alloc.h` |
| [tinygltf](https://github.com/syoyo/tinygltf) | `external/tinygltf` | MIT (see header notice) | `tiny_gltf.h` (gltf_import tool) |
| [stb](https://github.com/nothings/stb) | `external/stb` | Public Domain / MIT (see header notices) | `stb_image.h`, `stb_image_write.h` |

## Prebuilt binaries vendored for the build/run

| Component | Path | License | Notes |
|---|---|---|---|
| Microsoft DXC (DirectXShaderCompiler) | `external/dxc` | [Apache-2.0 w/ LLVM exceptions](external/dxc/LICENSE-LLVM.txt), [MIT](external/dxc/LICENCE-MIT.txt), [MS](external/dxc/LICENSE-MS.txt) | `dxc.exe`/headers; the build itself uses the Vulkan SDK's `dxc.exe` (`%VULKAN_SDK%/Bin`) |
| NVIDIA Streamline SDK 2.14.1 | `external/streamline` | [license.txt (MIT-style)](external/streamline/license.txt) + [NSight Perf SDK](external/streamline/license.txt) | Interposer + Reflex runtimes (`sl.*.dll`, `NvLowLatencyVk.dll`) redistributed with the required notices: [`reflex.license.txt`](external/streamline/bin/x64/reflex.license.txt), [`3rd-party-licenses.md`](external/streamline/3rd-party-licenses.md). The NGX snippet `nvngx_dlss.dll` (~56 MB) is NOT redistributed — it is fetched separately by devs ([how](external/streamline/bin/x64/README.md)); its license terms ([`nvngx_dlss.license.txt`](external/streamline/bin/x64/nvngx_dlss.license.txt)) stay vendored for whoever fetches it. Optional at runtime: if absent, the engine simply runs without DLSS-SR (verified: native render extent, Reflex/PCL unaffected, clean exit). DLSS features need NVIDIA hardware. |

## Ported / embedded code and data (source kept in-tree)

- [SMAA](https://github.com/iryoku/smaa) — Enhanced Subpixel Morphological Antialiasing, by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (2013) — MIT License. Used for the SMAA 1x / T2x antialiasing chain: the shader core is ported in `engine/shaders/smaa.hlsli`, and the precomputed area/search lookup tables are embedded byte-exact in `engine/include/Engine/Rendergraph/smaa_area_tex.h` and `smaa_search_tex.h`.
- [Mastering Graphics Programming with Vulkan](https://github.com/PacktPublishing/Mastering-Graphics-Programming-with-Vulkan) — MIT License. The scene format is derived from its scene description format (the `Portions Copyright (c) 2022 Packt` headers in `engine/src/Scene/` + `engine/include/Engine/Scene/`); also a technique reference for the deferred shadow-map (DPS) work. Nothing is vendored under `external/` — the license text is embedded in the third-party notice in [`LICENSE`](LICENSE).

## Not vendored, but required at build time

- [KTX-Software](https://github.com/KhronosGroup/KTX-Software) — Apache-2.0. Located via `find_package(ktx)` (`KTX_ROOT` env or `-Dktx_DIR`). Not redistributed with this repo.
- [Boost](https://www.boost.org) (1.89) — Boost Software License. Required ONLY for the editor's optional fiber backend (`BOOST_ROOT` env hint). Not redistributed with this repo.
- [Vulkan SDK](https://vulkan.lunarg.com) — DXC (`%VULKAN_SDK%/Bin/dxc.exe`) compiles the HLSL to SPIR-V. Not redistributed with this repo.
