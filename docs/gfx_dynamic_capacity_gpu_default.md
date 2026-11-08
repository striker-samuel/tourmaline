# GfxFilter dynamic capacities and GPU-scene default

## Default GPU-scene raster

GPU-scene Main/depth is **enabled by default** for the canonical
source-admitted static opaque subset delivered by #276. The existing
legacy GfxFilter and GPU-skinned pipeline handles all incompatible
objects. `TOURMALINE_GPU_SCENE_RASTER=0` explicitly opts out for
debugging/A-B parity. GPU-scene validation mode remains separate.
No other per-pass admission, shadow, material or motion-vector behavior
is changed in this stage.

## Legacy dynamic-capacity contract

The old CPU candidate fatal was triggered by a hardcoded 262,144
`kMaxBatchSlots` workload and the 65,535 `kMaxDraws` draw limit;
survivors have a separate `kMaxBatches=65536` fixed index-monoblock
budget. **The coordinated cutover above is LANDED**: the runtime rings
are page-planned per (view, flight), the HLSL consumes runtime capacity
fields through the pushes, and the fixed caps' fail-closed successors
are the declared shader-array bounds. The survivor ring stays
separately budgeted at `kMaxBatches` (growing it means growing the
static index monoblock — an independent decision, still open).

## Incremental implementation on PR #280

- The draw-oriented GfxBatchCull, GfxSkinBounds, GfxConeDebug and
  GfxDrawCull dispatches now use a Vulkan-1.3-minimum-safe two-dimensional
  workgroup grid (65,535 groups per X row). The matching shaders flatten
  the Y coordinate into their draw index. Headless planner tests cover
  65,535/65,536 groups, 70,000 groups, maximum and overflow.
- The pure capacity planner now aligns allocated **bytes** to 64 KiB,
  including non-divisor element strides (e.g. 24 bytes), rather than
  merely rounding by floor(64 KiB / stride).
- A separate checked byte-budget planner now describes draw, candidate,
  indirect, temporal, kept-parity, cull-state, skin-bounds, debug-line
  and mesh-batch-list allocation sizes and fails without publishing a
  partial plan if any size overflows. The survivor index ring remains
  independently capped and is not automatically grown.

## The runtime cutover (landed)

The planner is no longer preparation-only — the engine's GfxFilter rings
are RUNTIME-capacity allocations:

- `FinalizeView` plans each (view, flight) through
  `tour::gfx::PlanViewCapacity` (page-rounded, overflow-checked, bounded
  by the DECLARED shader arrays) and GROWS every coupled ring that is too
  small — draw/batch/indirect/kept/cullState, the temporal visDraw +
  seed/remainder indirect rings, the State-level skinBounds table, the
  mesh batch lists and the armed cone-debug line buffer — rebinding their
  rendergraph logicals. Grow-only; the old physical buffers retire on the
  graphics timeline (the Holder release routes through the retire-ordered
  deferred destroy), and a grown kept ring restarts zero-filled (one
  conservative temporal frame, the InvalidateHistory arming).
- The retired fixed caps (`kMaxDraws=65535`, `kMaxBatchSlots=262144`) and
  the compile-time cull-state word indices are GONE. Their fail-closed
  successors are the declared shader-array bounds
  (`tour::gfx::kDeclaredMaxDraws = 1<<20`, `kDeclaredMaxCandidates =
  1<<22`; HLSL mirrors `GFX_MAX_DRAWS` / `GFX_MAX_BATCH_SLOTS`) — a scene
  past them is a loud fatal, never a silent drop. The cullState
  counter/diag words sit at the RUNTIME draw-capacity suffix carried
  through the pushes (`cullStateCounterWord`), and the kept ring's parity
  regions stride by the runtime candidate capacity.
- The shadow draw ring grows the same way at `UploadShadowList` (the
  indirect ring stays survivor-capped, below).
- **THE SKINBOUNDS FLAT-LAYOUT RULE** (the device-lost the knight
  hardware run caught): `GfxSkinBoundsTable` is ONE flat
  `GfxClusterData` array — cluster slots `[0, candidateCapacity)`, then
  the per-draw `drawBound` tail at float4 index
  `candidateCapacity * 4 + drawIdx`. A trailing declared `drawBound[]`
  member would address the tail at the DECLARED bound's offset
  (hundreds of MB past the runtime buffer) — the fixed layout only worked
  because declared == allocated. Every tail access goes through
  `GfxSkinDrawBound` / `GfxSkinSetDrawBound`, and every consumer push
  carries the TABLE's own capacity (`skinBoundsCandidateCapacity`,
  distinct from the view's kept-ring `candidateCapacity`).

## Hardware verification (RTX 5070 Ti, 2026-11-08)

- **Sponza (static, LODs, shadows)**: GPU-scene default vs
  `TOURMALINE_GPU_SCENE_RASTER=0` legacy — **0.00% differing pixels**.
- **SponzaGrid32 (1024 instances)**: the default camera pushes 25,411
  draws and **1,041,498 candidate slots — 4x the retired 262,144 cap**
  (a fatal on master) — and renders cleanly through the grown rings;
  the cull-baseline JSON readback (runtime-sized) lands.
- **Knight (skeleton + attached walk clip, animated)**: GPU-scene
  default vs legacy — **0.00% differing pixels**, 96 frames, the full
  skinning -> GfxSkinBounds -> batch/draw cull chain on the dynamic
  table (fresh-cooked project; the pre-SMP8 knight.pak is stale).
- **Sponza x10 (GI + shadows)**: clean 96-frame run with the shadow
  ring path (its ~2.9k shadow draws stay under the first-page capacity,
  so the shadow grow leg itself is exercised through the shared
  RebindBuffer/PlanViewCapacity machinery the main rings proved at 1M
  candidates).
- SponzaGrid32 WITH shadows still trips the PRE-EXISTING survivor-cap
  fatal (`kMaxBatches=65536` shadow commands — identical on master);
  the separately-budgeted survivor strategy below is the sanctioned
  follow-up, unchanged by this PR.
- Full ctest suite (116/116, knight_anim designed-skip) + the text
  integrity suite green.

## Later renderer work

These are independent of dynamic GfxFilter capacity and must be
reviewed separately:

- **Layered materials on GPU-scene raster.** Publish material layers
  and all required FS/Forward+/G-buffer variants with screenshot
  parity; until then layered objects use the legacy material pipeline.
- **Remove the per-flight owner-generation mask.** Replace it with
  canonical membership in GPU-scene instance rows / generation-stable
  compaction or source-slot journals. Maintain exactly-once Main/depth
  submissions, O(dirty) updates and fail-closed source retirement.
  Do not simply remove the shader mask test while the instance-cull
  candidate stream includes legacy-owned objects.

## Verification

The default change required real GPU static-only and mixed skinned,
masked, lightmapped and LOD screenshot parity, plus Vulkan validation
(see "Hardware verification" above — static, instanced/LOD and skinned
are green; layered materials stay on the legacy pipeline by the later-
work gate below). The dynamic-capacity helper has headless boundary
tests; the runtime cutover is hardware-verified through the runs above.
The shadow SURVIVOR ring (kMaxBatches) remains the separately-budgeted
open item.
