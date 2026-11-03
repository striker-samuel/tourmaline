# OpenUSD source import (optional)

The USD importer is an **offline authoring adapter**, not a Tourmaline renderer
or a USD runtime backend. A composed OpenUSD stage is translated into an
in-memory `tinygltf::Model`, then passed to Tourmaline's existing
`ImportMeshSource -> DeriveGltfAssets` pipeline. It produces native
`.geometry`, `.material`, and `<source>.meta` assets, which continue
through the ordinary cook/pak/project pipeline. Vulkan, scene tables, and the
runtime do not link to OpenUSD.

## Build

Install OpenUSD with a CMake `pxr` package exposing `usd`, `usdGeom`,
and `usdShade` CMake targets (or their `pxr::` aliases), then configure
with:

```sh
cmake -S . -B out/build/usd-Release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_WITH_USD=ON \
  -DCMAKE_PREFIX_PATH=/path/to/openusd/install
cmake --build out/build/usd-Release --target Import SourceImportTests
ctest --test-dir out/build/usd-Release -R source_import_tests --output-on-failure
```

Normal prerequisites for building Tourmaline still apply. USD defaults OFF,
so an ordinary Tourmaline build does not need OpenUSD. The imported source
extensions are `.usd`, `.usda`, `.usdc`, and `.usdz`; archives must be
resolvable by the installed OpenUSD plugins. Import with the existing CLI
(`Import <path/to/stage.usda>`) or the existing project import workflow.

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
  `DeriveGltfAssets` pipeline; USD is not embedded into a pak.

## Explicitly unsupported (fail closed)

This is an initial conversion backend, **not support for every USD schema**.
The importer rejects unsupported or ambiguous data instead of producing a
plausible but corrupted native scene. Unimplemented features include:

- Polygon faces with more than three corners, subdivision surfaces,
  GeomSubset per-face material bindings, and animated point samples.
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
3. Per-prim stable identity across reorders/renames and richer USD
   layer dependency fingerprinting, including external resolvers.
4. USD skeletal and time-sampled animation into native `.anim` assets.
5. PointInstancer and physics conversion, plus authoring/editor sync.

This module should remain on the import/cook side of the build; never add
OpenUSD dependencies to the renderer or shipped player.
