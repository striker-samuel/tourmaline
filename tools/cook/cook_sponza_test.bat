@echo off
rem Test-quality sponza cook: slower than cook_sponza_fast.bat but
rem still low quality — a middle ground for iterating on bake/atlas
rem behavior (chart overlap debugging, artifact inspection) with
rem visible shadows and readable texels, without the full-quality
rem cook time.
rem
rem Settings rationale:
rem   --lm-meters-per-texel 0.0625  16 texels/m: readable atlas, still
rem                                 coarse (full cook uses 0.03125 = 32)
rem   --lm-rays 64 --lm-bounces 2  soft shadows + one indirect bounce
rem   --lm-lobes 5 --lm-lobe-relerr 0  all meshes pinned at the 5-lobe
rem                                 basis (cheapest fit, no scoring)
rem   --lm-compress uncompressed    classic 9 RGBA16F layers (no BC6H
rem                                 pass; drop-in for artifact A/B)
rem   --lm-curve-target 0           uniform density
rem Usage: run from anywhere (paths anchor at the repo root).
setlocal
pushd "%~dp0..\.."
if not exist build\Release\PakCooker.exe (
  echo ERROR: build\Release\PakCooker.exe not found.
  echo Build it first: cmake --build build --config Release --target PakCooker
  popd
  exit /b 1
)
build\Release\PakCooker.exe --content-dir F:\content --in-vfs models/sponza/Sponza.gltf --out-pak F:\content\paks\sponza.pak --verify on --lightmap on --lm-meters-per-texel 0.0625 --lm-rays 64 --lm-bounces 2 --lm-lobes 5 --lm-lobe-relerr 0 --lm-compress uncompressed --lm-curve-target 0
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak build\Debug\content\paks\sponza.pak
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak build\Release\content\paks\sponza.pak
if errorlevel 1 goto fail
copy /y F:\content\gi\sponza.sglm build\Debug\content\gi\sponza.sglm
if errorlevel 1 goto fail
copy /y F:\content\gi\sponza.sglm build\Release\content\gi\sponza.sglm
if errorlevel 1 goto fail
echo sponza.pak test-cooked (low quality, uncompressed) and synced.
popd
exit /b 0
:fail
popd
exit /b 1
