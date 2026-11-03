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

## Space transforms and shadow projection (small Unity-style subset)

Projects may include `tm_project_shader_macros.hlsli` (which includes
`tm_project_shader_space.hlsli`) in **vertex or fragment shaders**. The
space helpers only depend on standard HLSL math types; no descriptor
layout, scene push block, `GfxDraw`, or renderer-owned shader entry
point is required.

Unlike Unity's implicit `UNITY_MATRIX_MVP`, Tourmaline has separate
RenderViews for the main camera and shadow passes, and its GPU-filtered
draws read the model matrix from the draw ring. **Supply the model and
view-projection matrices explicitly.** This prevents a project shader
from accidentally using the camera VP while rendering a shadow atlas.

| Macro | Result | Inputs |
|---|---|---|
| `TM_OBJECT_TO_WORLD(posOS, model)` | `float3` | Local position + object-to-world |
| `TM_WORLD_TO_VIEW(posWS, view)` | `float3` | World position + world-to-view |
| `TM_WORLD_TO_CLIP(posWS, viewProj)` | `float4` | World position + chosen view-projection |
| `TM_OBJECT_TO_CLIP(posOS, model, viewProj)` | `float4` | Local position + model + chosen VP (MVP convenience) |
| `TM_CLIP_TO_NDC(clip)` | `float3` | Homogeneous clip position with nonzero w |
| `TM_WORLD_TO_SHADOW_CLIP(posWS, shadowVP)` | `float4` | World position + selected cascade/spot VP |
| `TM_SHADOW_CLIP_TO_COORD(shadowClip)` | `float3` | Local shadow `(u, v, depth)`, **not atlas UV** |
| `TM_WORLD_TO_SHADOW_COORD(posWS, shadowVP)` | `float3` | Combined world-to-light and local-coordinate helper |
| `TM_SHADOW_COORD_IN_BOUNDS(uvDepth)` | `bool` | Local normalized xy in [0,1], normal-Z depth in (0,1) |

All matrix transforms use Tourmaline's established
`mul(matrix, float4(position, 1))` convention; no Unity globals or
Unity Y-flip rules are assumed. The shadow coordinate helper matches
`shadows.hlsli::SampleCascadeTile` (`uv = ndc.xy * 0.5 + 0.5`).
Negative/zero homogeneous w returns an out-of-bounds sentinel.
The functions are typed HLSL helpers behind expression-like macros, so
each argument is evaluated once.

### Shadow-pass compile-time macro

`TM_PASS_SHADOW_CASTER` is **1** when a wrapper defines
`TM_SHADOW_CASTER_PASS` before the header, **0** otherwise. Engine
`shadow.vert.hlsl`, `shadow.frag.hlsl`, and both
`gfxshadowseed*.vert.hlsl` variants set that marker. Main prepass
`shadow_rev.frag.hlsl` does **not**: it is a reversed-Z depth pass, not
a light-shadow atlas pass. If your project writes custom caster
wrappers, define the marker in each shader stage that uses it.

**These helpers do not sample shadows or report whether a fragment is
'in shadow'.** Directional cascades, spot atlases, EVSM/PCF, normal bias,
contact shadowing, atlas scale/bias and deferred local-light shadows all
have different sampling contracts. Publishing a single implicit
`TM_SAMPLE_SHADOW` would hide important light/pass state and could
produce incorrect lighting. Shadow attenuation is therefore left in the
engine's lighting body or explicit project lighting functions.

```hlsl
#include "tm_project_shader_macros.hlsli"

float4 ProjectPositionCS(float3 positionOS,
                         float4x4 model, float4x4 selectedViewProj)
{
    return TM_OBJECT_TO_CLIP(positionOS, model, selectedViewProj);
}

float3 ProjectCascadeUvDepth(float3 positionWS, float4x4 cascadeVP)
{
    return TM_WORLD_TO_SHADOW_COORD(positionWS, cascadeVP);
}
```

The `project_space_helpers_probe.vert.hlsl` example in
`samples/04_project_game` compiles every helper through the existing
project shader target without binding a live runtime program.

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
compile-time guards for the mesh, layering, A2C and fixed-lobe macros,
and a compile-only vertex/shadow-space helper probe.

Check each resulting module's VS/MS input linkage and run optional
direct-lightmap / GPU-driven indexed / mesh parity tests on a GPU
before shipping. This PR does not change runtime rendering.
