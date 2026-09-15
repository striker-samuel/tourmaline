@echo off
rem Recook sponza.pak with the current engine structs (64-byte StaticVertex,
rem 128-byte MaterialValues) and sync it into the build content dirs the App
rem loads (content/paks relative to the App working directory).
rem Usage: run from anywhere (paths anchor at the repo root via this script).
setlocal
pushd "%~dp0..\.."
if not exist build\Release\PakCooker.exe (
  echo ERROR: build\Release\PakCooker.exe not found.
  echo Build it first: cmake --build build --config Release --target PakCooker
  popd
  exit /b 1
)
rem NOTE: --lightmap on is load-bearing here, not optional. It bakes the
rem per-vertex lightmap UVs + per-mesh lobe counts INTO the pak and writes
rem the atlas to F:\content\gi\sponza.sglm alongside. A pak cooked without
rem it carries lmChunk=-1 everywhere (F8 palette goes all-green) while a
rem stale sponza.sglm lingers next to it.
rem --lm-budget 8M texels (~576 MB of RGBA16F atlas layers) lets the
rem density search converge far above the 4M default (sponza's ornament
rem needs it; the convergence log prints the achieved texels/m^2).
rem --lods on cooks the sibling chain AFTER the lightmap bake
rem (CookLodSiblings): siblings inherit the stamped atlas UVs, chart ids
rem keep collapses inside one lightmap chart, and every level is audited
rem for parameterization drift (see [LM-LOD] lines; log-only by default).
build\Release\PakCooker.exe --content-dir F:\content --in-vfs models/sponza/Sponza.gltf --out-pak F:\content\paks\sponza.pak --meshlets on --lods on --verify on --lightmap on --lm-budget 8388608
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak build\Debug\content\paks\sponza.pak
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak build\Release\content\paks\sponza.pak
if errorlevel 1 goto fail
copy /y F:\content\gi\sponza.sglm build\Debug\content\gi\sponza.sglm
if errorlevel 1 goto fail
copy /y F:\content\gi\sponza.sglm build\Release\content\gi\sponza.sglm
if errorlevel 1 goto fail
echo sponza.pak recooked and synced.
popd
exit /b 0
:fail
popd
exit /b 1
