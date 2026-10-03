# AGENTS.md

## Build system

**Always configure and build with the Ninja generator.**

```powershell
# Configure (Release)
cmake -B out/build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=Release .

# Build
cmake --build out/build/x64-Release --target App

# Configure (Debug)
cmake -B out/build/x64-Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug .
cmake --build out/build/x64-Debug --target App
```

Never use the Visual Studio generator — the build directories in `out/build/`
are Ninja-configured, and mixing generators fails with "generator : Ninja does
not match the generator used previously". If a `CMakeCache.txt` mismatch ever
appears, delete the offending `out/build/<config>` directory and reconfigure
with Ninja.

MSVC environment: `cl.exe` needs `vcvarsall` when building from a plain shell:

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul && cmake --build out/build/x64-Release --target App"
```

## Shaders

- Source: `engine/shaders/` (HLSL, compiled by DXC via `cmake/CompileShaders.cmake`)
- Output: `out/build/<config>/content/shaders/*.spv`, synced next to `App.exe`
- Editing a shader: just rebuild the `App` target — the `EngineScene_shaders` dependency
  recompiles changed `.hlsl` and syncs the `.spv`
- HLSL entry points: `mainVS`/`mainPS`/`mainCS` by file extension
  (`.vert.hlsl`/`.frag.hlsl`/`.cs.hlsl`)
- Debug builds compile shaders `-Od`; files listed in `HLSL_FORCE_O3_FILES`
  (`cmake/CompileShaders.cmake`, currently `gfxfilter.cs.hlsl` — its `-Od`
  SPIR-V AVs nvgpucomp64.dll) are pinned to `-O3` even in Debug

## Running

`App.exe` runs from the build directory (`out/build/x64-Release`), loading
`content/paks/*.pak` and `content/shaders/*.spv` relative to CWD.

Headless-ish deterministic capture (auto-exits after the bench budget):

```powershell
$env:VULKANTOY_SSR='1'; $env:VULKANTOY_AA='off'; $env:VULKANTOY_AUTO_EXPOSURE='0'
$env:VULKANTOY_CAM_POS='24.26,1.82,-9.52'; $env:VULKANTOY_CAM_FWD='-0.947,-0.022,-0.321'
$env:VULKANTOY_LIGHTS_STATIC='1'; $env:VULKANTOY_CAPTURE='48'
& .\App.exe --pak content/paks/bistro.pak --disable_control --bench 64
```

- `VULKANTOY_CAPTURE=<frame>` exports `exports/screenshot_*.png` — the frame
  number must land INSIDE the bench budget (capture < bench; e.g. capture 12
  with bench 32), otherwise the auto-exit happens before the export
- `VULKANTOY_SSR_DUMP=<frame>` dumps SSR stage stats + PNGs to `exports/`
- `VULKANTOY_AO=off|ssao|sao` pins the AO mode headless (default sao; the O
  key / UI combo stay live); combined with `VULKANTOY_SSR`,
  `VULKANTOY_EVSM`, `VULKANTOY_TRANSPARENCY` this spans the full A/B matrix
- `--bench <n>` runs 10 warmup + n measured frames, then exits

## Tests

Tests are plain executables (no gtest), built by the normal target build and
run via ctest from the build directory:

```powershell
ctest --test-dir out/build/x64-Release --output-on-failure
```

## Conventions

- Push-constant structs: C++ (`engine/include/Engine/Rendergraph/*.h`) and
  HLSL mirrors must stay layout-identical — check the `static_assert(sizeof(...))`
  and the HLSL comment when editing either side
- Frame renderer plumbing: `engine::AppFrameContext` (engine/include/Engine/Rendergraph/FrameContext.h)
  carries per-frame handles; renderers snapshot what they need at Append time
- Heavy explanatory comments are the repo norm — keep the reasoning auditable
- Dont use F12 for any binding since it can trigger debugbreak