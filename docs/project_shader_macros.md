# Public macros for project-authored lit shaders

This is the small, stable compile-time interface for project shader
wrappers. Include `lit_dependencies.hlsli` before project hook libraries
as usual; it now includes `tm_project_shader_macros.hlsli`. Projects
may also include that header directly. All names are prefixed `TM_`.

## Core macros

| Macro | Value / usage | Contract |
|---|---|---|
| `TM_LIT_MATERIAL_ID(IN)` | `uint` expression | Use in `mainPS` only: mesh FS gets `IN.meshMaterial`; direct or GPU-filtered indexed FS gets `pc.materialId`. Hook implementations should use `TMLitInput.materialId` instead. |
| `TM_LIT_MESH_FRAGMENT` | 0 or 1 | **Compiled fragment path**, not whether a particular vertex was GPU-cull filtered. 1 only for `GFXMESH_FS_PASS`. |
| `TM_LIT_ALPHA_TO_COVERAGE` | 0 or 1 | 1 when the wrapper enables `ALPHA_TO_COVERAGE`. |
| `TM_LIT_LAYERING_SUPPORTED` | 0 or 1 | 0 for `MAT_STANDARD`, whose shader omits layered-material machinery; 1 means compiled in, *not* that the current draw is layered. |
| `TM_LM_NONE` | `0xFFFFFFFFu` | Absent baked-lightmap lobe-count sentinel. Not an offset/delta sentinel. |
| `TM_LM_VALID_LOBE_COUNT(n)` | `bool` | Valid only for cooked SG counts 5, 6, 9 or 12. Argument evaluated once. |
| `TM_LM_COMPILED_LOBES` | 0, 5, 6, 9 or 12 | 0 = generic **runtime-lobe** shader, not 'no lightmap'; nonzero = fixed SG count from `SG_LM_LOBE_COUNT`. |

Only use the 0/1 and fixed-count macros in preprocessor expressions.
`TM_LM_VALID_LOBE_COUNT` is an HLSL expression/function, so use it at
shader runtime rather than in `#if`.

Build flags must be set **before** including `lit_dependencies.hlsli`:
`GFXMESH_FS_PASS`, `ALPHA_TO_COVERAGE`, `MAT_STANDARD` and
`SG_LM_LOBE_COUNT`. The project sample's three lit wrappers show the
intended include order. The fragment body, PSO matrix and cooked-asset
formats are unchanged.

## Expanded project hook input

`lit_hooks_api.hlsli` adds these readonly per-fragment members at
the *end* of `TMLitInput` (existing fields and functions stay intact):

| Field | Type | Value |
|---|---|---|
| `materialId` | `uint` | The selected material ID on all direct, filtered indexed and mesh FS paths. |
| `lightmapAtlasUv` | `float2` | Atlas-space UV already computed by VS/MS for a baked static mesh. |
| `lightmapLobes` | `uint` | The published cooked lobe count, or `TM_LM_NONE`. |

Both `TM_ModifySurface` and `TM_ModifyLitColor` receive these values.
No new GPU buffers, descriptor bindings, varying locations, SPIR-V push
fields or material IDs are introduced. Passing extra values into hook
functions may extend their source-level input struct; project hooks
should be compiled against the matching engine shader headers.

`lightmapLobes != TM_LM_NONE` does **not** mean atlas illumination was
successfully sampled: `GI_NONE`, `frame.lmEnabled`, atlas residency,
and material handling still control actual lighting. Avoid treating
unlightmapped `lightmapAtlasUv` (typically zero) as valid chart data.
Baked static lightmaps are never authored for skinned geometry.

### Example project hook

```hlsl
#include "lit_hooks_api.hlsli"
#include "tm_project_shader_macros.hlsli"

void TM_ModifySurface(inout TMSurface surface, TMLitInput input)
{
    const bool charted = TM_LM_VALID_LOBE_COUNT(input.lightmapLobes);
    // Projects may branch on charted or input.materialId if needed.
    // For no-op effects, don't insert an unnecessary runtime branch.
}
```

For projects that require no distinction between paths, continue to
use the ordinary typed hooks, without macros.

## Deliberately private renderer internals

No public `TM_GPU_DRIVEN`: direct and GPU-filtered indexed stages share
fragment programs, so the distinction is not a fragment compile-time
property. Also private: `GfxDraw.misc*`, chart-heap additive deltas,
`SV_VertexID` addressing, bindless binding numbers, raw GPU pointers,
meshlet IDs, and full position/shading pulling.

These are implementation details and may change without a project
shader source migration.

## Compile/validation matrix

Build the engine and `GameProjectLit_shaders` sample target for SPIR-V,
including `project_lit.frag.hlsl`, `project_lit_a2c.frag.hlsl`,
and `gfxmesh_project_lit.frag.hlsl`, plus their
`MAT_STANDARD`/`SG_LM_LOBE_COUNT`/`GI_*` variants. The sample has
compile-time guards for the mesh, layering, and fixed-lobe macros.

Check each resulting module's VS/MS input linkage and run optional
direct-lightmap / GPU-driven indexed / mesh parity tests on a GPU
before shipping. This PR does not change runtime rendering.
