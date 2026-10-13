@echo off
rem Fast low-quality sponza iteration cook: coarse density, minimum
rem rays, 5 pinned lobes, UNCOMPRESSED payload (no BC6H encode pass —
rem the atlas uploads as the classic 9 RGBA16F stream layers), no LOD
rem chain, no meshlets. The lightmap bake stays OFF (deprecated; see
rem cook_sponza.bat). Overwrites sponza.pak with the iteration result —
rem re-run cook_sponza.bat to restore the full-quality cook.
rem
rem Settings rationale:
rem   --lm-meters-per-texel 0.25  4 texels/m: tiny atlas, fast trace
rem   --lm-rays 16 --lm-bounces 1 minimum sampling, direct+1 bounce
rem   --lm-lobes 5 --lm-lobe-relerr 0  every mesh pinned at the 5-lobe
rem                               basis (cheapest fit, no candidate
rem                               scoring) — the compression ratio and
rem                               the bake both win
rem   --lm-compress uncompressed   skip the BC6H encode entirely
rem   --lm-curve-target 0         no curvature density boost (uniform
rem                               coarse density)
rem Usage: run from anywhere (paths anchor at the repo root).
setlocal
pushd "%~dp0..\.."
if not exist build\Release\PakCooker.exe (
  echo ERROR: build\Release\PakCooker.exe not found.
  echo Build it first: cmake --build build --config Release --target PakCooker
  popd
  exit /b 1
)
build\Release\PakCooker.exe --content-dir F:\content --in-vfs models/sponza/Sponza.geometry --out-pak F:\content\paks\sponza.pak --verify on --lm-debug-pink on --meshlets on --lods on --verify on --lightmap off --lm-meters-per-texel 0.1 --lm-rays 16 --lm-bounces 1 --lm-lobes 5 --lm-lobe-relerr 0 --lm-compress uncompressed --lm-curve-target 0 --cubemap on --cubemap-size 64 --cubemap-rays 16
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak build\Debug\content\paks\sponza.pak
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak build\Release\content\paks\sponza.pak
if errorlevel 1 goto fail
rem sglm sync retired with the lightmap deprecation (no atlas is baked;
rem the runtime never loads one). Kept as comments for the path layout.
rem copy /y F:\content\gi\sponza.sglm build\Debug\content\gi\sponza.sglm
rem copy /y F:\content\gi\sponza.sglm build\Release\content\gi\sponza.sglm
echo sponza.pak fast-cooked (low quality, uncompressed) and synced.
popd
exit /b 0
:fail
popd
exit /b 1
