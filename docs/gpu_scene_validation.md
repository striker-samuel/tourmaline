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

## Stable GPU verdict compaction (follow-up to #37)

The validation path now follows the instance-cull dispatch with **three
ordered compute stages**, all using persistent candidate order:

1. `gpu_scene_compact_count.cs.hlsl`: count visible, generation-valid
   verdicts in each 64-candidate group.
2. `gpu_scene_compact_prefix.cs.hlsl`: exclusive prefix over group counts;
   slot `offsets[groupCount]` publishes the total visible count.
3. `gpu_scene_compact_scatter.cs.hlsl`: group-local parallel inclusive scan,
   then scatter visible `uint4` verdicts to unique contiguous positions.
   Their original order is preserved, including across group boundaries.

A verdict is kept only when `instanceSlot != InvalidIndex`,
`generation != 0` and `visible != 0`. Conservative bounds bypass
verdicts remain visible. The output is an ordered list of full
`InstanceCullVerdict` records, not yet submesh commands.

The three shader dispatches run in the **same RenderGraph compute pass**
following the cull kernel. Explicit `EmitGlobalMemoryBarrier` calls
separate cull -> count -> prefix -> scatter: these dependencies are BDA
buffer accesses, not inferred graph resources. Per-flight scratch and
output buffers live in `CandidateValidator` and are read back only
after that flight's acquire fence. Both the output count and every
retained verdict are compared to the CPU stable-compaction oracle.
Missing shaders or allocation failure bypass compaction rather than
affecting production rendering.

The opt-in validator currently caps input to **32,768 candidates**
(512 workgroups). The group-prefix shader deliberately uses a
single-invocation scan: this is suitable for parity development at
that scale, but must be replaced with a hierarchical parallel scan
before scaling to the 1,048,576 candidate maximum. It would be
incorrect to call this production-ready GPU-driven rendering or to
consume its output for draws without authoritative double camera /
scene transforms, residency/LOD expansion, and performance testing.

Run `GpuSceneValidationTests` for headless reference-order coverage,
then enable `TOURMALINE_GPU_SCENE_VALIDATE=1` on a Vulkan device to
verify GPU readback and compiled shader behavior. CI has not been
verified for this change.

## GPU Scene v8: resident LOD and submesh work expansion

The opt-in validation pass extends the stable compacted visible-instance
records with three further compute stages:

- `gpu_scene_work_select.cs.hlsl`: fetch each compacted instance through its
  generation-checked GPU-scene row, validate the referenced shared mesh,
  then select the **finest fully registered resident LOD** (lowest index).
  Check both the shared-mesh residency bit and the individual LOD flags.
  Validate the LOD's contiguous submesh span and count only live submesh
  rows with a registered geometry ID. The shader writes one choice and one
  uint32 count for **every input candidate**, including zero for invisible
  or invalid indices, so stale frame memory cannot leak into the prefix.
- `gpu_scene_work_prefix.cs.hlsl`: compute exclusive per-candidate output
  offsets, with total work count in `offsets[candidateCount]`.
- `gpu_scene_work_scatter.cs.hlsl`: expand each chosen LOD's valid submeshes
  into 32-byte `SceneSubmeshWork` records in **stable instance/submesh
  order**. Each record contains the instance slot + generation, transient
  registered geometry ID, material ID, selected LOD index, persistent
  submesh slot, pipeline group and pass mask.

The count is read from the *GPU-produced compact offsets*, not from a CPU
visibility list; the CPU list exists only as an independent validation
oracle. The pipeline uses explicit compute memory barriers between each
BDA-dependent dispatch.

`CandidateValidator` owns choices, counts, offsets, and expanded-work
buffers **per flight** and reads both total count and all compacted work
records on flight reuse, after its acquire fence. The CPU reference walks
the same generational scene rows and residency rules, including fallback
when the finer LOD is not ready. Tests exercise multiple submeshes,
unregistered geometry, residency change, stale generation and invisible
instances.

### Important constraints

- **Still validation-only**: it never issues indirect draws, and does not
  replace GfxDraw, HZB, seed/remainder, main raster, or shadow rendering.
- The temporary LOD policy deliberately chooses the finest eligible
  resident LOD. This is a correct residency selector but **not yet
  screen-space-error LOD**; the production selector must share the
  renderer's per-view error budget and switch policy.
- The validator remains limited to 32,768 candidates, with at most 16
  submeshes per selected LOD and 524,288 potential work rows. It allocates
  space for this upper bound before any GPU write.
- The per-candidate exclusive scan is serial and intentionally bounded to
  validation usage. Replace both serial scans with scalable hierarchical
  GPU scans before enabling a million candidates.
- Existing World scene/global transforms and RenderView matrices are
  still float32 absolute-world; the DoubleFloat row encoding cannot
  reconstruct precision already lost there. A new authoritative double
  scene/camera path is required before using GPU work for production
  rendering at large world offsets.

For GPU parity run with `TOURMALINE_GPU_SCENE_VALIDATE=1` and inspect
`[GpuSceneWork]` count/record mismatches. Headless unit coverage lives in
`GpuSceneValidationTests`. HLSL compilation and Vulkan-device verification
must pass before enabling a production consumer.

## GPU Scene v9 — double-source contract and indirect work consumption

This milestone establishes **two interfaces** needed before switching the
production renderer away from CPU draw preparation.

### Double-precision source authority

`SceneRegistry::SourceRecord::globalModel` now stores `glm::dmat4` and
`SceneRegistry::SetTransform` splits a **double** world translation into
the existing 32-byte high/low `DoubleFloat3` GPU instance record. The
rotational/scale basis remains float32. `UpdateGlobal` accepts an
authoritative `glm::dmat4` and an explicitly isolated `glm::mat4`
adapter for existing scene journals. At 1e12-unit world origins, 0.125-unit
updates are preserved and covered by CPU-only tests.

`AuthoritativeRelativeFrustum(projection, worldToViewRotation, cameraWorld)`
builds the six cull half-spaces from a **translation-free** double matrix:
`projection * worldToViewRotation`. The double render origin travels
separately and is split into high/low just before GPU upload. This avoids
constructing a huge absolute-world view-projection matrix, which would
otherwise risk cancellation. The origin and relative planes are in exactly
the coordinate frame consumed by `gpu_scene_instance_cull`.

**Integration limit:** World::scene still calculates float32 global
transforms, and CameraPositioner_FirstPerson still owns float32 position.
The new dmat4 path is an authoritative *input contract*, not a claim that
these producers have already migrated. The current GPU parity pass retains
its raster-consistent legacy-view adapter until that coordinated migration
is complete. Do not switch production culling to an unrelated double
camera position.

### GPU-generated indirect compute commands

After stable submesh expansion the GPU can now produce and consume its own
dispatch count, without a CPU readback:

1. `gpu_scene_indirect_args.cs.hlsl` reads
   `workOffsets[candidateCount]` and writes a 12-byte
   `VkDispatchIndirectCommand`-compatible `uint3`, with
   `groupCountX = ceil(workCount / 64)`, y=z=1.
2. A global memory barrier makes the arguments visible to the indirect
   command fetch.
3. `CommandBuffer::DispatchIndirect` launches
   `gpu_scene_indirect_verify.cs.hlsl`, which consumes exactly the
   populated work rows and copies them into a separately owned per-flight
   parity buffer.
4. At the flight-reuse fence, the CPU validator reads back the GPU-produced
   command and every consumed record and compares both against the
   CPU work oracle. Zero output dispatches zero groups; invalid counts
   suppress dispatch rather than exceeding the allocated work buffer.

This is a *real GPU-driven indirect compute dispatch* in the validation
frame. It is **not yet VkDrawIndexedIndirectCommand generation** and does
not submit geometry. The current `geometryId` is a transient meshlet
registration, not an indexed-draw descriptor with index count/offset,
base vertex, material/pipeline layout or per-instance transform. Treating
it as a draw command now would be incorrect.

All new work remains behind `TOURMALINE_GPU_SCENE_VALIDATE=1`, with the
32,768-candidate bound and independent per-flight buffer lifetimes.
Production indexed draws require: double-authoritative hierarchy and
camera producers; consistent camera-relative raster matrices; a GPU-
addressable geometry draw-argument table; scalable parallel prefix
algorithms; shader/pipeline grouping; and Vulkan-device parity/perf tests.

Regression coverage: `GpuSceneValidationTests` for the double view and
indirect arguments; `GpuSceneWorldTests` for preserved double source
transforms. Shader compilation and real GPU readback parity are required
before the indirect stage can become authoritative.
