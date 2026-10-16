# Third-Party Notices

This project (Tourmaline) is released under the MIT license (see `LICENSE`).
It vendors a number of third-party libraries and SDKs under `external/`.
Each vendored component keeps its own license file in-tree (linked below);
those licenses apply to the respective subdirectories.

All of them are permissive (MIT / Apache-2.0 / BSD / zlib / public domain)
and are compatible with this project's MIT release. No copyleft (GPL/LGPL)
code is linked or vendored.

## Vendored libraries (compiled into the build)

| Component | Path | License | Notes |
|---|---|---|---|
| [Embree](https://github.com/embree/embree) 4.4.1 | `external/embree` | [Apache-2.0](external/embree/LICENSE.txt) | Ray-tracing backend for GI/lightmap bakes (trimmed build: static lib, internal tasking, triangle-only) |
| [GLFW](https://www.glfw.org) | `external/glfw` | [zlib](external/glfw/LICENSE.md) | Legacy windowing; kept for the samples (engine uses SDL3) |
| [GLM](https://github.com/g-truc/glm) | `external/glm` | [Happy Bunny (MIT-like)](external/glm/copying.txt) | Math |
| [Dear ImGui](https://github.com/ocornut/imgui) | `external/imgui` | [MIT](external/imgui/LICENSE.txt) | Editor/UI |
| [nlohmann/json](https://github.com/nlohmann/json) | `external/json` | [MIT](external/json/LICENSE.MIT) | JSON (also required by tinygltf) |
| [LZ4](https://github.com/lz4/lz4) | `external/lz4` | BSD-2-Clause (license text in the [`lz4.h`](external/lz4/lz4.h) header block) | Block codec for cooked pak blobs |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | `external/meshoptimizer` | [MIT](external/meshoptimizer/LICENSE.md) | Mesh optimization |
| [Optick](https://github.com/bombomkov/optick) | `external/optick` | [MIT](external/optick/LICENSE) | CPU/GPU profiler (optional, `ENABLE_OPTICK`) |
| [SDL3](https://github.com/libsdl-org/SDL) 3.4.16 | `external/SDL3-3.4.16` | [zlib](external/SDL3-3.4.16/LICENSE.txt) | Windowing/input/audio (vendored prebuilt dev package) |
| [SPIRV-Reflect](https://github.com/KhronosGroup/SPIRV-Reflect) | `external/SPIRV-Reflect` | [Apache-2.0](external/SPIRV-Reflect/LICENSE) | CPU reflection for push/spec readback |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | `external/SQLiteCpp` | [MIT](external/SQLiteCpp/LICENSE.txt) | RAII wrapper over sqlite3 (editor artifact DB) |
| SQLite3 (amalgamation) | `external/SQLiteCpp/sqlite3` | Public Domain | `sqlite3.{h,c}` |
| [Box3D](https://github.com/erincatto/box3d) | `external/box3d` | [MIT](external/box3d/LICENSE) | Vendored for upcoming physics work (not yet wired into targets) |
| [xatlas](https://github.com/jpcy/xatlas) | `external/xatlas` | [MIT](external/xatlas/LICENSE) | Lightmap UV2 chart+pack (includes bundled OpenFBX, license in-tree) |
| [DirectXTex](https://github.com/microsoft/DirectXTex) | `external/DirectXTex` | [MIT](external/DirectXTex/LICENSE) | Only the BC block codec TUs compile (BC6H/BC7 payload) |
| [zlib](https://zlib.net) | `external/zlib` | [zlib](external/zlib/LICENSE) | CRC/reference only |
| [volk](https://github.com/zeux/volk) | `external/volk` | [MIT](external/volk/LICENSE.md) | Vulkan loader |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | `external/VulkanMemoryAllocator` | MIT (see header notice) | `vk_mem_alloc.h` |
| [tinygltf](https://github.com/syoyo/tinygltf) | `external/tinygltf` | MIT (see header notice) | `tiny_gltf.h` (gltf_import tool) |
| [stb](https://github.com/nothings/stb) | `external/stb` | Public Domain / MIT (see header notices) | `stb_image.h`, `stb_image_write.h` |
| [nanosvg](https://github.com/memononen/nanosvg) | `external/nanosvg` | [zlib](external/nanosvg/LICENSE.txt) | Editor SVG icons |

## Prebuilt binaries vendored for the build/run

| Component | Path | License | Notes |
|---|---|---|---|
| Microsoft DXC (DirectXShaderCompiler) | `external/dxc` | [Apache-2.0 w/ LLVM exceptions](external/dxc/LICENSE-LLVM.txt), [MIT](external/dxc/LICENCE-MIT.txt), [MS](external/dxc/LICENSE-MS.txt) | `dxc.exe`/headers; the build itself uses the Vulkan SDK's `dxc.exe` (`%VULKAN_SDK%/Bin`) |
| NVIDIA Streamline SDK 2.14.1 | `external/streamline` | [license.txt (MIT-style)](external/streamline/license.txt) + [NSight Perf SDK](external/streamline/license.txt) | Interposer + DLSS-SR + Reflex runtimes (`sl.*.dll`, `nvngx_dlss.dll`, `NvLowLatencyVk.dll`) redistributed with the required notices: [`nvngx_dlss.license.txt`](external/streamline/bin/x64/nvngx_dlss.license.txt), [`reflex.license.txt`](external/streamline/bin/x64/reflex.license.txt), [`3rd-party-licenses.md`](external/streamline/3rd-party-licenses.md). Optional at runtime: if absent, the engine simply runs without DLSS/Reflex. DLSS features need NVIDIA hardware. |

## Ported / embedded code and data (source kept in-tree)

- [SMAA](https://github.com/iryoku/smaa) — Enhanced Subpixel Morphological Antialiasing, by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (2013) — MIT License. Used for the SMAA 1x / T2x antialiasing chain: the shader core is ported in `engine/shaders/smaa.hlsli`, and the precomputed area/search lookup tables are embedded byte-exact in `engine/include/Engine/Rendergraph/smaa_area_tex.h` and `smaa_search_tex.h`.
- [Mastering Graphics Programming with Vulkan](https://github.com/PacktPublishing/Mastering-Graphics-Programming-with-Vulkan) — MIT License. Technique reference for the deferred shadow-map (DPS) work; only its license text is vendored (`external/MasteringGraphicsProgrammingwithVulkan/LICENSE.txt`).

## Not vendored, but required at build time

- [KTX-Software](https://github.com/KhronosGroup/KTX-Software) — Apache-2.0. Located via `find_package(ktx)` (`KTX_ROOT` env or `-Dktx_DIR`). Not redistributed with this repo.
- [Boost](https://www.boost.org) (1.89) — Boost Software License. Required ONLY for the editor's optional fiber backend (`BOOST_ROOT` env hint). Not redistributed with this repo.
- [Vulkan SDK](https://vulkan.lunarg.com) — DXC (`%VULKAN_SDK%/Bin/dxc.exe`) compiles the HLSL to SPIR-V. Not redistributed with this repo.
