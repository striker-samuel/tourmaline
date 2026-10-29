# Tourmaline

[![CI](https://github.com/striker-samuel/tourmaline/actions/workflows/ci.yml/badge.svg)](https://github.com/striker-samuel/tourmaline/actions/workflows/ci.yml)
[![Tests](https://github.com/striker-samuel/tourmaline/actions/workflows/tests.yml/badge.svg)](https://github.com/striker-samuel/tourmaline/actions/workflows/tests.yml)

A C++20 toy engine and renderer for learning real-time graphics on Vulkan 1.3 —
a personal project by Samuel Martin, built from scratch (RHI, render graph,
GPU-driven culling, content pipeline and editor included).

Windows (MSVC) and Linux (GCC 14, verified on Ubuntu 24.04) both build the
full tree and pass the CPU test suite. Everything is Ninja-driven and
shader-hot: HLSL sources compile to SPIR-V at build time and load next to
the executable. The runtime render loop needs a real Vulkan GPU (see
[Platform support](#platform-support)).

**Heads-up:** this repository contains **no ready-to-run scene content**.
The viewer (`App.exe`) loads cooked `*.pak` archives; building the demo
Sponza pak requires an external content store and the project pipeline
(see [Content pipeline and sample scene](#content-pipeline-and-sample-scene)).

## Screenshots

GPU meshlet clusters, false-colored per cluster over the Sponza interior, with
the debug HUD and View menu open:

![GPU meshlet clusters](docs/images/meshlets_showing.png)

Debug tooling: the texture viewer (including the BC5 normal maps), the VRAM
usage breakdown and the Tourmaline Debug panel:

![Debug menus and VRAM usage](docs/images/debug_menus.png)

## Current features

- **Custom Vulkan 1.3 RHI** — bindless descriptor pools, render graph with
  automatic barrier placement, staging uploads, GPU timeline semaphores.
- **GPU-driven rendering** — HZB occlusion culling, GPU mesh filter with
  LOD chains, clustered Forward+ lighting, GPU-indexed draws.
- **GPU skinning** — glTF skeletal animation: `.anim` clips, joint-palette
  skinning in the mesh pipeline, animated-pose shadows/occluders, and
  conservative culling bounds for deformed meshes.
- **Shadows** — cascaded sun + per-spot shadow atlases, EVSM soft shadows,
  deferred shadow maps (DPS) with debug visualizers, a coarse volumetric
  shadow tile for fog sun-occlusion.
- **Lighting & GI** — analytic lights, SSR, raymarched volumetric fog with
  fog-GI sampling, SSAO / SAO, cubemap GI bakes + convex probe volumes and
  dense regular grids (Embree ray tracing) cooked offline into the content
  packs. Lightmaps are baked but deprecated (the runtime loader stays for
  existing content).
- **Post-processing** — auto exposure, bloom, GT7-style tonemap, DoF,
  motion blur, SMAA (1x/T2x), optional NVIDIA DLSS-SR / Reflex via the
  Streamline interposer (Windows only; the engine falls back to its native
  render-scale path when absent).
- **Content pipeline** — loose glTF/KTX2 assets are imported, cooked and
  packed into LZ4-compressed `*.pak` archives, served through a VFS +
  artifact DB. `Project.exe` is the one CLI entry point for the whole flow.
- **Editor** — ImGui-based world editor with gizmos, asset registry
  (SQLite), threaded job pool, command system with recordable tours.
- **Profiling** — Optick CPU/GPU instrumentation, headless deterministic
  bench/capture modes, golden-image regression tooling (dev machines).

## Experimental status and known limitations

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
  may be stale, incomplete or visibly wrong outside the scenes they were
  authored for. (The legacy world-field octree is retired — its runtime,
  bake machinery, serializer and analyzer are deleted.)

Treat both as preview-quality, not production features.

## Platform support

What is actually verified, and what is not:

- **Build verification**: both platforms compile the full tree in CI
  ([CI](https://github.com/striker-samuel/tourmaline/actions/workflows/ci.yml)
  — build-only, `-DBUILD_TESTING=OFF`).
- **CPU tests**: 44 plain-executable tests run per platform in the
  separate
  [Tests](https://github.com/striker-samuel/tourmaline/actions/workflows/tests.yml)
  workflow, with the five GPU-window tests excluded on hosted runners
  (no display / Vulkan ICD — see [Testing](#testing)). Linux test parity
  with Windows is verified on Ubuntu 24.04 + GCC 14 (WSL).
- **Runtime rendering** requires a **real Vulkan GPU** and is verified on
  the author's Windows machines. A successful build + CPU test pass does
  **not** guarantee working rendering on any given machine. The lavapipe
  software rasterizer boots and loads, but crashes inside its
  compute-pipeline compiler on the engine's BDA-heavy shaders — do not
  report that as a port bug without a GPU-verified repro.
- Clang is expected to work on Linux but is not exercised in CI. Other
  distributions/compilers are unverified.

## Requirements

- **Windows 10/11 x64** or **Ubuntu 24.04-class Linux x64**.
- **CMake 3.17+** and **Ninja** — the repo is Ninja-only and
  out-of-source-only (the Visual Studio *generator* is rejected at
  configure time; the VS IDE itself is fine — see
  [Visual Studio IDE](#visual-studio-ide)).
- **Git** — the tree pins unmodified upstream libraries as git submodules
  (clone with `--recursive`).

Installed by you, per platform:

| Dependency | Windows | Linux | Notes |
|---|---|---|---|
| Visual Studio 2022+ | required | — | MSVC + the "Desktop development with C++" workload (includes CMake + Ninja) |
| GCC 14 | — | required | `gcc-14`/`g++-14` packages; see the [toolchain note](#linux-notes) |
| Vulkan SDK | required | optional | Windows: provides `dxc.exe` (HLSL→SPIR-V at build time); `VULKAN_SDK` env var must point at it. Linux: the installer fetches the official DXC package, a full SDK is not needed |
| KTX-Software 4.4.2 | required | required | No distro package — build/install once (commands in both quick starts) |
| Embree 4.4.1 prebuilt | recommended | — | Windows: the vendored Embree **source** build ICEs MSVC (C1001 on the AVX curve-intersector TUs) — use the official prebuilt package. Linux: the vendored source build is the normal route |

Everything else is fetched by the repo's installer script
(`install_dependencies.ps1` / `install_dependencies.sh`) — see
[Dependencies](#dependencies). The installers do **not** install the
Vulkan SDK or KTX; those stay manual.

## Quick start — Windows

Steps 1–2 run from any shell; steps 3–6 need the MSVC environment, so
use the **Developer PowerShell for Visual Studio** (Start menu — it
opens PowerShell with `cl.exe`, `cmake` and `ninja` already on PATH).
Steps 1–4 are one-time setup. Commands prefixed `PS>` are PowerShell.

**Prerequisites**: Visual Studio 2022 or newer with the
*Desktop development with C++* workload (this also provides CMake and
Ninja), Git, and the [Vulkan SDK](https://vulkan.lunarg.com/) installed
with its `VULKAN_SDK` environment variable set (the SDK installer does
this; verify with `Test-Path "$env:VULKAN_SDK\Bin\dxc.exe"`).

### 1. Clone with submodules

```powershell
PS> git clone --recursive https://github.com/striker-samuel/tourmaline.git
PS> cd tourmaline
# (already cloned? just: git submodule update --init --recursive)
```

### 2. Fetch the pinned dependencies

Downloads the vendored source trees and Windows binary packages
(see [Dependencies](#dependencies)) into `external/`, SHA256-checked.
Idempotent — re-run is a no-op.

```powershell
PS> powershell -NoProfile -ExecutionPolicy Bypass -File .\install_dependencies.ps1
```

This does **not** fetch the Vulkan SDK (install it yourself, above) or
KTX (next step).

### 3. Build and install KTX-Software 4.4.2

There is no Windows package for KTX-Software — build and install it
once from the same source configuration CI uses. **Run this from the
"Developer PowerShell for Visual Studio"** (Start menu — it opens a
shell with the MSVC compiler already initialized); any install prefix
works, this example uses `%USERPROFILE%\ktx`:

```powershell
PS> git clone --depth 1 --branch v4.4.2 https://github.com/KhronosGroup/KTX-Software $env:USERPROFILE\ktx-src
PS> cmake -S $env:USERPROFILE\ktx-src -B $env:USERPROFILE\ktx-build -G Ninja `
       -DCMAKE_BUILD_TYPE=Release `
       -DKTX_FEATURE_TOOLS=OFF -DKTX_FEATURE_TESTS=OFF -DKTX_FEATURE_DOC=OFF `
       -DCMAKE_INSTALL_PREFIX=$env:USERPROFILE\ktx
PS> cmake --build $env:USERPROFILE\ktx-build
PS> cmake --install $env:USERPROFILE\ktx-build
```

### 4. Get the prebuilt Embree package (recommended)

The official Embree 4.4.1 Windows package avoids the MSVC ICE in the
vendored source build. Download it, verify the checksum (the same pin CI
uses), extract it anywhere you like — this example uses
`%USERPROFILE%\embree-4.4.1`:

```powershell
PS> curl.exe -L -o $env:TEMP\embree.zip `
       https://github.com/embree/embree/releases/download/v4.4.1/embree-4.4.1.x64.windows.zip
PS> $hash = (Get-FileHash $env:TEMP\embree.zip -Algorithm SHA256).Hash.ToLower()
PS> if ($hash -ne '5aa1fa3161a720f9c610b06c652b80f421cb2b1fe8221fdf92b74157336fdf8f') { throw "embree zip sha mismatch: $hash" }
PS> Expand-Archive $env:TEMP\embree.zip -DestinationPath $env:TEMP\embree-x
PS> Move-Item "$env:TEMP\embree-x\embree-4.4.1.x64.windows" $env:USERPROFILE\embree-4.4.1
```

Without a prebuilt package the configure falls back to building the
vendored Embree source — which currently **crashes the MSVC compiler**
(C1001 + 0xC0000005 on the AVX curve-intersector TUs) on the author's
toolchain, so the prebuilt route is strongly recommended on Windows.

### 5. Configure and build

Open the **Developer PowerShell for Visual Studio** (Start menu) — it has
`cl.exe`, `cmake` and `ninja` on PATH — and `cd` into the repo.

```powershell
PS> $env:KTX_ROOT = "$env:USERPROFILE\ktx"
PS> cmake -B out/build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=Release `
       -DEMBREE_PREBUILT_DIR="$env:USERPROFILE\embree-4.4.1" .

# Build what you need:
PS> cmake --build out/build/x64-Release --target App       # the pak-viewer host
PS> cmake --build out/build/x64-Release --target Editor    # the world editor
PS> cmake --build out/build/x64-Release --target Project   # the content-pipeline CLI
PS> cmake --build out/build/x64-Release                    # everything (tools, samples, tests)
```

`EMBREE_PREBUILT_DIR` must be the extracted package root (the directory
containing `lib\cmake`). Debug builds are the same commands with
`-DCMAKE_BUILD_TYPE=Debug` and the `out/build/x64-Debug` directory.

`build.bat` (from any shell, including a plain one — it finds MSVC via
`vswhere` or reuses an initialized environment, auto-configures missing
build dirs, and honors the `EMBREE_PREBUILT_DIR` env var) and its
`build_app.bat` shortcut wrap the common cases:

```bat
build.bat                 full tree, x64-Release
build.bat App             one target
build.bat App x64-Debug   target + config
build_app.bat             == build.bat App
```

### 6. Test

```powershell
PS> ctest --test-dir out/build/x64-Release --output-on-failure
```

See [Testing](#testing) for the GPU-test caveats.

### 7. Run

Executables run **from their build directory** and load `content/`
relative to the CWD:

```powershell
PS> cd out\build\x64-Release
PS> .\App.exe --pak content\paks\Sponza.pak
```

`--pak` is **required** — and the pak does not exist yet on a fresh
checkout. Build it through the project pipeline first (see
[Content pipeline and sample scene](#content-pipeline-and-sample-scene)).

The editor needs a project directory (it never picks one itself —
`EditorLauncher.exe` is the picker):

```powershell
PS> .\EditorLauncher.exe                  # the launcher: recents + New/Browse
PS> .\Editor.exe --project <project-dir>  # direct (the launcher spawns this)
```

## Quick start — Linux

Verified configuration: **Ubuntu 24.04 + GCC 14**. Other distributions
are unverified. Run from bash in your clone.

```bash
# 1. System packages FIRST (curl etc. must exist before the dependency
#    installer runs; gcc-14/g++-14 are REQUIRED - see the toolchain note):
sudo apt update
sudo apt install -y build-essential gcc-14 g++-14 cmake ninja-build git curl unzip \
  pkg-config glslang-tools libvulkan-dev zlib1g-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxinerama-dev \
  libxi-dev libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libxkbcommon-x11-dev libwayland-dev libegl1-mesa-dev libgl1-mesa-dev \
  libasound2-dev libpulse-dev libdbus-1-dev libudev-dev libunwind-dev \
  libpipewire-0.3-dev libdecor-0-dev libgbm-dev libdrm-dev libv4l-dev \
  libinput-dev

# 2. Submodules (or clone with --recursive):
git clone --recursive https://github.com/striker-samuel/tourmaline.git
cd tourmaline

# 3. Fetched deps: SDL3 3.4.16 source, the official DXC linux package,
#    full Vulkan-Headers 1.4.357, DirectX-Headers/DirectXMath (DirectXTex):
./install_dependencies.sh

# 4. KTX-Software (no distro package; build/install once):
git clone --depth 1 --branch v4.4.2 https://github.com/KhronosGroup/KTX-Software
cmake -S KTX-Software -B KTX-Software/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKTX_FEATURE_TOOLS=OFF -DKTX_FEATURE_TESTS=OFF -DKTX_FEATURE_DOC=OFF
cmake --build KTX-Software/build && sudo cmake --install KTX-Software/build
# (a user-local prefix works too: add -DCMAKE_INSTALL_PREFIX=$HOME/ktx and
#  pass -DKTX_ROOT=$HOME/ktx at the engine configure instead of sudo)

# 5. Configure + build (GCC 14; see the toolchain note):
CC=gcc-14 CXX=g++-14 cmake -B out/build/linux-Release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release .
cmake --build out/build/linux-Release --target App

# 6. Test:
ctest --test-dir out/build/linux-Release --output-on-failure
```

Embree here builds from the vendored source (fetched by step 3) — the
normal route on Linux. To run: `cd out/build/linux-Release && ./App --pak ...`
(see [Running](#running-the-viewer-and-editor) — the pak requirement
applies identically).

### Linux notes

- **GCC 14+ required-ish.** GCC 13 works for most of the tree but hits
  two compiler bugs in the engine (`gimplify` ICEs on array-NSDMI
  aggregates; see the NOTE at `struct ShaderProgram` in
  `engine/include/Engine/smvk.h`). Clang is expected to work but is not
  exercised in CI.
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
- **Runtime needs a real GPU** — see [Platform support](#platform-support).

## Building and testing

### Build targets

| Target | What it is |
|---|---|
| `App` | the pak-viewer host (`app/`) |
| `Editor` / `EditorLauncher` | the world editor + its project picker (`editor/`) |
| `Project` | the content-pipeline CLI (`tools/project`) |
| `Import` | the raw glTF→engine-format converter CLI (`tools/import`) |
| `Cook`, `Pak`, `GiBaker`, `CubemapBaker` | the cook/bake/pak stage executables (`tools/`) |
| `01_triangle`, `02_blur`, `03_fuzz` | minimal Vulkan samples (`samples/`) |
| *(no target)* | `cmake --build <dir>` builds everything, tests included |

### Testing

44 plain-executable tests (no gtest): hand-rolled `Check()` runners with
`checks=/failures=` summaries, wired through `ctest`. They are gated
behind CMake's standard `BUILD_TESTING` (ON by default):

```powershell
# Normal configs include the tests - run them directly:
ctest --test-dir out/build/x64-Release --output-on-failure

# A dedicated tests tree keeps them out of working configs:
cmake -B out/build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON .
cmake --build out/build/tests
ctest --test-dir out/build/tests --output-on-failure

# Or exclude the test targets entirely:
cmake -B out/build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF .
```

Five of the 44 create a real (hidden) SDL window + offscreen Vulkan
device — `retirement_fuzz_tests`, `world_fields_gpu_tests`,
`placement_gpu_tests`, `probepremix_gpu_tests`,
`rhi_upload_barriers_gpu_tests` — and need a desktop session with a
working Vulkan driver. They run in dev environments and are excluded in
CI; everything else is pure CPU and runs anywhere.

### CI and the Tests workflow

Two independent GitHub workflows, deliberately decoupled (a test failure
cannot fail the build check and vice versa):

- **[CI](https://github.com/striker-samuel/tourmaline/actions/workflows/ci.yml)**
  — build verification only: full-tree compile on Ubuntu 24.04/GCC 14 and
  Windows/MSVC, `-DBUILD_TESTING=OFF`, nothing executed.
- **[Tests](https://github.com/striker-samuel/tourmaline/actions/workflows/tests.yml)**
  — the suite: builds with `-DBUILD_TESTING=ON` on both platforms and
  runs ctest with the five GPU-window tests excluded (hosted runners
  have no display / Vulkan ICD). Manually launchable
  (*workflow_dispatch*), own concurrency group.

### How it's built

- **CMake + Ninja, out-of-source only.** In-source configures and the
  Visual Studio generator are hard-rejected at configure time (they
  pollute the tree with `.vcxproj`/`Testing/` junk and fight the Ninja
  build dirs).
- **Shaders are build products.** `engine/shaders/*.vert|frag|cs.hlsl`
  (entry points `mainVS`/`mainPS`/`mainCS`) compile through
  `cmake/CompileShaders.cmake` with DXC (`-spirv
  -fspv-target-env=vulkan1.3 -fvk-use-dx-layout`) into
  `out/build/<config>/content/shaders/*.spv`, synced next to the
  executables by the `EngineScene_shaders` target. Debug compiles `-Od`
  (a few files pinned `-O3` — see `HLSL_FORCE_O3_FILES`); Release
  compiles `-O3` and strips debug info. Rebuild the `App` target after
  editing any shader.
- **Push-constant contract.** C++ structs
  (`engine/include/Engine/Rendergraph/`) and their HLSL mirrors stay
  layout-identical, guarded by `static_assert(sizeof(...))` and contract
  tests.
- **KTX discovery**: CMake accepts `-DKtx_DIR=<prefix>/lib/cmake/ktx`,
  `-DCMAKE_PREFIX_PATH=<prefix>`, or the `KTX_ROOT` env var (translated
  to `CMAKE_PREFIX_PATH`). Note the capital-K package name — KTX-Software
  ships `KtxConfig.cmake`.

### Visual Studio IDE

The **Visual Studio IDE works with the Ninja configuration**: open the
repo folder (File → Open → Folder) and the checked-in
`CMakeSettings.json` picks up the `x64-Debug` / `x64-Release` (+
DSViz / ShaderDebug) Ninja configurations with the MSVC environment
inherited. The **Visual Studio CMake generator** (`-G "Visual Studio
..."`) is unsupported and rejected at configure time — never switch a
build directory to it; if a `CMakeCache.txt` mismatch ever appears,
delete the offending `out/build/<config>` directory and reconfigure
with Ninja.

## Running the viewer and editor

Executables run from their build directory (e.g.
`out/build/x64-Release`) and load `content/` relative to the CWD. The
app is a pak viewer — `--pak <path>` is **required**, and content is
cooked separately through the project pipeline:

```powershell
PS> cd out\build\x64-Release
PS> .\App.exe --pak content\paks\Sponza.pak    # built by the pipeline (below)
```

Headless deterministic capture / benchmarking — env vars pin features
for A/B benches (the full set is parsed in
`engine/src/App/RunConfig.cpp`; `AGENTS.md` carries the annotated list):

```powershell
PS> $env:TOURMALINE_SSR='1'; $env:TOURMALINE_AA='off'; $env:TOURMALINE_AUTO_EXPOSURE='0'
PS> $env:TOURMALINE_CAM_POS='24.26,1.82,-9.52'; $env:TOURMALINE_CAM_FWD='-0.947,-0.022,-0.321'
PS> $env:TOURMALINE_CAPTURE='48'    # frame to export exports/screenshot_*.png (must be < bench)
PS> .\App.exe --pak content\paks\Sponza.pak --disable_control --bench 64
```

- `--bench <n>` runs 10 warmup + n measured frames, then exits (vsync
  forced off — uncapped immediate present; `--present <mode>` wins).
- Other pins in the same family: `TOURMALINE_AO=off|ssao|sao`,
  `TOURMALINE_EVSM`, `TOURMALINE_TRANSPARENCY`,
  `TOURMALINE_VOLUMETRIC` (+`_STEPS`, `_GI_VOLUMES`),
  `TOURMALINE_MESH`, `TOURMALINE_CULLFLAGS`,
  `TOURMALINE_ASYNC_COMPUTE`, `TOURMALINE_PROBE_PREMIX=0|1`,
  `TOURMALINE_GI_INTENSITY`, `TOURMALINE_NORMAL_FIX` — and many more
  (Linux: same variables exported, `./App` instead of `.\App.exe`).

The editor: `Editor.exe --project <dir>` (the flag is required — the
editor never picks a project itself) or `EditorLauncher.exe` for the
picker UI (recents + New/Browse). The editor drives the same content
pipeline in-process as the CLI tools.

## Content pipeline and sample scene

**The demo scene (Sponza) is not in this repository.** Large scene
binaries live in an external content store; the repo ships only the
engine, tools and small runtime resources (fonts, LUTs). To get a
runnable pak you either point the pipeline at your own content store,
or copy an already-built project directory from somewhere else.

One project → one importer → one geometry preprocessing stage → one
scene cooker → one packager. `Project.exe` is the entry point; the
editor and the batch scripts use the same implementation — there is no
alternate path that derives different geometry from the same source.

```
glTF / GLB source          (NOT in git — an external content store)
  |
  |  Project.exe init/import      the ONE importer (gltf_import):
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
  |     -> GiBaker              the GI volumes + world fields
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
# Sponza.bin + the textures + the authored Sponza.tour:
PS> $env:TOURMALINE_CONTENT_DIR = 'D:\my-content-store'

PS> tools\cook\cook_sponza.bat        # or the explicit commands:
#   Project.exe init  --project <dir> --from-gltf <store>\models\sponza\Sponza.gltf `
#                       --from-scene <store>\models\sponza\Sponza.tour
#   Project.exe build --project <dir> --output <dir>\paks\Sponza.pak
#   Project.exe status --project <dir>     # explains staleness per source

PS> cd out\build\x64-Release
PS> .\App.exe --pak content\paks\Sponza.pak
```

The project defaults to your **user Documents** —
`%USERPROFILE%\Documents\Tourmaline\Sponza` on Windows
(`~/Documents/Tourmaline/Sponza` elsewhere), never the repo tree — and
`TOURMALINE_PROJECT_DIR` overrides it. It is **self-contained**: after
the first successful build it rebuilds with no external content store
present at all — the imported copies are the source of record, and a
project directory can be copied to another machine and built there
as-is.

Every bake stage records content fingerprints the pak gate re-verifies:
stale bakes are refused, never silently embedded.

## Dependencies

Third-party code lives under `external/`. The full license audit is in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

**Pinned git submodules** (in git, unmodified upstream — clone with
`--recursive`):

- **GLM** — math
- **nlohmann/json** — JSON (required by tinygltf)
- **meshoptimizer** — LOD simplification, meshlets, vertex-cache/fetch
  optimization
- **DirectXTex** — BC6H/BC7 block codecs (GI volume + lightmap payload
  encoding, cubemap compression)
- **volk** — Vulkan loader
- **SPIRV-Reflect** — CPU reflection for push/spec readback
- **nanosvg** — editor SVG icon rasterization

**Fetched by the installer scripts** (not in git — pinned
archives/raw files, SHA256-checked, extracted into `external/`):

- Vendored source trees, built by the normal build: **Embree**
  (ray tracing for the GI bakes — the prebuilt-package route on Windows
  is recommended instead, see the quick start), **Dear ImGui**,
  **xatlas** (lightmap UV2 charting + packing), **LZ4** (block codec for
  cooked pak blobs), **SQLiteCpp** + the sqlite3 amalgamation (the
  editor's artifact cache), **Optick** (profiler, `ENABLE_OPTICK` on by
  default).
- Header-only: **tinygltf** (glTF parsing, used by `tools/gltf_import`),
  **stb** (`stb_image.h` / `stb_image_write.h`),
  **VulkanMemoryAllocator** (`vk_mem_alloc.h`).
- Windows binary packages: **SDL3** 3.4.16 prebuilt dev package,
  **DXC** (the in-process `dxcompiler` the engine links for runtime
  HLSL→SPIR-V; build-time compiles use the Vulkan SDK's `dxc.exe`),
  **Streamline** 2.14.1 (the optional DLSS-SR / Reflex interposer —
  no hard dependency, the engine falls back when absent).
- Linux-only fetches: **SDL3** 3.4.16 source (built as a subproject when
  no system SDL3 exists), the official **DXC** Linux package, full
  **Vulkan-Headers** 1.4.357 (ahead of older distro headers), and
  **DirectX-Headers / DirectXMath** (DirectXTex's non-Windows path).

**Installed manually by you** (never fetched): the **Vulkan SDK**
(Windows) and **KTX-Software** (both platforms) — see
[Requirements](#requirements).

**Fetched but intentionally not wired into the build**: **Box3D**
(upcoming physics work) and **zlib** (CRC/reference only) — pass
`-IncludeUnused` / `--include-unused` to the installers if you want
them.

## Architecture and repository layout

```
engine/      engine static lib (RHI, render graph, renderers, assets, shaders)
editor/      the ImGui-based editor + its launcher
app/         App.exe — the pak-viewer host shell
tools/       import (Import.exe), gltf_import (the importer lib),
             project (Project.exe) + project_pipeline (the shared lib),
             cook (Cook.exe + cook_sponza.bat), pak_cooker (Pak.exe),
             gi_baker, cubemap_baker, artifact_cache (lib),
             deps (installer pin manifests), push_constants_idl (codegen),
             python helpers (golden capture, shader -Od retry)
samples/     minimal Vulkan samples (01_triangle, 02_blur, 03_fuzz)
tests/       44 plain-exe test executables (ctest)
external/    third-party: pinned git submodules + installer-fetched trees
             (see THIRD_PARTY_NOTICES.md)
docs/        project guide + per-system documentation
```

Frame data flow: `DrawFrame` fills the frame context → GPU-driven filter
(seed/HZB/mesh filter) → Forward+ (prepass + light culling) → shadow
atlases (CSM/spot/EVSM/deferred/coarse fog tile) → main forward pass →
motion/DoF/SSR/volumetric/exposure/bloom/tonemap/SMAA/UI → swapchain.
The GPU-driven filtered path is always on (the direct-draw leg remains
only as the unfilterable/LOD-far fallback).

## Related documentation

- `AGENTS.md` — build/run/test quick reference for humans and agents.
- `docs/project_guide.md` — systems, seams, and where to extend things.
- `docs/editor_tour_and_context.md` — the editor tour system.
- `docs/target_heap_packing.md`, `docs/probe_premix_plan.md`,
  `docs/dx12_porting_notes.md`, `docs/world_partition.md` — per-system
  deep dives.
- `THIRD_PARTY_NOTICES.md` — the full third-party license audit.

## AI disclosure

Development of Tourmaline was assisted by AI coding agents: **Muse Spark
1.3** and **GLM 5.3** (Z.ai, driven through the opencode CLI). They
contributed to implementation, debugging, testing and documentation
across the codebase; the architecture, review and final decisions are
the author's.

## License

Copyright (c) 2026 Samuel Martin. Released under the **MIT License** — see
[LICENSE](LICENSE). Third-party components keep their own licenses, see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The scene format is derived from the scene description format in
[Mastering Graphics Programming with Vulkan](https://github.com/PacktPublishing/Mastering-Graphics-Programming-with-Vulkan)
(Castorino & Portociampo, Packt, MIT) — see the third-party notice in
[LICENSE](LICENSE).
