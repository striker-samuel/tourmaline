# Tourmaline

[![CI](https://github.com/striker-samuel/tourmaline/actions/workflows/ci.yml/badge.svg)](https://github.com/striker-samuel/tourmaline/actions/workflows/ci.yml)
[![Tests](https://github.com/striker-samuel/tourmaline/actions/workflows/tests.yml/badge.svg)](https://github.com/striker-samuel/tourmaline/actions/workflows/tests.yml)

A C++20 toy engine and renderer for learning real-time graphics on Vulkan 1.3 —
a personal project by Samuel Martin, built from scratch (RHI, render graph,
GPU-driven culling, content pipeline and editor included).

Windows (MSVC) and Linux (GCC 14, verified on Ubuntu 24.04) both build the
full tree. Everything is Ninja-driven and shader-hot: HLSL sources compile
to SPIR-V at build time and load next to the executable.

## Features

- **Custom Vulkan 1.3 RHI** — bindless descriptor pools, render graph with
  automatic barrier placement, staging uploads, GPU timeline semaphores.
- **GPU-driven rendering** — HZB occlusion culling, GPU mesh filter with
  LOD chains, clustered Forward+ lighting, GPU-indexed draws.
- **GPU skinning** — glTF skeletal animation: `.anim` clips, joint-palette
  skinning in the mesh pipeline, animated-pose shadows/occluders, and
  conservative culling bounds for deformed meshes.
- **Shadows** — cascaded sun + per-spot shadow atlases, EVSM soft shadows,
  deferred shadow maps (DPS) with debug visualizers.
- **Lighting & GI** — analytic lights, SSR, raymarched volumetric fog with
  fog-GI sampling, SSAO / SAO, cubemap GI bakes + convex probe volumes
  (Embree ray tracing) cooked offline into the content packs. Lightmaps
  are baked but deprecated (the runtime loader stays for existing content).
- **Post-processing** — auto exposure, bloom, GT7-style tonemap, DoF,
  motion blur, SMAA (1x/T2x), optional NVIDIA DLSS-SR / Reflex via the
  Streamline interposer.
- **Content pipeline** — loose glTF/KTX2 assets are imported, cooked and
  packed into LZ4-compressed `*.pak` archives, served through a VFS +
  artifact DB.
- **Editor** — ImGui-based world editor with gizmos, asset registry
  (SQLite), threaded job pool, command system with recordable tours.
- **Profiling** — Optick CPU/GPU instrumentation, headless deterministic
  bench/capture modes, golden-image regression tooling.

## Status

Personal learning project ("toy" is the operative word) — APIs and data
formats are not stable and will break without notice. Format versions are
magic-bumped instead of carrying compat shims, by design.

**Work in progress, and pretty broken.** The following subsystems are active
experiments: they compile and run for the scenes they were developed
against, but expect gaps, visual artifacts, stale bakes and API churn.

- **Terrain** — procedural tiles, height painting and LOD stitching are
  unfinished; seams, missing tiles and unstable edits are known.
- **GI (global illumination)** — probe cubemaps, convex GI volumes,
  dense regular grids, and lightmap baking are preview-quality; bakes
  may be stale, incomplete or visibly wrong outside the scenes they
  were authored for. (The legacy world-field octree is retired —
  its runtime, bake machinery, serializer and analyzer are deleted.)

Treat both as preview-quality, not production features.

## Screenshots

GPU meshlet clusters, false-colored per cluster over the Sponza interior, with
the debug HUD and View menu open:

![GPU meshlet clusters](docs/images/meshlets_showing.png)

Debug tooling: the texture viewer (including the BC5 normal maps), the VRAM
usage breakdown and the Tourmaline Debug panel:

![Debug menus and VRAM usage](docs/images/debug_menus.png)

## Requirements

Common to both platforms:

- **CMake 3.17+** and **Ninja** — the repo is Ninja-only and
  out-of-source-only (the Visual Studio generator is rejected at configure
  time)
- **KTX-Software** (KTX/KTX2 texture container) — set `KTX_ROOT` to its
  install prefix, or pass `-DKtx_DIR=<prefix>/lib/cmake/ktx` (note the
  capital-K package name)
- Optional: **NVIDIA Streamline 2.14 runtime** (DLSS-SR / Reflex) — fetched
  by the installers below; if absent, DLSS simply reports unavailable
  (no hard dependency)

Windows 10/11 x64:

- **Visual Studio 2022+ (MSVC, C++20)** — builds run from a plain shell
  via `vcvarsall`
- **Vulkan SDK** — provides the `dxc.exe` that compiles HLSL → SPIR-V at
  build time (`VULKAN_SDK` env var must be set)

Linux (verified on Ubuntu 24.04):

- **GCC 14+** (recommended — see the platform notes in the Linux build
  section) or Clang
- **Vulkan loader** (`libvulkan-dev` or an installed SDK) — the full
  modern Vulkan headers are fetched by `install_dependencies.sh`, so the
  distro's header age does not matter
- KTX-Software has no distro package — build and install it once
  (commands in the Linux build section)

## Dependencies

Third-party code lives under `external/` in three forms: **unmodified
upstream libraries are pinned git submodules** (clone with `--recursive`),
**trimmed/patched trees and single headers are vendored** in-tree, and the
**platform binary packages are fetched by the installers** (not in git).
The full license audit lives in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Pinned submodules (unmodified upstream):

- **GLM** — math
- **nlohmann/json** — JSON (required by tinygltf)
- **meshoptimizer** — LOD simplification, meshlets, vertex-cache/fetch
  optimization
- **DirectXTex** — BC6H/BC7 block codecs (GI volume + lightmap payload encoding, cubemap compression)
- **volk** — Vulkan loader
- **SPIRV-Reflect** — CPU reflection for push/spec readback
- **nanosvg** — editor SVG icon rasterization

Vendored, built from source:

- **Embree** — ray-tracing backend for the GI/lightmap bakes
- **Dear ImGui** — editor and engine UI
- **xatlas** — lightmap UV2 charting + packing
- **LZ4** — block codec for cooked pak blobs
- **SQLiteCpp** + the sqlite3 amalgamation — the editor's artifact cache
- **Optick** — CPU/GPU profiler (`ENABLE_OPTICK`, on by default)

Vendored, header-only:

- **tinygltf** — glTF parsing (`tools/gltf_import`)
- **stb** — `stb_image.h` / `stb_image_write.h`
- **VulkanMemoryAllocator** — `vk_mem_alloc.h`

Fetched by the platform installers (`install_dependencies.ps1` on Windows,
`install_dependencies.sh` on Linux) — not in git:

- Windows: **SDL3** 3.4.16 prebuilt dev package, **DXC** (build-time
  shader compiles via the Vulkan SDK's `dxc.exe`; the engine also links
  the in-process `dxcompiler` for runtime HLSL→SPIR-V), **Streamline**
  2.14.1 (DLSS-SR / Reflex interposer + runtimes)
- Linux: **SDL3** 3.4.16 source tree (built as a subproject when no
  system SDL3 exists), the official **DXC** Linux package, full
  **Vulkan-Headers** (ahead of older distro headers), and
  **DirectX-Headers / DirectXMath** (DirectXTex's non-Windows path)

Vendored but not wired into the build (intentional):

- **Box3D** — upcoming physics work
- **zlib** — CRC/reference only

Not vendored but required at build time: the **Vulkan SDK** (Windows) and
**KTX-Software** (both platforms — see Requirements above).

## Build

### Windows

```powershell
# 0. Submodules (skip if you cloned with --recursive):
git submodule update --init --recursive

# 1. Enter the MSVC environment (plain PowerShell; adjust the path to
#    your Visual Studio version/edition):
cmd /c "`"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`" x64 && powershell"

# 2. Point CMake at KTX (example location):
$env:KTX_ROOT = 'C:\SDKs\ktx'

# 3. Configure (Ninja, out-of-source, Release):
cmake -B out/build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=Release .

# 4. Build a target:
cmake --build out/build/x64-Release --target App       # pak-viewer app
cmake --build out/build/x64-Release --target Editor   # the editor
cmake --build out/build/x64-Release --target all       # everything (tools, samples, tests)

# 5. Test:
ctest --test-dir out/build/x64-Release --output-on-failure
```

Debug builds: same with `-DCMAKE_BUILD_TYPE=Debug` in `out/build/x64-Debug`.
`build_app.bat` wraps the vcvarsall + build steps for the common case.

### Linux

The tree builds on Linux (tested on Ubuntu 24.04, GCC 14) — the same
CMake/Ninja flow, with `install_dependencies.sh` instead of the `.ps1`:

```bash
# 0. Submodules + fetched deps:
git submodule update --init --recursive
./install_dependencies.sh   # + the Linux pins: SDL3 source, DXC linux
                            #   package, Vulkan-Headers 1.4.357,
                            #   DirectX-Headers/DirectXMath (DirectXTex)

# 1. System packages (Ubuntu 24.04 names; gcc-14/g++-14 are REQUIRED —
#    see the toolchain note below; run BEFORE install_dependencies.sh
#    so curl and friends are available):
sudo apt install build-essential gcc-14 g++-14 cmake ninja-build git curl unzip \
  pkg-config glslang-tools libvulkan-dev zlib1g-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxinerama-dev \
  libxi-dev libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libxkbcommon-x11-dev libwayland-dev libegl1-mesa-dev libgl1-mesa-dev \
  libasound2-dev libpulse-dev libdbus-1-dev libudev-dev libunwind-dev \
  libpipewire-0.3-dev libdecor-0-dev libgbm-dev libdrm-dev libv4l-dev \
  libinput-dev

# 2. KTX-Software (Ubuntu has no package; build/install once):
git clone --depth 1 --branch v4.4.2 https://github.com/KhronosGroup/KTX-Software
cmake -S KTX-Software -B KTX-Software/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKTX_FEATURE_TOOLS=OFF -DKTX_FEATURE_TESTS=OFF -DKTX_FEATURE_DOC=OFF
cmake --build KTX-Software/build && sudo cmake --install KTX-Software/build

# 3. Configure + build (GCC 14 recommended; see the toolchain note below):
CC=gcc-14 CXX=g++-14 cmake -B out/build/linux-Release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release .
cmake --build out/build/linux-Release --target App

# 4. Test:
ctest --test-dir out/build/linux-Release --output-on-failure
```

Platform notes:

- **GCC 14+ recommended.** GCC 13 works for most of the tree but hits
  two compiler bugs in the engine (`gimplify` ICEs on array-NSDMI
  aggregates; see the NOTE at `struct ShaderProgram` in
  `engine/include/Engine/smvk.h`). Clang is expected to work but is
  not exercised in CI.
- **The PCH is an MSVC/Clang feature.** GCC gets pch.h force-included
  as a plain `-include` instead (GCC's `.gch` does not carry
  `#pragma once` state; see `engine/CMakeLists.txt`).
- **Vulkan headers**: distro `libvulkan-dev` packages ship older
  trimmed headers (Ubuntu 24.04: 1.3.275, missing 1.4-era symbols the
  engine uses), so the build pins the full Vulkan-Headers 1.4.357 (the
  same set the Windows SDK uses) ahead of the system headers. The
  loader stays the system's.
- **SDL3**: a system/distro SDL3 install wins (`find_package`); when
  absent, the 3.4.16 source tree fetched by `install_dependencies.sh`
  is built as a subproject.
- **DLSS/Streamline stays unavailable on Linux** (the runtime set is
  Windows-only; the engine falls back to the renderScale/bilinear path
  and logs a one-time notice).
- **Content**: no external content store is read by the build — the
  small runtime resources (fonts, LUTs) are tracked in `app/content/`
  and synced next to the exe; scene content ships inside paks, passed
  explicitly (`App --pak <path>` is REQUIRED).
- **The runtime render loop expects a real GPU.** Builds, tests
  (42/42 parity with Windows) and boot + world loading are verified;
  the software rasterizer (lavapipe) crashes inside its compute-pipeline
  compiler on the engine's BDA-heavy shaders.

### How it's built

- **CMake + Ninja, out-of-source only.** In-source configures and the
  Visual Studio generator are hard-rejected at configure time (they pollute
  the tree with `.vcxproj`/`Testing/` junk and fight the Ninja build dirs).
- **Shaders are build products.** `engine/shaders/*.vert|frag|cs.hlsl`
  (entry points `mainVS`/`mainPS`/`mainCS`) compile through
  `cmake/CompileShaders.cmake` with DXC (`-spirv -fspv-target-env=vulkan1.3
  -fvk-use-dx-layout`) into `out/build/<config>/content/shaders/*.spv`,
  synced next to the executables. Debug compiles `-Od` (a few files pinned
  `-O3` — see `HLSL_FORCE_O3_FILES`); Release compiles `-O3` and strips
  debug info. Rebuild the `App` target after editing any shader.
- **Third-party deps build from source.** `external/` mixes pinned git
  submodules (unmodified upstream) with vendored trees (locally trimmed —
  e.g. Embree: static lib, internal tasking, triangle geometry only, tests
  and CPack cut); the root `CMakeLists.txt` configures each and links them.
- **Push-constant contract.** C++ structs (`engine/include/Engine/Rendergraph/`)
  and their HLSL mirrors stay layout-identical, guarded by
  `static_assert(sizeof(...))` and contract tests.
- **42 plain executables as tests** (no gtest): hand-rolled `Check()`
  runners with `checks=/failures=` summaries, wired through `ctest`.
  Gated behind CMake's standard `BUILD_TESTING` (ON by default;
  `-DBUILD_TESTING=OFF` skips the test targets entirely — a dedicated
  `out/build/tests` tree with `-DBUILD_TESTING=ON` keeps them out of
  working configs). On GitHub, CI (`ci.yml`) verifies compilation only;
  the suite runs in the separate, independent `Tests` workflow
  (`tests.yml`), with the five GPU-window tests excluded on hosted
  runners (no display / Vulkan ICD).

## Run

Executables run from their build directory (e.g. `out/build/x64-Release`)
and load `content/` relative to the CWD. The app is a pak viewer — content
is cooked separately through the project pipeline (below):

```powershell
.\App.exe --pak content/paks/sponza.pak    # the demo pak (built by the
                                           # project pipeline — see below)
```

Headless deterministic capture / benchmarking (the full A/B matrix is
declared in `engine/include/Engine/App/RunConfig.h`):

```powershell
$env:TOURMALINE_SSR='1'; $env:TOURMALINE_AA='off'; $env:TOURMALINE_AUTO_EXPOSURE='0'
$env:TOURMALINE_CAM_POS='24.26,1.82,-9.52'; $env:TOURMALINE_CAM_FWD='-0.947,-0.022,-0.321'
$env:TOURMALINE_CAPTURE='48'    # frame to export exports/screenshot_*.png (must be < bench)
& .\App.exe --pak content/paks/sponza.pak --disable_control --bench 64
```

- `--bench <n>` runs 10 warmup + n measured frames, then exits (vsync
  forced off — uncapped immediate present; `--present <mode>` wins).
- The same env vars pin features headless for A/B benches:
  `TOURMALINE_AO=off|ssao|sao`, `TOURMALINE_EVSM`, `TOURMALINE_SSR`,
  `TOURMALINE_TRANSPARENCY`, `TOURMALINE_VOLUMETRIC[_STEPS|_CVOL]`,
  `TOURMALINE_MESH`, `TOURMALINE_CULLFLAGS`, `TOURMALINE_ASYNC_COMPUTE`,
  `TOURMALINE_PROBE_PREMIX=0|1` (the Remember Me probe pre-mix — one
  blended-cubemap fetch + dominant-box parallax vs the per-pixel walk;
  see `docs/probe_premix_plan.md`)
  (Linux: same variables exported, `./App` instead of `.\App.exe`).

### Content pipeline (the canonical project flow)

One project → one importer → one geometry preprocessing stage → one
scene cooker → one packager. `Project.exe` is the entry point; the
editor and the batch scripts use the same implementation — there is no
alternate path that derives different geometry from the same source.

```
glTF / GLB source          (NOT in git — an external content store)
  |
  |  Project.exe init/import      the ONE importer (tools/gltf_import):
  |                               copy the source + deps INTO the project,
  |                               derive the .geometry family — the LOD
  |                               chains + meshlets bake AT IMPORT
  v
Project assets             (renderer-ready .geometry + .material + .anim
  |                         + the import manifest with its fingerprint)
  |  Project.exe build
  |     -> Cook                 scene-dependent bakes (cubemap captures);
  |     |                       consumes the imported .geometry, never
  |     |                       regenerates or rewrites it
  |     -> GiBaker              the world field + authored GI volumes
  |     -> Pak                  assembly + the bake-freshness gate + verify
  v
*.pak                      -> App.exe / Editor.exe (VFS + artifact DB)
```

**The demo (Sponza).** One command creates the project, imports the
source, seeds the authored scene, cooks, bakes, paks (the freshness gate
accepts only fresh bakes) and syncs the pak next to `App.exe`; later runs
are incremental (unchanged content reimports and bakes nothing):

```powershell
# Your content store root — must contain models\sponza\Sponza.gltf +
# Sponza.bin + the textures + the authored Sponza.tour.
# Defaults to F:\content on the author's machine; override anywhere:
$env:TOURMALINE_CONTENT_DIR = 'D:\my-content-store'

tools\cook\cook_sponza.bat        # or the explicit commands:
#   Project.exe init  --project <dir> --from-gltf <store>\models\sponza\Sponza.gltf `
#                       --from-scene <store>\models\sponza\Sponza.tour
#   Project.exe build --project <dir> --output <dir>\paks\Sponza.pak
#   Project.exe status --project <dir>     # explains staleness per source

cd out\build\x64-Release
.\App.exe --pak content/paks/sponza.pak
```

The project defaults to your **user Documents** — `%USERPROFILE%\Documents\Tourmaline\Sponza` on Windows (`~/Documents/Tourmaline/Sponza`
elsewhere), never the repo tree — and `TOURMALINE_PROJECT_DIR` overrides
it. It is **self-contained**: after the first successful build it rebuilds
with no external content store present at all — the imported copies are
the source of record, and a project directory can be copied to another
machine and built there as-is. The demo content itself is not in git
(large scene binaries live in the external store); set
`TOURMALINE_CONTENT_DIR` or copy a built project over.

The editor drives the same pipeline in-process (its Cook command wraps
the same entry points the CLI tools expose), and every bake stage records
content fingerprints the pak gate re-verifies: stale bakes are refused,
never silently embedded.

## Repository layout

```
engine/      engine static lib (RHI, render graph, renderers, assets, shaders)
editor/      the ImGui-based editor
app/         App.exe — pak-viewer host shell
tools/       gltf_import, cook, pak_cooker, gi_baker, cubemap_baker,
             artifact_cache, python helpers (bench/golden capture)
samples/     minimal Vulkan samples (01_triangle, 02_blur, 03_fuzz)
tests/       42 plain-exe test executables (ctest)
external/    third-party sources: pinned git submodules + vendored trees
             (see THIRD_PARTY_NOTICES.md)
docs/        project guide + editor tour documentation
```

- `AGENTS.md` — build/run/test quick reference for humans and agents.
- `docs/project_guide.md` — systems, seams, and where to extend things.
- `THIRD_PARTY_NOTICES.md` — full third-party license audit.

## AI disclosure

Development of Tourmaline was assisted by AI coding agents: **Muse Spark
1.3** and **GLM 5.3** (Z.ai, driven through the opencode CLI). They
contributed to implementation, debugging, testing and documentation
across the codebase; the architecture, review and final decisions are the
author's.

## License

Copyright (c) 2026 Samuel Martin. Released under the **MIT License** — see
[LICENSE](LICENSE). Third-party components keep their own licenses, see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The scene format is derived from the scene description format in
[Mastering Graphics Programming with Vulkan](https://github.com/PacktPublishing/Mastering-Graphics-Programming-with-Vulkan)
(Castorino & Portociampo, Packt, MIT) — see the third-party notice in
[LICENSE](LICENSE).
