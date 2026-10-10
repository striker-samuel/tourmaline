@echo off
rem Recook bistro.pak with the current engine structs (64-byte StaticVertex,
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
rem the atlas to F:\content\gi\bistro.sglm alongside. A pak cooked without
rem it carries lmChunk=-1 everywhere (F8 palette goes all-green) while a
rem stale bistro.sglm lingers next to it.
rem The atlas width is fixed at the default 2048 (--lm-atlas-width); the
rem height grows with the content up to --lm-max-height 8192 (2048x8192
rem = 16M texels, ~1.1 GiB of RGBA16F layers at the cap) — no texel
rem budget anymore, the density search only runs if the height cap
rem overflows. 0.25 m/texel = 4 texels/m coarse full-scene density.
rem Payload compression defaults to bc6hopt (Frostbite invalid-texel
rem prefill + BC6H blocks, one texture per lobe, ~8x smaller GPU
rem footprint than RGBA16F; --lm-compress uncompressed opts out).
rem --cubemap on bakes GGX-preconvolved specular cubemaps INTO the pak
rem (textures/cubemaps/bistro.ktx2, high-frequency specular only;
rem probes/lightmaps own diffuse + low-freq specular).
rem --lods on is frostbite: siblings cook BEFORE the bake and inherit
rem the LOD0 atlas layout through UV transfer (one bake, one atlas).
build\Release\PakCooker.exe --content-dir F:\content --in-vfs models/Bistro/bistro.geometry --out-pak F:\content\paks\bistro.pak --meshlets on --lods on --verify on --lightmap on --lm-meters-per-texel 0.25 --cubemap on --cubemap-size 128 --cubemap-rays 128
if errorlevel 1 goto fail
copy /y F:\content\paks\bistro.pak build\Debug\content\paks\bistro.pak
if errorlevel 1 goto fail
copy /y F:\content\paks\bistro.pak build\Release\content\paks\bistro.pak
if errorlevel 1 goto fail
copy /y F:\content\gi\bistro.sglm build\Debug\content\gi\bistro.sglm
if errorlevel 1 goto fail
copy /y F:\content\gi\bistro.sglm build\Release\content\gi\bistro.sglm
if errorlevel 1 goto fail
echo bistro.pak recooked and synced.
popd
exit /b 0
:fail
popd
exit /b 1
