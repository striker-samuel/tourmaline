# Bistro demo — recreate-from-source (Windows & Linux)

The **Amazon Lumberyard Bistro** exterior demo (CC BY 4.0, see
[`../../BISTRO-CREDITS.md`](../../BISTRO-CREDITS.md)). The cooked pak is
~1.4 GB and the source textures are ~700 MB, so neither is committed to
the git tree. This directory carries the small *definition* — recipe,
scene tour, glTF, identity ledger and the 254 derived material assets —
enough to recreate the demo from the ORCA source. This README is the
agent-facing, cross-platform flow; the maintainer notes are at the end.

## TL;DR

```powershell
# Windows (PowerShell)
.\tools\recreate_bistro.ps1
```
```bash
# Linux
./tools/recreate_bistro.sh
```

Each script verifies the source is in place and drives the cook, or
prints exactly what is missing. The steps below are the manual path.

## 1. Prerequisites

| | Windows | Linux |
|---|---|---|
| Build | Visual Studio 2022 (MSVC) + CMake | GCC/Clang + CMake (or the repo's `install_dependencies.sh`) |
| Build dir | `out/build/x64-Release` | `out/build/linux-Release` |
| glTF conversion | Blender (4.x) | Blender (4.x) |
| texture conversion | `tools/ktx_bulk_convert.py` + `toktx` (KHR KTX-Software) | same |

The engine build is the normal one (see the top-level README). The
conversion tools are only needed to regenerate the glTF/`.ktx2` assets;
they are not part of the engine build.

## 2. Build the engine

```powershell
# Windows
cd <repo>
cmd /c "build.bat App x64-Release"
cmd /c "build.bat Project x64-Release"
```
```bash
# Linux
cd <repo>
cmake --build out/build/linux-Release --target App Project
```

`Project.exe`/`Project` is the cooker; `App.exe`/`App` is the viewer.

## 3. Download the ORCA source

Download the Amazon Lumberyard Bistro from NVIDIA ORCA (FBX + textures):

  <https://developer.nvidia.com/orca/amazon-lumberyard-bistro>

This is a ~800 MB archive (FBX geometry + TGA/PNG textures). Unpack it
anywhere outside the repo (it is licensed content, not source).

## 4. Convert to glTF + KTX2

The committed [`content/models/bistro_static/bistro_static.gltf`](content/models/bistro_static/bistro_static.gltf)
is the **authoritative URI layout**. Regenerate the payloads so they
match it:

1. **FBX → glTF.** Import the Bistro FBX in Blender, then
   *File → Export → glTF 2.0* with an **external `.bin` buffer** named
   `bistro.bin`. The scene must reference the same nodes/materials as the
   committed glTF (the exterior is static; the committed `.meta` identity
   ledger keeps GUIDs stable across re-imports).
2. **Textures → KTX2.** Convert the 343 source textures to `.ktx2` with
   the same relative URIs the committed glTF names (e.g.
   `objects/natural/bushes/buxus/buxus_ddna.ktx2`,
   `textures/shared/tiling/wood_planks_01/…_diff.ktx2`). The repo tool is
   `tools/ktx_bulk_convert.py` (drives `toktx`).

Place the results beside the committed glTF:

```
releases/bistro/content/models/bistro_static/
  bistro_static.gltf      (committed)
  bistro_static.gltf.meta (committed)
  *.material              (committed, 254 files)
  bistro.bin              (you generate)
  objects/**/*.ktx2       (you generate)
  textures/**/*.ktx2      (you generate)
```

## 5. Cook

```powershell
# Windows
.\out\build\x64-Release\Project.exe build `
  --project releases/bistro `
  --recipe releases/bistro/bistro_static.tourrecipe `
  --publish releases/bistro/build --manifest-name bistro_static
```
```bash
# Linux
./out/build/linux-Release/Project build \
  --project releases/bistro \
  --recipe releases/bistro/bistro_static.tourrecipe \
  --publish releases/bistro/build --manifest-name bistro_static
```

The recipe has `bake gi off` / `bake cubemaps off` (the exterior authors
no GI volumes or cubemaps), so those stages exit clean. The output is a
content-addressed pak (`bistro_static-<hash>.pak`) plus
`bistro_static.tapp` under `releases/bistro/build/`.

## 6. Run

```powershell
# Windows — pak (stable-name copy)
Copy-Item releases\bistro\build\bistro_static-*.pak out\build\x64-Release\content\paks\bistro.pak
.\out\build\x64-Release\App.exe --pak content\paks\bistro.pak

# or via the manifest
.\out\build\x64-Release\App.exe --project releases\bistro\build\bistro_static.tapp
```
```bash
# Linux
cp releases/bistro/build/bistro_static-*.pak out/build/linux-Release/content/paks/bistro.pak
./out/build/linux-Release/App --pak content/paks/bistro.pak
```

## Verify

The task/mesh mesh-shader path should engage (not the indexed fallback).
At the benchmark camera (`24.26, 1.82, -9.52` looking `-0.947,-0.022,-0.321`):

```
[GpuSceneTaskMesh] active: workCapacity=2909 ...
[perf] task/mesh drawn tris 2223272 ...
```

## What is committed here

| path | role |
|---|---|
| `bistro_static.tourrecipe` | the recipe build definition |
| `content/scenes/bistro_static.tour` | the scene tour (static-root + `setStatic`) |
| `content/models/bistro_static/bistro_static.gltf` | the glTF (materials, lights, texture URIs, geometry buffer ref) |
| `content/models/bistro_static/bistro_static.gltf.meta` | the identity ledger (stable GUIDs) |
| `content/models/bistro_static/*.material` | the 254 derived material assets |

## Maintainer notes

- The source is obtained from
  <https://developer.nvidia.com/orca/amazon-lumberyard-bistro> (CC BY 4.0).
- `bistro.bin` (96 MB) + the 343 `.ktx2` textures (~700 MB) are the only
  things NOT committed; everything else is in this directory.
- The recipes are `bistro_static.tourrecipe` (this directory) and
  `bistro.tourrecipe` (the meshlet stress variant). The static recipe is
  the one that cooks cleanly (the full `bistro.tour` references vegetation
  source textures that the KTX2 set does not carry).
- A format bump in the engine (e.g. the SMP9 meshlet-trailer change) breaks
  old paks; re-cook from zero with the current `Project` build.
