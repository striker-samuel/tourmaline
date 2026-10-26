@rem Copyright (c) 2026 Samuel Martin
@rem SPDX-License-Identifier: MIT

@echo off
rem Recook sponza.pak with the current engine structs (64-byte StaticVertex,
rem 128-byte MaterialValues) and sync it into the ninja build content dir the
rem App loads (content/paks relative to the App working directory).
rem Usage: run from anywhere (paths anchor at the repo root via this script).
setlocal
pushd "%~dp0..\.."
if not exist out\build\x64-Release\Cook.exe (
  echo ERROR: out\build\x64-Release\Cook.exe not found.
  echo Build it first: cmake --build out/build/x64-Release --target Cook
  popd
  exit /b 1
)
if not exist out\build\x64-Release\Pak.exe (
  echo ERROR: out\build\x64-Release\Pak.exe not found.
  echo Build it first: cmake --build out/build/x64-Release --target Pak
  popd
  exit /b 1
)
rem NOTE: lightmaps are DEPRECATED (the lit FS never samples the atlas;
rem sg_lightmap.hlsli is commented out and no .sglm is loaded at runtime),
rem so the bake stays OFF here (--lightmap off is explicit so it also wins
rem over any tour cook block that enables it). A pak cooked without it
rem carries lmChunk=-1 everywhere (the FS gates on lmChunk none and never
rem samples it) and no sglm bake file is written. The --lm-* tuning below is
rem inert without the trigger (kept as the last-good bake config).
rem --cubemap on bakes GGX-preconvolved specular cubemaps loose under the
rem VFS root (gi/cubemaps/<stem>[_N].ktx2 + <stem>.cubemanifest,
rem high-frequency specular only; probes/lightmaps own diffuse + low-freq
rem specular). The capture set is NAMELESS: the base is the input
rem artifact's stem (Sponza.geometry -> "Sponza"), which is exactly what
rem pak.exe's side-car-stem discovery embeds (gi/cubemaps/<stem>).
rem The old --cubemap-name flag is gone (the "sky" default baked fine
rem but never landed in the pak: gi=0, cubemaps=0, no specular IBL).
rem --cubemap-octree on = sparse adaptive placement (open volumes coarse,
rem near-surface cells refine, 32-probe budget); --cubemap-png on writes
rem face/mip/probe-map debug PNGs loose next to the pak inputs.
rem --lods on is frostbite: siblings cook BEFORE the bake and inherit
rem the LOD0 atlas layout through UV transfer (one bake, one atlas).
rem --meshlets on cooks the meshlet clusters (the VK_EXT_mesh_shader
rem filter path).
rem SUN: --sun-azimuth 30 --sun-elevation 80 are EXPLICIT so the scene's
rem authored env block (az=0/el=0 - a horizon sun imported from glTF)
rem can never win (EnvForBake: CLI flags override the scene env). 55 deg
rem elevation = the engine's canonical daylight default (matches the
rem runtime environment defaults + the Pak docs).
rem --cubemap-extra-pos appends artist-PINNED probes (manifest 'pin' rows
rem sort first at load, winning weight ties): 0,0.25,0 = the chrome ball
rem center, so the ball mirrors a capture taken exactly at itself.
rem PIPELINE (import/cook/pak split): import.exe derives the .geometry
rem once; cook.exe runs the bakes + writes them back into the artifacts
rem (+ Sponza.cooked); pak.exe assembles the .pak from the cooked
rem artifacts (pure serialization - bake flags are rejected there).
rem SUN flags ride BOTH steps (cook bakes with that sun, pak serializes it).
rem AMBIENT CLOSURE OFF (the default): sponza's pak carries the GI stack
rem (GiBaker below), so the runtime ambient comes from the .sggi octree +
rem GI volumes - baking the closure into the captures would DOUBLE-COUNT it
rem (mirror-like surfaces - the chrome probe ball, gold trim - reflect
rem ~traced-bounce + closure ~= 2x the ambient-lit field). Stack-less
rem cooks (bistro) opt in with --cubemap-ambient-closure on.
rem --mask-transmission on: the MASK cutout CLOTH (the banners) rides
rem BOTH paths in every bake - its full lit albedo (the banners' captured
rem look stays) AND a partial pass-through tinted per texel by the
rem baseColor texture (the light through the weave; cutout holes pass
rem unobstructed; the tint stays real in the white-world GI bake - the
rem tinted light pools behind the banners ARE the point). Bistro keeps
rem it OFF (foliage approximates volume by its occlusion).
rem GI VOLUMES (both layers of the probes-only pivot): GiBaker runs
rem BETWEEN cook + pak - F:/content/models/sponza/Sponza.tour authors
rem the convex GI volumes (the per-space layer: dense local probe grids
rem over the nave/atrium/galleries + the SEALED side aisles the coarse
rem global octree underserves) and its --out writes the GLOBAL layer
rem (gi/Sponza.sggi, the sparse adaptive octree - "the .sggi and .givolume
rem files pair up"). Pak.exe embeds both (gi/<stem>.sggi + the givolumemanifest
rem + every .givolume row) + registers the bindings; the runtime resolves
rem them out of the pak. White-world GI fields (uniform albedo 0.7 - the
rem established convention: per-material albedo prints chromatic dots
rem the 9-lobe fit cannot represent, verified on Sponza); the cubemap
rem captures bake TEXTURED (per-hit baseColor sampling - see bake.h).
out\build\x64-Release\Cook.exe F:\content models/sponza/Sponza.geometry --cubemap on --cubemap-png on --meshlets on --lods on --lightmap off --lm-meters-per-texel 0.1 --sun-azimuth 30 --sun-elevation 80 --sun-intensity 10000 --sky-intensity 6000 --cubemap-size 128 --cubemap-rays 512 --cubemap-grid 0 --cubemap-octree on --cubemap-octree-max 8.0 --cubemap-octree-min 2.0 --cubemap-octree-depth 4 --cubemap-extra-pos 0,0.25,0 --cubemap-extra-radius 1.5 --mask-transmission on
if errorlevel 1 goto fail
out\build\x64-Release\GiBaker.exe --content-dir F:/content --tour F:/content/models/sponza/Sponza.tour --out F:/content/gi/Sponza.sggi --backend embree --max-probes 131072 --mask-transmission on
if errorlevel 1 goto fail
out\build\x64-Release\Pak.exe F:\content models/sponza/Sponza.geometry --out-pak F:\content\paks\sponza.pak --verify on --sun-azimuth 30 --sun-elevation 80 --sun-intensity 10000 --sky-intensity 6000
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak out\build\x64-Release\content\paks\sponza.pak
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak out\build\x64-Debug\content\paks\sponza.pak
if errorlevel 1 goto fail
echo sponza.pak recooked and synced.
popd
exit /b 0
:fail
popd
exit /b 1
