# GPU Scene v6 — validation-only GPU instance culling

Stacked on PR #37, which is stacked on PR #34 and PR #31.

## Scope

An **opt-in** GPU instance-culling dispatch, with isolated
origin-relative views and frame-fenced GPU/CPU verdict comparison.
Production rendering and its existing float32 absolute-world
RenderViews, GfxDraw, LODs, seed/remainder, shadows and HZB are
UNMODIFIED. No GPU verdict controls raster or draw generation.

Set `TOURMALINE_GPU_SCENE_VALIDATE=1` before starting the renderer.
The validation path samples once every 120 frames, caps its per-frame
candidate set to 32,768, and prints verdict disagreement totals.
Without that environment variable, no validation shader is loaded,
candidate buffers allocated, or GPU validation pass scheduled.

## View contract

`LegacyRelativeFrustum` converts an existing RenderView's absolute
float32 world-space frustum planes to origin-relative *double* planes:

`d_relative = d_absolute + dot(normal, originWorld)`

The selected origin is the current float32 camera position (cast to
double). `InstanceCullGpuView` publishes that same origin as a
DoubleFloat high/low value and the six relative planes as float4s.

This is internally consistent for validating the existing float32
renderer. It **does not** restore large-world precision already lost
in Scene/globalTransform or camera/View matrices. Large-world
production adoption must replace this bridge with
`DoubleRelativeFrustum(doubleVP, doubleOrigin)`, computed from
authoritative CPU doubles. The new extractor supports Vulkan's
zero-to-one depth and a disabled infinite far plane.

## Per-flight resource and lifetime model

`CandidateValidator` owns independent GPU buffers per acquired flight:
- a geometrically grown `PassedInstanceRange` input buffer,
- a fixed 128-byte DoubleFloat origin + planes view buffer,
- a geometrically grown `InstanceCullVerdict` output buffer.

The existing scene database uploader supplies its own per-flight
Mesh/Instance/Leaf/LeafMember addresses. The new compute stage uses
only these pointers. CPU input uploads target Compute ShaderRead via
the RHI's existing upload/barrier machinery.

On the next reuse of the **same flight**, after the acquire fence,
the diagnostic path reads the old verdict output *before* any buffer
is replaced. It compares each uint4 to a CPU reference verdict with
generation-validity and fail-open semantics, logging mismatches. This
uses a blocking debug readback rather than claiming an additional
concurrent ResourceManager async ticket (the latter is already shared
by exposure/streaming readbacks). No CPU readback runs in production.

The output is *not compacted*. It is intentionally excluded from
drawing and LOD decisions.

## Safety

- Any failed GPU scene upload, leaf snapshot, invalid cull view,
  candidate overflow, missing shader or null address SKIPS the
  validation pass. It never turns into missing production geometry.
- GPU instance references must match leaf/member/instance/mesh
  generations; unknown/incomplete bounds must fail open.
- Candidate staging is capped; buffers only grow when needed and
  retire via ResourceManager holders.
- Every new compute pass is feature-gated. GPU readback diagnostics
  are expected to introduce stalls and should not be used as a
  production benchmark.

## Tests

`GpuSceneValidationTests` is a CPU-only test for legacy plane rebasing,
full-double origin-relative view extraction at 1e12 units, invalid
frusta, Vulkan clip-space layout, high/low packing, CPU reference
visibility decisions, incomplete mesh bypass and generational
tombstones.

The GPU shader is compiled through the normal engine/shaders HLSL
glob, but runtime GPU parity requires an actual Vulkan renderer.
CI cannot demonstrate bitwise GPU/CPU agreement on its own.

## Follow-up

Once validation shows correct GPU reference results across moving
camera, world unload/reload, streamed LODs and shadow views, complete
the authoritative double-transform and double-camera migration,
promote this path to multi-view selection, then GPU prefix compaction,
LOD transitions and submesh draw generation.
