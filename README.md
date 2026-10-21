# Tourmaline

A C++20 toy engine and renderer for learning real-time graphics on Vulkan 1.3 —
a personal project by Samuel Martin, built from scratch (RHI, render graph,
GPU-driven culling, content pipeline and editor included).

Everything is Windows-first, MSVC-compiled, Ninja-driven and shader-hot: HLSL
sources compile to SPIR-V at build time and load next to the executable.

## Features

- **Custom Vulkan 1.3 RHI** — bindless descriptor pools, render graph with
  automatic barrier discipline, staging uploads, GPU timeline semaphores,
  strict-resource exhaustion contract (call sites decide policy, the RHI
  returns error handles).
- **GPU-driven rendering** — HZB occlusion culling + GPU mesh filter with
  LOD chains, clustered Forward+ lighting, indexed draws skipping eligible
  mesh ranges (exactly-once discipline).
- **GPU skinning** — glTF skeletal animation: `.anim` clips with the
  root-bone basis fix, joint-palette skinning in the mesh pipeline,
  animated-pose prepass/shadows/occluders sharing the deformed pose, and
  deformed meshlet cones + per-draw union bounds (GfxSkinBounds) keeping
  skinned culling conservative by default (`TOURMALINE_SKIN_CONE` A/B
  lever); pinned end-to-end by the knight companion scene
  (knight_anim_tests).
- **Shadows** — cascaded sun + per-spot shadow atlases, EVSM soft shadows,
  deferred shadow maps (DPS) with debug visualizers.
- **Lighting & GI** — analytic lights, SSR, raymarched volumetric fog with
  fog-GI sampling, SSAO / SAO, lightmap baking (xatlas charting + Embree
  ray tracing) and cubemap GI bakes cooked offline into the content packs.
- **Post-processing** — auto exposure, bloom, GT7-style tonemap, DoF,
  motion blur, SMAA (1x/T2x, shader port + embedded LUTs), optional
  NVIDIA DLSS-SR / Reflex via the Streamline interposer.
- **Content pipeline** — loose glTF/KTX2 assets are imported, cooked and
  packed into `*.pak` archives (LZ4-compressed, magic-versioned, no legacy
  conversions), served through a VFS + artifact DB.
- **Editor** — ImGui-based world editor with gizmos, asset registry
  (SQLite), fiber job backend, command system with recordable tours.
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
- **GI (global illumination)** — probe cubemaps, convex volumes, the
  world-field octree and lightmap baking are preview-quality; bakes may be
  stale, incomplete or visibly wrong outside the scenes they were authored
  for.

Treat both as preview-quality, not production features.

## Screenshots

GPU meshlet clusters, false-colored per cluster over the Sponza interior, with
the debug HUD and View menu open:

![GPU meshlet clusters](docs/images/meshlets_showing.png)

Debug tooling: the texture viewer (including the BC5 normal maps), the VRAM
usage breakdown and the Tourmaline Debug panel:

![Debug menus and VRAM usage](docs/images/debug_menus.png)

## Requirements

- **Windows 10/11 x64**
- **Visual Studio 2022+ (MSVC, C++20)** — `cl.exe` and the MSVC headers;
  builds run from a plain shell via `vcvarsall`
- **CMake 3.17+** and **Ninja** (the Visual Studio generator is rejected
  at configure time — this repo is Ninja-only and out-of-source-only)
- **Vulkan SDK** (provides `dxc.exe` used to compile HLSL → SPIR-V;
  `VULKAN_SDK` env var must be set)
- **KTX-Software** (KTX/KTX2 texture container) — set `KTX_ROOT` to its
  install prefix, or pass `-Dktx_DIR=<prefix>/lib/cmake/ktx`
- Optional, **editor only**: **Boost 1.89** static libs (context/fiber)
  staged via `BOOST_ROOT` for the fiber job backend — the engine, app and
  tools never link Boost
- Optional: **NVIDIA Streamline 2.x runtime** — vendored binaries are
  staged automatically next to the exe; if absent, DLSS simply reports
  unavailable (no hard dependency)

Third-party code under `external/` comes in two forms: **unmodified upstream
libraries are pinned git submodules** (GLM, nlohmann/json, meshoptimizer,
DirectXTex, volk, SPIRV-Reflect, nanosvg — clone with `--recursive`), while
**locally trimmed/patched trees and binary packages** (Embree, SDL3, ImGui,
Optick, SQLiteCpp, xatlas, lz4, Streamline, VMA, ...) are vendored and built
from source — see `THIRD_PARTY_NOTICES.md` for the full audit.

## Build

```powershell
# 0. Submodules (skip if you cloned with --recursive):
git submodule update --init --recursive

# 1. Enter the MSVC environment (plain PowerShell):
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
- **27 plain executables as tests** (no gtest): hand-rolled `Check()`
  runners with `checks=/failures=` summaries, wired through `ctest`.

## Run

Executables run from their build directory (e.g. `out/build/x64-Release`)
and load `content/` relative to the CWD. The app is a pak viewer — content
is cooked separately (below):

```powershell
.\App.exe --pak content/paks/bistro.pak
```

Headless deterministic capture / benchmarking (the full A/B matrix is
declared in `engine/include/Engine/App/RunConfig.h`):

```powershell
$env:TOURMALINE_SSR='1'; $env:TOURMALINE_AA='off'; $env:TOURMALINE_AUTO_EXPOSURE='0'
$env:TOURMALINE_CAM_POS='24.26,1.82,-9.52'; $env:TOURMALINE_CAM_FWD='-0.947,-0.022,-0.321'
$env:TOURMALINE_CAPTURE='48'    # frame to export exports/screenshot_*.png (must be < bench)
& .\App.exe --pak content/paks/bistro.pak --disable_control --bench 64
```

- `--bench <n>` runs 10 warmup + n measured frames, then exits (vsync
  forced off — uncapped immediate present; `--present <mode>` wins).
- `TOURMALINE_AO=off|ssao|sao`, `TOURMALINE_EVSM`, `TOURMALINE_SSR`,
  `TOURMALINE_TRANSPARENCY`, `TOURMALINE_VOLUMETRIC[_STEPS|_CVOL]`,
  `TOURMALINE_MESH`, `TOURMALINE_CULLFLAGS`, `TOURMALINE_ASYNC_COMPUTE`,
  `TOURMALINE_PROBE_PREMIX=0|1` (the Remember Me probe pre-mix — one
  blended-cubemap fetch + dominant-box parallax vs the per-pixel walk;
  see `docs/probe_premix_plan.md`)
  pin features headless for A/B benches.

### Content pipeline

```
loose glTF / KTX2 assets
  -> tools/gltf_import     (mesh/material extraction)
  -> tools/cook            (asset cooking: geometry, textures, manifests)
  -> tools/gi_baker / tools/cubemap_baker / lightmaps   (Embree ray tracing)
  -> tools/pak_cooker      (*.pak archives: LZ4, magic-versioned)
  -> App.exe / Editor.exe   (VFS + artifact DB on top of the paks)
```

`tools/cook_bistro.py` and friends show a full end-to-end example against
the Bistro test scene (scripts reference local asset locations).

## Repository layout

```
engine/      engine static lib (RHI, render graph, renderers, assets, shaders)
editor/      the ImGui-based editor (Boost.Fibers job backend optional)
app/         App.exe — pak-viewer host shell
tools/       gltf_import, cook, pak_cooker, gi_baker, cubemap_baker,
             artifact_cache, python helpers (bench/cook/golden capture)
samples/     minimal Vulkan samples (01_triangle, 02_blur)
tests/       27 plain-exe test executables (ctest)
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
