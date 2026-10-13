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
rem NOTE: lightmaps are DEPRECATED (the lit FS never samples the atlas;
rem sg_lightmap.hlsli is commented out and no .sglm is loaded at runtime),
rem so the bake stays OFF here (--lightmap off is explicit so it also wins
rem over any tour cook block that enables it). A pak cooked without it
rem carries lmChunk=-1 everywhere (the FS gates on lmChunk none and never
rem samples it) and no sglm sidecar is written. The --lm-* tuning below is
rem inert without the trigger (kept as the last-good bake config).
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
build\Release\PakCooker.exe --content-dir F:\content --in-vfs models/Bistro/bistro.geometry --out-pak F:\content\paks\bistro.pak --meshlets on --lods on --verify on --lightmap off --lm-meters-per-texel 0.25 --cubemap on --cubemap-size 128 --cubemap-rays 128
if errorlevel 1 goto fail
copy /y F:\content\paks\bistro.pak build\Debug\content\paks\bistro.pak
if errorlevel 1 goto fail
copy /y F:\content\paks\bistro.pak build\Release\content\paks\bistro.pak
if errorlevel 1 goto fail
rem sglm sync retired with the lightmap deprecation (no atlas is baked;
rem the runtime never loads one). Kept as comments for the path layout.
rem copy /y F:\content\gi\bistro.sglm build\Debug\content\gi\bistro.sglm
rem copy /y F:\content\gi\bistro.sglm build\Release\content\gi\bistro.sglm
echo bistro.pak recooked and synced.
popd
exit /b 0
:fail
popd
exit /b 1
