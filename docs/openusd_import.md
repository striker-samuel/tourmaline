# OpenUSD source import (optional)

The USD importer is an **offline authoring adapter**, not a Tourmaline renderer
or a USD runtime backend. A composed OpenUSD stage is translated into an
in-memory `tinygltf::Model`, then passed to Tourmaline's existing
`ImportMeshSource -> DeriveNativeAssets` pipeline. It produces native
`.geometry`, `.material`, and `<source>.meta` assets, which continue
through the ordinary cook/pak/project pipeline. Vulkan, scene tables, and the
runtime do not link to OpenUSD.

## Setup (Windows / Linux)

OpenUSD is **optional** and defaults OFF. Enable it only when you need to
import `.usd` / `.usda` / `.usdc` / `.usdz` sources. The build needs a CMake
`pxr` package exposing `usd`, `usdGeom` and `usdShade` targets (or their
`pxr::` aliases), plus Python 3.12 for the USD Python bindings.

### Acquire OpenUSD

- **Windows — prebuilt (recommended):** NVIDIA ships prebuilt OpenUSD
  binaries (e.g. `openusd_25.08`) carrying the `pxr` CMake package and the
  `usd_*.dll` runtime. Download and unpack to a stable path, e.g.
  `C:\openusd\openusd_25.08`, then point `CMAKE_PREFIX_PATH` at it. (The
  repo's `usd_shim` is an alternative that forwards to a prebuilt root
  beside it; use one or the other.)
- **Linux — build from source:** follow the upstream
  [OpenUSD build docs](https://openusd.org/release/building_usd.html)
  (Python, TBB, OpenSubdiv, OpenEXR/Imath, MaterialX). Install to a prefix
  such as `/opt/openusd`.

### Configure and build

```powershell
# Windows (x64 Developer PowerShell)
cmake -S . -B out/build/usd-Release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DTOURMALINE_WITH_USD=ON "-DCMAKE_PREFIX_PATH=C:\openusd\openusd_25.08"
cmake --build out/build/usd-Release --target Import
```
```bash
# Linux
cmake -S . -B out/build/usd-Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTOURMALINE_WITH_USD=ON -DCMAKE_PREFIX_PATH=/opt/openusd
cmake --build out/build/usd-Release --target Import
```

Normal prerequisites for building Tourmaline still apply. USD defaults OFF,
so an ordinary Tourmaline build does not need OpenUSD.

### Run-time DLL path (USD import only)

`Import.exe` **delay-loads** the OpenUSD runtime: glTF/OBJ import works with
no USD DLLs on `PATH`, and a missing USD runtime fails only the `.usd`
import, not the tool. To actually import a `.usd` stage, the OpenUSD `lib`
directory (plus its `python312.dll` on Windows, and the prebuilt's `bin`
transitive DLLs) must be on the search path when `Import.exe` runs:

```powershell
# Windows: usd_*.dll (lib) + python312.dll (python) + transitive deps (bin)
$env:PATH = "C:\openusd\openusd_25.08\lib;C:\openusd\openusd_25.08\python;C:\openusd\openusd_25.08\bin;$env:PATH"
Import.exe .\path\to\stage.usda
```
```bash
# Linux: the shared libs under <prefix>/lib
export LD_LIBRARY_PATH=/opt/openusd/lib:$LD_LIBRARY_PATH
./out/build/usd-Release/Import ./path/to/stage.usda
```

The imported source extensions are `.usd`, `.usda`, `.usdc`, and `.usdz`;
archives must be resolvable by the installed OpenUSD plugins. Import with the
existing CLI (`Import <path/to/stage.usda>`) or the existing project import
workflow.

## Conversion contract

- USD **stage composition** is evaluated by OpenUSD, including active
  references and selected variant opinions. Layer dependencies inside the
  source tree are recorded for project staging and content fingerprints.
- Xform, Scope, Mesh, and perspective Camera prims become native scene
  hierarchy nodes, geometry, and cameras; mesh vertices, indexed normals,
  and `primvars:st` become Tourmaline geometry attributes.
- A root node converts `metersPerUnit` to meters and Z-up to Y-up.
- Direct, unconnected `UsdPreviewSurface` scalar/color values become
  Tourmaline PBR material properties.
- Native `.geometry` and related assets are created by the **existing**
  `DeriveNativeAssets` pipeline; USD is not embedded into a pak.

## Explicitly unsupported (fail closed)

This is an initial conversion backend, **not support for every USD schema**.
The importer rejects unsupported or ambiguous data instead of producing a
plausible but corrupted native scene. Planar concave polygons and quads are
triangulated by an ear-clipping pass that preserves face-varying normals and UVs;
non-planar or degenerate polygons are rejected. Meshes with materialBind
GeomSubset face assignments are split into material-grouped glTF primitives,
while overlapping and invalid subset assignments fail closed.

For identities across reimports, optionally author
`custom string tourmaline:stableId = "asset-identifier"` on mesh, transform,
camera, and material prims. IDs must be unique within the composed stage and
consist of ASCII letters, digits, `_`, `-`, `.`, or `:` (up to 128 chars).
These become stable logical names (`usd-id:asset-identifier`) in the imported
model. Without an authored ID, the composed USD prim path is used. Siblings
are visited in deterministic order using stable ID, then path, rather than
source-authored child order, reducing numeric node-ID churn.

**Identity scope:** Existing source-family IDs and named material GUIDs are
carried across reimports using the existing `.meta` ledger. Stable prim IDs
improve naming across reorder/rename operations; they do **not** yet provide
a complete per-node UUID mapping into `.tour` overrides. Inserting/removing
nodes or changing hierarchy may still shift numeric cluster node IDs. Before
relying on stable overrides under structural scene edits, implement the
UUID-based resolver at the editor/cooker `.tour` boundary. Renames without
`tourmaline:stableId` also change logical names.

Unimplemented features include:

- Non-planar/degenerate polygon faces, subdivision surfaces, non-materialBind
  GeomSubset behavior, and animated point samples.
- PointInstancers, instance proxies, skinned animation, skeletons, USD
  physics and other non-mesh typed schemas.
- Connected shader graphs, texture/MDL networks, multiple material
  shaders, and materials without a direct `UsdPreviewSurface`.
- External layer dependencies outside the imported source directory.
- Authoring-round-trip/export and Omniverse Nucleus live synchronization.

Before expanding schema support, add conversion fixtures and round-trip
checks for each new type. Avoid enabling one schema merely because it
can be parsed: its *authored meaning* must survive native conversion.

## Follow-ups

1. Robust triangulation and subdivision tessellation with correct
   face-varying attributes and normals.
2. Shader-network conversion: `UsdUVTexture`, material bindings,
   UV sets, colorspaces, and MDL policy.
3. Connect authored `tourmaline:stableId` to persistent `.tour` UUID resolution;
   preserve node edits across hierarchy/cluster index shifts. Improve external
   layer resolver dependency fingerprints.
4. USD skeletal and time-sampled animation into native `.anim` assets.
5. PointInstancer and physics conversion, plus authoring/editor sync.

This module should remain on the import/cook side of the build; never add
OpenUSD dependencies to the renderer or shipped player.
