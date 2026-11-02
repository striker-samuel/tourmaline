# DoubleFloat / high-low world position scaffolding

This PR establishes an **opt-in transport contract** for the planned
persistent GPU scene and CPU-side, view-relative draw conversion. It does
**not** change camera, scene, draw, render-view, culling, or shader behavior.

## Ownership and coordinate spaces

- CPU scene state retains absolute positions in `glm::dvec3` or an
  affine `glm::dmat4` (migration to these types is **not** in this PR).
- A GPU scene may store **absolute** positions as `DoubleFloat3`, two
  float4s (`high` and `low`, 32 bytes, 16-byte aligned); `.w == 0`.
- Each rendering view supplies a high/low origin, also constructed from
  the original double position using `SplitDoubleFloat`.
- Shaders subtract **high from high and low from low**, then add their
  differences to obtain a nearby **view-relative float3**.
- Model bases, local vertices, projected bounds, frustum planes and HZB
  calculations are float32 in that SAME relative space. Never pass a
  relative model to an absolute-world view/projection matrix.
- Existing rendering still uses per-view `GfxDraw.model` (float4x4).
  `RelativeModel` can convert double affine world transforms at draw
  upload time without changing the shader interface.

## API

| CPU: `Engine/Core/double_float.h` | GPU: `engine/shaders/double_float.hlsli` |
|---|---|
| `SplitDoubleFloat(dvec3)` | `DoubleFloat3` layout |
| `JoinDoubleFloat(DoubleFloat3)` | `DoubleFloatRelative(world, origin)` |
| `DoubleFloatRelative(world, origin)` | `DoubleFloatTransformPoint(local, basis, world, origin)` |
| `RelativeFloat(dvec3, dvec3)` | — |
| `RelativeModel(dmat4, dvec3)` | — |
| `SnapRenderOrigin(dvec3, cellMeters=4096)` | — |

**Scope/precision:** These are GPU high/low *position* utilities, not a
general compensated-float arithmetic library. Conversion assumes finite
world coordinates within the finite float32 range. High/low subtraction
is designed for positions *near the view origin*, not arbitrary distant
world-space vector operations. Error grows with very large absolute
magnitudes; test scene budgets against your required spatial tolerance.

## Follow-up GPU scene integration

1. Move authoritative CPU scene/camera positions to double precision.
2. Create persistent GPU instance records holding high/low translation
   and float32 local basis; publish dirty instance updates only.
3. Supply the split view origin and **relative** view matrix. For a
   snapped origin, camera translation is `cameraWorld - originWorld`.
4. Compute high/low relative instance translations **once per instance
   or visible meshlet group**, then reuse in culling, HZB, seed, mesh
   shaders, forward lighting and rasterization.
5. Establish stable visibility/temporal identities, origin-change
   handling, and multi-view/frozen-culling origin consistency.
6. Add shader-compile and GPU parity tests as the GPU scene is wired.

Do not add high/low coordinates to existing `GfxDraw` yet: the existing
culling chain's push constants, CPU precull, frozen/live views, lighting
and temporal seed/remainder logic all require a coordinated migration.

## Validation

`DoubleFloatTests` is a CPU-only CTest target. It covers the mirrored
GPU arithmetic, large positive/negative coordinates (including 1e12),
high/low reconstruction, float-before-subtract failure, double-before-
float affine model rebasing, and negative-coordinate snapping.
