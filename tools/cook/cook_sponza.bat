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
rem The atlas width is fixed at the default 1024 (--lm-atlas-width); the
rem height grows with the content up to --lm-max-height 8192 (1024x8192
rem = 8M texels at the cap) — no texel budget anymore, the density
rem search only runs if the height cap overflows. 0.03125 m/texel = the
rem old 32 texels/m default. The bake charts a voxel-remesh scene proxy
rem once (--lm-proxy-res 256) and UV-transfers the layout to the meshes.
rem Payload compression defaults to bc6hopt (Frostbite invalid-texel
rem prefill + BC6H blocks, one texture per lobe, ~8x smaller GPU
rem footprint than RGBA16F; --lm-compress uncompressed opts out).
rem --cubemap on bakes GGX-preconvolved specular cubemaps INTO the pak
rem (textures/cubemaps/sponza[_N].ktx2 + .cubemanifest, high-frequency
rem specular only; probes/lightmaps own diffuse + low-freq specular).
rem --cubemap-octree on = sparse adaptive placement (open volumes coarse,
rem near-surface cells refine, 32-probe budget); --cubemap-png on writes
rem face/mip/probe-map debug PNGs loose next to the pak inputs.
rem --lods on is frostbite: siblings cook BEFORE the bake and inherit
rem the LOD0 atlas layout through UV transfer (one bake, one atlas).
rem See tools/cook/cook_sponza_fast.bat for the low-quality iteration
rem variant (coarse density, 5 lobes, uncompressed, no LODs/meshlets).
rem --cubemap-extra-pos appends artist-PINNED probes (manifest 'pin' rows
rem sort first at load, winning weight ties): 0,0.25,0 = the chrome ball
rem center, so the ball mirrors a capture taken exactly at itself.
build\Release\PakCooker.exe --content-dir F:\content --in-vfs models/sponza/Sponza.geometry --out-pak F:\content\paks\sponza.pak --cubemap on --cubemap-png on --meshlets on --lods on --verify on --lightmap on --lm-meters-per-texel 0.1 --cubemap on  --cubemap-size 128 --cubemap-rays 512 --cubemap-grid 0 --cubemap-octree on --cubemap-octree-max 8.0 --cubemap-octree-min 2.0 --cubemap-octree-depth 4 --cubemap-extra-pos 0,0.25,0 --cubemap-extra-radius 1.5
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
