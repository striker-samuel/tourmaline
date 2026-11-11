// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GeometryFX-style per-view triangle filter (Vulkan port), procedural API.
//
// Produces, per view, a compacted index list + VkDrawIndexedIndirectCommand
// array from static geometry. Direct port of AMD GeometryFX filtering:
//   reference/GeometryFX/amd_geometryfx/src/AMD_GeometryFX_Filtering.cpp
//   reference/GeometryFX/amd_geometryfx/src/Shaders/AMD_GeometryFX_Filtering.hlsl
//
// Filing system (deliberately NOT objects, cf. staticVertexFmtHeap):
//   - `State` is plain data (single instance owned by the app renderer).
//   - Free functions in `smvk::gfx` operate on it (Ensure/Destroy/Register/
//     BeginFrame/BeginView/PushDraw/FinalizeView/...).
//
// Units are CLUSTERS (meshlets, <= kGfxMaxClusterTris triangles), not whole
// meshes. ONE cull regime owns this state:
//
//   GPU cull chain (Northlight style, the only path): the CPU seeds state
//   only — the material/bucket-sorted draw list (with each draw's
//   candidate-slot base riding Draw::candidateBase) and the (bucket,
//   material) range layout. Then,
//   with ZERO mid-pipeline CPU readbacks:
//     GfxBatchCull  — one workgroup per draw: mesh-bound fast discard
//                     (frustum, LIVE view) -> per-cluster sphere +
//                     world-space normal-cone -> kept bits (parity ring;
//                     the PRE-OCCLUSION geometric verdict — the filter
//                     refines them to final visibility later in the
//                     frame) + temporal seed/remainder commands
//                     (prev-kept region, GPU-resident: seed = kept &&
//                     prevKept, remainder = kept && !prevKept, where
//                     prevKept is FINAL visibility two frames back —
//                     slot-stable identity, both drift directions
//                     conservative) + the batch array + the filter's
//                     indirect statics
//     GfxSeedDepth  — prev-kept intersection into depth (T3)
//     GfxHiz        — the MIN-reduced pyramid (T2)
//     GfxDrawCull   — one thread per draw: mesh-bound frustum (FROZEN
//                     view) + HZB draw-bound occlusion verdicts
//     GfxFilter     — per-triangle tests + per-cluster HZB occlusion
//                     (T4); surviving groups allocate filtered-index
//                     ranges through an atomic counter (clamped at the
//                     ring capacity); verdict-dropped slots exit before
//                     any projection work; the final-visibility
//                     refinement of each slot's kept-bit history word
//                     (the temporal split's input) happens here — after
//                     every occlusion decision has landed
//     GfxRemainderDepth — the disocclusion leg (T5)
//   Main          — the mesh-shader leg (its indexCount==0 gate keeps
//                     exact mesh/indexed parity) + indexed ranges
//
// Geometry layout ("rearranged heap", load-time): cluster-ordered global
// vertex ids are baked once into an owned index buffer (positions stay in
// the shared static heap, so the direct path is untouched).

#include <Engine/Memory/pool.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/gpu_scene_static.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/Renderer/visibility_hiz.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// StaticMeshUpload / CookedMeshlets forward decls and the Cluster/Mesh/
// ClusterConstants + StaticGeometry registration surface now live in
// gpu_scene_static.h (included above); this header keeps the render-side
// types (flags, draws, batches, views, the temporal/Hi-Z sidecar, State)
// that the GPU-scene backends and GfxRenderer still consume.

// (namespace/RHI boundary audit, todo 8.4) the GPU cull/filter CONFIG
// + policy layer is ENGINE-OWNED (renderer-side culling decisions over
// RHI resources) and lives in tour::gfx; the RHI types it drives
// (Holder/BufferHandle/ShaderProgram/RenderGraph/limits) stay smvk::
// and are spelled FULLY QUALIFIED here — no broad using-directive: a
// public one is R6-gated, and its transitive lookup collides this
// namespace's own Cluster with tour::Cluster for every
// using-namespace-gfx consumer (the ambiguity that decided this shape).
// The smvk::gfx namespace ALIAS at the bottom keeps every
// smvk::gfx::* spelling resolving.
namespace tour {
namespace gfx {

// Must match gfxfilter.hlsli GFX_CULL_*.
typedef enum CullFlagBits {
    CullFlagBit_Degenerate = 0x1,
    CullFlagBit_Backface = 0x2,
    CullFlagBit_Frustum = 0x8,
    CullFlagBit_SmallPrim = 0x20,
    // HZB occlusion (T4): per-cluster sphere test against the MIN-reduced
    // depth pyramid. Facing-agnostic: applies to every draw alike.
    CullFlagBit_HzbOcclusion = 0x40,
    // Exclude MASK (alpha-tested) draws from the HZB occlusion test.
    // The MIN-reduced pyramid stores the NEAREST sub-texel per coarse
    // texel, so a single leaf texel of a cutout card poisons the whole
    // texel "near" — dense masked foliage (its own worst occluder) then
    // culls the clusters that are still visible through its cutout holes
    // (sky-through flicker), and masked clusters are the smallest, so
    // their bound spheres land on the coarsest mips where the poison has
    // the most reach. With this flag masked draws can only be culled by
    // the geometric tests (frustum/backface/degenerate/small-prim).
    CullFlagBit_HzbSkipMasked = 0x200,
    // Contribution (T4): reject clusters whose screen footprint is below
    // TemporalView::minimumContribution px^2 (0 = disabled).
    CullFlagBit_Contribution = 0x80,
    // Selects the opposite homogeneous-winding sign (engine Y-flip).
    // Verify once: closed sphere must cull ~50% via backface alone.
    CullFlagBit_FlipWinding = 0x100,
} CullFlagBits;
typedef uint32_t CullFlags;

// Per-draw flags (stored in Draw::flags / HLSL GfxDraw misc.y).
// Scene::ditherCloseup fade band this frame (DitherCloseupStateFor ==
// Fade at PushDraw time; see DrawSetBit_Masked - Fade draws also route
// into the masked bucket so the alpha-tested depth/shadow legs can
// screen-door with them). HLSL mirror: GFX_DRAW_DITHER_CLOSEUP
// (gfxfilter.hlsli), read by gfxfiltered.vert / gfxfilter.mesh and
// forwarded flat (VS location 8) to the lit FS family.
inline constexpr uint32_t DrawFlagBit_DitherCloseup = 0x2;
// The draw's mesh is SKINNED (the Northlight compute path): the
// mesh-shader leg fetches the deformed pos/normal/tangent from the
// per-frame DEFORMED buffer (GfxMeshParams.deformed, indexed
// vi - deformedBase) instead of the bind-pose heaps. Set at
// PushDraw time ONLY when the frame carries a valid deformed buffer
// (the renderer's skinning driver - a skinned mesh without animation
// data keeps the static bind-pose fetches). HLSL mirror:
// GFX_DRAW_SKINNED (gfxfilter.hlsli misc.y bit 2).
inline constexpr uint32_t DrawFlagBit_Skinned = 0x4;
// The draw's material is DOUBLE-SIDED (Material::doubleSided): the
// rasterizer draws both faces (CullMode::None via
// MakePipelineStateForMaterial) and the GPU filter's per-triangle
// backface test spares these draws — else the filtered/mesh lists
// would drop exactly the faces the rasterizer now draws (thin shells
// see-through at open edges). HLSL mirror: GFX_DRAW_DOUBLESIDED
// (gfxfilter.hlsli misc.y bit 3).
inline constexpr uint32_t DrawFlagBit_DoubleSided = 0x8;

typedef uint32_t DrawFlags;
// Draw-set buckets: geometry grouped by properties that affect
// rasterization (Alan Wake 2 style). The seed depth pass (T3) issues one
// PSO per bucket (opaque vs. alpha-tested);
// views accept a mask of buckets (main view: all, shadows later).
inline constexpr uint32_t DrawSetBit_Opaque = 0x1;
inline constexpr uint32_t DrawSetBit_Masked = 0x2;
inline constexpr uint32_t DrawSetMask_All = DrawSetBit_Opaque | DrawSetBit_Masked;
typedef uint32_t DrawSetMask;

// One GPU workgroup per cluster (128 threads, 1 thread per triangle).
static constexpr uint32_t kThreadsPerCluster = 128;
// kMaxClusterTris and kMaxMeshes moved to gpu_scene_static.h.
// Bistro-scale scenes need headroom here: ~2.9k draws / ~49k meshlet
// clusters (Sponza fits the old 1k/4k). Cost is one filtered-index ring
// slice per flight (kMaxBatches * kMaxClusterTris * 3 uints = 96MB at 64k)
// inside the 512MB static index monoblock — keep monoblock sizing in sync
// (see AllocateStaticVertexFmt in engine/src/Renderer/mesh.cpp).
//
// THE SURVIVOR RING STAYS SEPARATELY BUDGETED (the dynamic-capacity
// cutover's contract, docs/gfx_dynamic_capacity_gpu_default.md): draws and
// candidate slots are runtime page-planned capacities (tour::gfx::
// PlanViewCapacity; the declared fail-closed ceilings live in
// gfx_dynamic_capacity.h), but the filtered-index survivor ring keeps this
// compile-time cap — growing it means growing the static index monoblock,
// an independent budget decision. The filter's atomic allocation clamps
// survivors here (the GPU twin of the old CPU cap + warning).
static constexpr uint32_t kMaxBatches = 65536; // surviving (draw x cluster) per view
// Sized for LOD chains: every cooked sibling registers its own gfx mesh
// (~3x the LOD0-only count), so headroom over the pre-LOD 4k budget.
// (kMaxMeshes moved to gpu_scene_static.h.)
static constexpr uint32_t kMaxViews = 64; // mirrors kMaxRenderViews
// (The retired fixed draw/candidate caps — kMaxDraws=65535 /
// kMaxBatchSlots=262144 — and the compile-time cull-state word indices
// died with the dynamic-capacity cutover: per-(view, flight) draw and
// candidate capacities are runtime state (View::drawCapacity /
// View::candidateCapacity, page-planned grow-only by FinalizeView), and
// the cullState counter/diag words sit at the RUNTIME suffix
// (View::drawCapacity + 0..4) carried to the shaders through the push
// (FilterPush::cullStateCounterWord)
// — never a compile-time macro. The fail-closed ceilings survive as
// tour::gfx::kDeclaredMaxDraws / kDeclaredMaxCandidates.)

// GPU cluster entry (4x uvec4) moved to gpu_scene_static.h as
// tour::gfx::ClusterConstants.

// Chart atlas transform: xy = scale, zw = offset, so atlasUv =
// chartUv * xy + zw. One entry per (mesh chart), concatenated scene-wide
// sorted by mesh then chart; each draw's lmChartOffset is its mesh's base.
struct LmChartXform {
    glm::vec4 scaleOffset{0.0f}; // xy scale, zw offset
};
static_assert(sizeof(LmChartXform) == 16);

// Per-draw frame entry. Must match HLSL GfxDraw (112 bytes).
struct alignas(16) Draw {
    glm::mat4 model{};
    uint32_t mesh = 0;
    uint32_t flags = 0;                   // DrawFlags (bit 2 = skinned, bit 1 = dither-closeup)
    uint32_t drawSet = DrawSetBit_Opaque; // DrawSetBits bucket (seed PSO select)
    // Lightmap chunk (HLSL GfxDraw.misc.w; 0xFFFFFFFF = none). Cooked per
    // mesh with --lightmap; the filtered VS passes it flat to the lit
    // fragment (static/dynamic GI separation).
    uint32_t lmChunk = 0xFFFFFFFFu;
    // Base offset into the scene-wide chart transform buffer for this
    // draw's mesh (HLSL GfxDraw.misc2.x). The filtered VS resolves the
    // atlas UV as chartUv * xform[lmChartOffset + chartId].xy/zw.
    // 0 when the mesh carries no lightmap.
    uint32_t lmChartOffset = 0;
    // Material id (HLSL GfxDraw.misc2.y). The designated publisher group
    // resolves the draw's 5 texture slots through the materials buffer
    // with it (PublishDrawDemand); no other stage reads it.
    uint32_t material = 0xFFFFFFFFu;
    // HLSL misc2.z: GPU-cull candidate slot base — this draw's first
    // (draw x cluster) candidate slot (the CPU candidate build's prefix);
    // 0xFFFFFFFF = the build dropped the draw (cap). 0 in the CPU-cull
    // legacy path (which never reads it).
    uint32_t candidateBase = 0xFFFFFFFFu;
    // HLSL misc2.w: the ADDITIVE lightmap-heap delta (lightmapBase -
    // meshVertexBase, both vertex units — the lightmap stream packs
    // INDEPENDENTLY of the pos/shading windows). The mesh-shader leg
    // fetches the chart words at (vi + lmDelta) * 2; 0xFFFFFFFF = the
    // mesh carries no lightmap stream. Unused by the indexed-raster legs
    // (the classic filtered VS binds the lightmapped layout per mesh
    // when lightmaps return).
    uint32_t lmDelta = 0xFFFFFFFFu;
    // HLSL misc3.x: the RESOLVED per-draw material userdata (Phase A of
    // docs/shader_library_design.md): the material instance's override
    // or the base material's default, resolved by the CPU draw producer
    // at frame build — one flat uint reaching the lit fragment (VS
    // location 9) for the TM_ModifySurface/TM_ModifyLitColor hooks.
    // Bit layout is gameplay-owned; the engine never interprets it.
    // Color-only by v1 contract: the depth/shadow/seed legs copy the
    // word for ring consistency but never read it (no coverage hooks).
    uint32_t userData = 0;
    // misc3.y: per-instance direct-light receiving channels.
    uint32_t lightLayerMask = 0xffffffffu;
    uint32_t userDataPad[2] = {};
};
static_assert(sizeof(Draw) == 112);

// GPU batch entry (1x uvec4): (cluster, draw, outputOffset, flags).
struct Batch {
    uint32_t cluster = 0;
    uint32_t draw = 0;
    uint32_t outputOffset = 0; // in uints; batchSlot * kMaxClusterTris * 3
    uint32_t flags = 0;        // BatchFlags (HLSL batch.w: GFX_BATCH_*)
};
static_assert(sizeof(Batch) == 16);

// Batch flags (stored in Batch::flags / HLSL batch.w).
// DemandPublisher: this batch's workgroup is the designated publisher
// for its draw's texture demand — the first batch emitted for the draw
// in this view's material-sorted expansion (BuildBatches sets it). Every
// group of a draw shares the draw's material, so one group publishes per
// draw instead of one publication per surviving cluster.
inline constexpr uint32_t BatchFlagBit_DemandPublisher = 0x1;
typedef uint32_t BatchFlags;

// Per-bindless-slot texture meta for the draw-level demand publisher
// (HLSL GfxTextureMeta mirror, 8 bytes). longSide feeds the coverage
// formula (WantedMipForScreenCoverage mirror); fullMips == 0 marks slots
// without a streamed texture (the publisher skips them — RequestMip has
// no entry to move). Built by TextureStreamer::BuildSlotMeta, uploaded
// through UploadTextureMeta when the streamer's meta epoch bumps
// (register/unregister only — slots are stable across retargets).
struct TextureMeta {
    uint32_t longSide = 0; // max(baseWidth, baseHeight) texels
    uint32_t fullMips = 0; // 0 = slot holds no streamed texture
};
static_assert(sizeof(TextureMeta) == 8);

// Demand sentinel: one uint per bindless slot in the demand ring;
// 0xFFFFFFFF = "no draw reported this texture this frame" (the consumer
// skips it; the min-publication starts from this fill every frame).
inline constexpr uint32_t kDemandNoReport = 0xFFFFFFFFu;

// GfxSkinBounds push (runs between the skinning pass and GfxBatchCull).
// Must match HLSL GfxSkinBoundsPush in gfxfilter.hlsli (64 bytes). One
// 128-thread workgroup per draw; threads stride the draw's clusters and
// rebuild each SKINNED cluster's object-space sphere + normal cone from the
// deformed heap, then fold the per-draw deformed union bound. Both output
// regions share the one skinBounds BDA (the table's cluster slots + its
// drawBound tail).
struct SkinBoundsPush {
    uint64_t draws = 0;      // this frame (misc2.z = slot base)
    uint64_t meshes = 0;     // static per-mesh cluster ranges
    uint64_t clusters = 0;   // static (bind-pose a/b/c/d)
    uint64_t indices = 0;    // cluster-ordered global ids (clusterIndexBase baked in)
    uint64_t deformed = 0;   // this frame's deformed heap
    uint64_t skinBounds = 0; // out (candidate slots + drawBound tail)
    uint32_t deformedBase = UINT32_MAX;
    uint32_t drawCount = 0;
    // 0 = degenerate keep-cones (TOURMALINE_SKIN_CONE=0, the A/B lever that
    // isolates the cone rebuild from the sphere rebuild in one headless
    // run) — the pre-rebuild behavior.
    uint32_t coneEnabled = 1;
    // The skinBounds table's candidate-slot capacity (this flight's
    // allocation): the belt-and-braces bound for every candidate-slot
    // write, successor of the retired GFX_MAX_BATCH_SLOTS macro.
    uint32_t candidateCapacity = 0;
};
static_assert(sizeof(SkinBoundsPush) == 64);

// GfxConeDebug push (the cone-axis overlay's GPU leg; runs after
// GfxSkinBounds, before Main's overlay draw). Must match HLSL
// GfxConeDebugPush in gfxfilter.hlsli (56 bytes). One 128-thread workgroup
// per draw; threads stride the draw's clusters and write two packed
// DebugLineVertex float3s per candidate slot (apex -> tip), the ACTUAL
// cull cones: baked bind-pose for static draws, this frame's deformed
// cones (the skinBounds table) for skinned draws — zero latency,
// GPU-driven end to end (the retired CPU readback path corrupted the host
// heap: its download destinations dangled through vector reallocation
// between submit and poll, and the cache never landed).
struct ConeDebugPush {
    uint64_t draws = 0;           // this frame (misc2.z = slot base)
    uint64_t meshes = 0;          // static per-mesh cluster ranges
    uint64_t clusters = 0;        // static (baked bind-pose cones)
    uint64_t skinBounds = 0;      // deformed (this frame's table)
    uint64_t lines = 0;           // out (2 packed verts per candidate slot)
    uint32_t skinBoundsValid = 0; // 0: every draw reads the static table
    uint32_t drawCount = 0;
    // The shared line buffer's candidate-slot capacity (successor of the
    // retired GFX_MAX_BATCH_SLOTS macro): the belt-and-braces bound for
    // every candidate-slot write.
    uint32_t candidateCapacity = 0;
    uint32_t pad2 = 0;
};
static_assert(sizeof(ConeDebugPush) == 56);

// Filtered-path VS tail push (gfxfiltered.vert.hlsl + the shared seed
// prologue, 40 bytes). Pushed at kFilteredVsPushOffset (the main pass'
// unused prevModel scratch, bytes 192..232) so the full 256B scene push
// stays intact for the fragment shader. The VS resolves viewProj through
// the render-view slab; the per-draw lightmap chunk rides GfxDraw.misc.w,
// and the chart atlas transforms ride the lmXforms buffer (indexed by
// GfxDraw.misc2.x + the vertex chart id).
static constexpr uint32_t kFilteredVsPushOffset = 192;
struct FilteredVsPush {
    uint64_t draws = 0;
    uint64_t renderViews = 0;
    uint32_t renderViewIndex = 0;
    // paramsValid (byte 212): 1 = the params BDA below is live (the
    // filler sets it with the BDA; the HLSL gates the skinned fetch on
    // it - a plain "params != 0" compare is illegal on a
    // vk::BufferPointer, so the flag word carries the null check).
    uint32_t paramsValid = 0;
    // BDA of the chart transform buffer (0 = none). Trailing so the
    // 24B prefix stays byte-identical to the motion VS tail
    // (FilteredMotionPush reads draws/renderViews/renderViewIndex from
    // 192..216 and its own FrameUniforms BDA at 216, which simply
    // overwrites this field the motion VS never declares).
    uint64_t lmXforms = 0;
    // VS-leg SKIN input, carried DIRECTLY (bytes 224..240 - the free
    // scratch; the mesh dispatch's MeshTailPush re-pushes 224..240 with
    // its own paramsAddress/list fields, harmless because the mesh legs
    // never execute a VS). The ONE pointer the VS legs' SKINNED fetch
    // swap needs beyond their fixed-function inputs (the Northlight
    // contract: one storage fetch swap in the draw shader, NO per-vertex
    // skin math): the frame's deformed heap (skinned draws fetch pos +
    // the deformed normal/tangent words at (VertexIndex - deformedBase)
    // * 5; the static fetch stays the fixed-function vertex-input path,
    // so the bind-pose heaps ride no pointer at all). Deliberately NOT a
    // GfxMeshParamsData pointer and not the pos/shading heap pointers:
    // observed wedges (the NVIDIA pipeline compiler never returns from
    // the vertex-stage creation) for BOTH a TEN-pointer chain and three
    // flat 256-512MB-array pointers; the ORIGINAL tail's one
    // 393KB-array pointer (draws) compiles instantly, so ONE modest
    // pointer is the proven-safe shape.
    uint64_t vsDeformed = 0;
    uint32_t vsDeformedBase = 0xFFFFFFFFu;
    uint32_t vsPad = 0;
};
static_assert(sizeof(FilteredVsPush) == 48);

// ---- Task-less mesh path (gfxfilter.mesh.hlsl + gfxmesh_lit FS twins) ----
// One mesh workgroup per cluster HALF of a CPU-partitioned batch list
// (DrawMeshTasks(2 * listCount) per FS variant). Batches are already CPU
// pre-culled (BuildBatches); the mesh shader runs the per-triangle
// CullTriangle tests and compacts via wave prefix sums. Eligibility lives
// on the CPU (RecordMeshMain partitions eligible batches per variant —
// opaque, variant twin exists; the indexed loop skips
// exactly those ranges). Masked/motion/seed/shadow stay indexed.
inline constexpr const char* kGfxMeshMsPath = "content/shaders/gfxfilter.mesh.hlsl.spv";
inline constexpr const char* kGfxMeshFsPath = "content/shaders/gfxmesh_lit.frag.hlsl.spv";

// THE PROJECT-LIT FAMILY (docs/shader_library_design.md Phase B): the
// project-authored lit FS namespace — the project's
// lit_hooks_project.hlsli compiled against the SAME shared lit body
// (see samples/04_project_game/project/content/shaders). These are
// NOT engine build outputs (never in engine-shaders.pak): a project
// compiles them through compile_shaders + tourmaline_project_lit_variants
// and serves them through the shader library's dev tree or a project
// shader pak. Best-effort at load: absent modules leave every material
// on StandardLit (the fail-safe stock pick, reported loudly at the
// WorldRenderer's family consult).
inline constexpr const char* kProjectLitFsPath = "content/shaders/project_lit.frag.hlsl.spv";
inline constexpr const char* kProjectLitA2CFsPath = "content/shaders/project_lit_a2c.frag.hlsl.spv";
inline constexpr const char* kProjectMeshFsPath =
    "content/shaders/gfxmesh_project_lit.frag.hlsl.spv";

// Mesh params blob. Must match GfxMeshParamsData in gfxmesh.hlsli
// (two matrices first for 16-alignment, then 10 BDAs, 4 uints, the
// deformed BDA + 2 uints = 240B).
// batchList points at this dispatch's slice of the compacted eligible
// batch indices (base folded into the address); listCount bounds it.
// TWO matrices (the culling-freeze contract): viewProj = the gfx view
// record's matrix (frozen while the debug freeze holds the filter — the
// CULL matrix), outViewProj = the live main-view matrix the rasterizer
// projects through (the indexed VS resolves the same through the live
// render-view slab; equal to viewProj when not frozen). indirect is this
// flight's indirect command array — the mesh gates fully-dropped clusters
// on the filter's own indexCount == 0 decision (exact mesh/indexed parity).
struct GfxMeshParams {
    glm::mat4 viewProj{};
    glm::mat4 outViewProj{};
    uint64_t draws = 0;
    uint64_t clusters = 0;
    uint64_t batches = 0;
    uint64_t positions = 0;
    uint64_t shading = 0;
    // The OPTIONAL lightmap stream (PackedVertexLightmap): fetched via the
    // per-draw additive delta (GfxDraw.misc2.w = lightmapBase -
    // meshVertexBase); 0 when no scene mesh carries chart data at all.
    uint64_t lightmap = 0;
    uint64_t indices = 0;
    uint64_t lmXforms = 0;
    uint64_t batchList = 0;
    // This flight's indirect commands (the filter's per-batch decisions —
    // the mesh gates fully-dropped clusters on indexCount == 0).
    uint64_t indirect = 0;
    uint32_t cullFlags = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t listCount = 0;
    // The per-frame DEFORMED buffer (the Northlight compute-skinning
    // output): skinned draws (GFX_DRAW_SKINNED) fetch pos/normal/tangent
    // words from it (indexed vi - deformedBase), everything else from the
    // bind-pose heaps. 0 = no skinned cluster this frame (static fetches).
    uint64_t deformed = 0;
    uint32_t deformedBase = 0xFFFFFFFFu; // the skinned span's heap-id base
    uint32_t pad0 = 0;
};
static_assert(sizeof(GfxMeshParams) == 240);

// Mesh tail push (gfxfilter.mesh.hlsl GfxMeshPush tail, 16 bytes at 224:
// deliberately OVERLAYS the FilteredVsPush tail's VS-skin fields — the mesh
// legs never execute a VS, and the mesh shader reads its deformed pair from
// the GfxMeshParams blob, so the shared bytes are mutually exclusive).
// paramsAddress is identical for every span of the frame (one params
// upload — per-span uploads to the same buffer race transfer writes
// against graphics reads); listBase/listCount slice this dispatch.
static constexpr uint32_t kMeshTailPushOffset = 224;
struct MeshTailPush {
    uint64_t paramsAddress = 0;
    uint32_t listBase = 0;
    uint32_t listCount = 0;
};
static_assert(sizeof(MeshTailPush) == 16);

// Draw-side mesh eligibility (the variant/program side is resolved by the
// caller via MeshProgramFor): opaque draw-set. Exact with
// nothing on the shader side — the shader trusts its CPU-partitioned list.
inline bool MeshDrawEligible(uint32_t drawSet, uint32_t drawFlags) {
    (void)drawFlags;
    if (drawSet != DrawSetBit_Opaque)
        return false;
    return true;
}

// Per-mesh chart tail for direct-path vertex shaders (uvflat1.vert.hlsl,
// default.vert.hlsl — 16 bytes). Pushed at kUvFlatVsPushOffset (the scene
// push's unused prevModel scratch, bytes 192..208) per mesh. Zeroed
// scratch reads hasXforms == 0 and selects the raw chart-uv passthrough,
// so passes that never push a tail need no handling; chartOffset is the
// mesh's base into the lmXforms table. Must match the HLSL tail structs.
static constexpr uint32_t kUvFlatVsPushOffset = 192;
struct UvFlatVsPushTail {
    uint32_t hasXforms = 0;
    uint32_t chartOffset = 0;
    uint64_t lmXforms = 0;
};
static_assert(sizeof(UvFlatVsPushTail) == 16);

// DIRECT static-geometry LM metadata at bytes 224..240. This overlaps
// the 16-byte unused padding of DepthSkinTail (208..240). The Main
// direct draw publishes this AFTER the per-draw skin tail. A magic word
// makes existing non-main uses of the same vertex module fail closed.
static constexpr uint32_t kDirectLightmapPullOffset = 224;
static constexpr uint32_t kDirectLightmapPullMagic = 0x4C4D5055u; // 'LMPU'
struct DirectLightmapPullTail {
    uint32_t delta = 0;
    uint32_t lobeCount = 0xFFFFFFFFu;
    uint32_t magic = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(DirectLightmapPullTail) == 16);

// One indirect command range per (material, lightmap lobe class)
// (commands sorted by both keys): a single multi-draw per range, and
// the Main filtered leg binds the matching lit program (the generic
// one, or the SG_LM_LOBE_COUNT-specialized twin for uniform-lobe
// ranges) exactly once per range.
struct MaterialRange {
    uint32_t material = 0;
    uint32_t firstCommand = 0;
    uint32_t commandCount = 0;
    // Cooked SG-lightmap lobe count shared by every draw in the range
    // (5/6/9/12; 0xFFFFFFFFu = mixed/none — bind the generic lit FS).
    uint32_t lobeCount = 0xFFFFFFFFu;
};

struct Stats {
    uint32_t trianglesProcessed = 0;
    uint32_t clustersProcessed = 0;
    uint32_t clustersCulled = 0;
    uint32_t draws = 0;
    uint32_t commands = 0;
};

// ---------------------------------------------------------------------------
// Plain state (no methods). Single instance lives in the app renderer.
// ---------------------------------------------------------------------------

struct DrawRequest {
    uint32_t mesh = 0;
    uint32_t material = 0;
    glm::mat4 model{1.0f};
    DrawFlags flags = 0;
    DrawSetMask drawSet = DrawSetBit_Opaque;
    uint32_t lmChunk = 0xFFFFFFFFu; // rides the draw ring (misc.w)
    uint32_t lmChartOffset = 0;     // base into the chart transform buffer (misc2.x)
    // The per-draw additive lightmap-heap delta (HLSL misc2.w; see the
    // Draw field). 0xFFFFFFFF = the mesh carries no lightmap stream.
    uint32_t lmDelta = 0xFFFFFFFFu;
    // The RESOLVED per-draw material userdata (HLSL misc3.x — the
    // material instance override or the base material default, resolved
    // by the caller from the World's runtime state at frame build; see
    // World::ResolvedItemUserData). 0 = none (the StandardLit no-op
    // family ignores it entirely).
    uint32_t userData = 0;
    uint32_t lightLayerMask = 0xffffffffu;
};

// Cluster / Mesh moved to gpu_scene_static.h (tour::gfx::Cluster /
// tour::gfx::Mesh) — the render sidecar references them from StaticGeometry.

struct View {
    glm::vec3 eye{0.0f};
    CullFlags cullFlags = CullFlagBit_Degenerate;
    uint32_t width = 0, height = 0;
    glm::mat4 viewProj{1.0f};
    const smvk::RenderView* renderView = nullptr; // frustum planes (owned by RenderViewSet)
    std::vector<Batch> batches;
    std::vector<Draw> drawConstants;
    std::vector<MaterialRange> materialRanges;
    uint32_t batchCount = 0;
    uint32_t frameIndex = 0; // which ring slice this frame's data lives in
    Stats stats{};
    // Compacted eligible mesh batch indices (capacity retained across
    // frames: RecordMeshMain rewrites it per mesh dispatch set).
    std::vector<uint32_t> meshListScratch;
    // Per-view outputs AND inputs, ringed by frame in flight.
    // Everything mutable is per (view, frame): frames overlap (the swapchain
    // only retires the same slot), and uploads flush immediately, so any
    // single-copy buffer would be clobbered by the next frame while the
    // current one still reads it. Same rule as RenderViewSet's slab.
    smvk::Holder<smvk::BufferHandle> batchBuffers[smvk::limits::MaxFramesInFlight];
    smvk::Holder<smvk::BufferHandle> drawBuffers[smvk::limits::MaxFramesInFlight];
    smvk::Holder<smvk::BufferHandle> indirectBuffers[smvk::limits::MaxFramesInFlight];
    smvk::LogicalBufferHandle batchLogicals[smvk::limits::MaxFramesInFlight]{};
    smvk::LogicalBufferHandle drawLogicals[smvk::limits::MaxFramesInFlight]{};
    smvk::LogicalBufferHandle indirectLogicals[smvk::limits::MaxFramesInFlight]{};
    // GPU cull chain state, per (view, flight):
    //   kept — kept-bit ring with TWO parity regions inside one buffer
    //          (the frame with parity P writes region P and reads 1-P,
    //          written smvk::limits::MaxFramesInFlight frames earlier — the flight
    //          fence guarantees that writer is complete; creation zeroes
    //          it so the first frames seed nothing). One buffer per flight
    //          is enough because the parity alternates between the
    //          flight's consecutive users. Each region write is
    //          two-stage within the frame, rendergraph-ordered:
    //          gfxbatchcull stores the pre-occlusion geometric verdict,
    //          and the filter (the slot's exclusive command owner)
    //          overwrites it with the FINAL-visibility verdict — so the
    //          temporal split's prevKept means "finally visible two
    //          frames ago", never "frustum-visible two frames ago".
    //   cullState — draw-cull verdict words + the filter's atomic
    //          filtered-offset counter (word drawCapacity, the runtime
    //          suffix carried through the pushes since the cutover).
    smvk::Holder<smvk::BufferHandle> keptBuffers[smvk::limits::MaxFramesInFlight];
    smvk::Holder<smvk::BufferHandle> cullStateBuffers[smvk::limits::MaxFramesInFlight];
    smvk::LogicalBufferHandle keptLogicals[smvk::limits::MaxFramesInFlight]{};
    smvk::LogicalBufferHandle cullStateLogicals[smvk::limits::MaxFramesInFlight]{};
    // Filtered-index scratch lives in the static index monoblock (one index
    // VkBuffer for mesh + cluster-ordered + filtered regions; firstIndex is
    // absolute, so the index binding never switches). Per (view, flight)
    // persistent regions, allocated on slot creation, freed on teardown.
    // Base in uint32 indices; allocBits is the VMA virtual-allocation
    // bit pattern (VmaVirtualAllocation is a uint64_t handle; this header
    // stays VMA-free, see the static_assert in gfx_filter.cpp).
    uint32_t filteredBase[smvk::limits::MaxFramesInFlight] = {};
    uint64_t filteredAllocBits[smvk::limits::MaxFramesInFlight] = {};
    // RUNTIME per-(view, flight) capacities (the dynamic-capacity cutover,
    // docs/gfx_dynamic_capacity_gpu_default.md): every ring above is sized
    // by these — page-planned from the frame's exact counts (tour::gfx::
    // PlanViewCapacity) at FinalizeView, GROW-ONLY (a smaller frame just
    // leaves the tail unused; a resize churns allocations + zero-fills).
    // The cullState slice's counter/diag words sit at the RUNTIME suffix
    // drawCapacity + 0..4 (carried to the shaders through the pushes —
    // never a compile-time macro), and the kept ring's parity regions
    // stride by candidateCapacity words. Fail-closed ceilings:
    // tour::gfx::kDeclaredMaxDraws / kDeclaredMaxCandidates.
    uint32_t drawCapacity[smvk::limits::MaxFramesInFlight] = {};
    uint32_t candidateCapacity[smvk::limits::MaxFramesInFlight] = {};
};

// Hierarchical-Z store for one view: MIN-reduced depth pyramid (reversed-Z
// conservative, see hzb.hlsli) in a Storage float buffer with manual mip
// layout (BDA style, no storage images / samplers needed). Mip 0 mirrors
// the depth texture 1:1; mip k dims are max(1, ceil(dim / 2^k)) - i.e.
// repeated dst = max((src + 1u) >> 1, 1u), clamped 2x2 covers odd edges.
// Backwards-compatible names: the storage and mip layout are now owned
// by the renderer-neutral visibility subsystem, not by gfx::State.
inline constexpr uint32_t kHizMaxMips = tour::visibility::kHizMaxMips;
using HizResources = tour::visibility::HizResources;
using tour::visibility::HzbMipCount;
using tour::visibility::HzbMipDims;
using tour::visibility::HzbMipOffsetFloats;
using tour::visibility::HzbReduce2x2;
using tour::visibility::HzbTotalFloats;

// Per-cluster screen-space footprint (T4). Pure-CPU mirror of the filter
// CS math (same formulas, same conservatism); the shader duplicates them
// in HLSL (see gfxfilter.cs.hlsl ClusterOcclusion).
struct ClusterFootprint {
    float centerX = 0.0f, centerY = 0.0f; // px, framebuffer y-down
    float radiusX = 0.0f, radiusY = 0.0f; // px semi-axes (conservative)
    float nearestDepth = 0.0f;            // reversed-Z value of the nearest point
    bool behindCamera = true;             // set when untestable (eye inside / w <= 0)
};
// Projects a world-space bounding sphere. viewProj is the Y-flipped
// main-view matrix; width/height is the HZB target extent. The footprint
// is the conservative clip-interval projection (radii from viewProj row
// lengths, exact under any rotation); the shader widens it by the jitter
// guard, the audit adds +1px to match (see GfxRenderer::DumpIndirectAudit).
ClusterFootprint ProjectClusterFootprint(const glm::vec3& worldCenter,
                                         float worldRadius,
                                         const glm::vec3& eye,
                                         const glm::mat4& viewProj,
                                         uint32_t width,
                                         uint32_t height);
// Mip whose texel covers a rectW x rectH px footprint (ceil(log2(max)),
// clamped to [0, mips); caller guarantees mips > 0.
uint32_t HzbMipForRect(float rectW, float rectH, uint32_t mips);
// Conservative occlusion test over an explicit HZB image (row-major
// floats in HzbMipOffsetFloats layout, MIN-reduced). True = provably
// hidden (nearest point behind every covered texel). Rects missing the
// target, behind-camera footprints and empty pyramids keep (false).
// outBindingStored (optional): the MIN stored depth over the covered
// texels — the value the cull decision hangs on (audit/debug).
bool HzbOccluded(const float* hzb,
                 uint32_t w,
                 uint32_t h,
                 uint32_t mips,
                 const ClusterFootprint& f,
                 float* outBindingStored = nullptr);

// Compute push. Must match HLSL HzbPush in hzb.hlsli (48 bytes).
// srcMip = kHizSrcDepth copies mip 0 from the depth texture instead.
inline constexpr uint32_t kHizSrcDepth = tour::visibility::kHizSrcDepth;
using HzbPush = tour::visibility::HzbPush;
static_assert(sizeof(HzbPush) == 48);

// Single-dispatch SPD HZB build (hzb_spd.cs.hlsl): one 256-lane
// workgroup per 64x64 mip-0 tile cascades mips 0..6 through LDS; a
// completion counter hands the tail (mips 7+) to the last completed
// group — AMD SPD's protocol, on the SAME BDA buffer + mip layout +
// clamped-footprint semantics as the per-mip HzbPush path (byte-
// identical output is the A/B + verify contract).
// counter = THIS frame-in-flight's slice of the counter ring (the
// caller resolves BDA + 16 * frameIndex at call time).
using HzbSpdPush = tour::visibility::HzbSpdPush;
static_assert(sizeof(HzbSpdPush) == 104);

// Per-view temporal visibility sidecar (Alan Wake 2 style). Deliberately
// NOT part of RenderView: that struct is HLSL-mirrored (256 bytes,
// static_asserted), so history lives here, parallel to views. Only the
// HZB pyramid + contribution gate remain.
struct TemporalView : tour::visibility::HizViewResources {
    bool historyValid = false;        // false until one full visibility frame lands
    float minimumContribution = 0.0f; // px-area threshold, T4 (0 = disabled)
    uint32_t drawSetMask = DrawSetMask_All;
    uint32_t width = 0, height = 0; // extent the history was built for
};

struct State : StaticGeometry {
    // The STATIC registration half (filterIndices/meshletVertices/
    // meshletTriangles/clusters/meshes/clusterIndexBase/the cluster &
    // meshlet & mesh GPU buffers) is inherited from StaticGeometry
    // (gpu_scene_static.h). This render-side half keeps the per-view
    // cull/draw/mesh/Hi-Z state the GPU-scene backends and GfxRenderer
    // still consume.
    uint32_t frameCounter = 0; // BeginFrame ++
    // Draw-level texture-demand source (gfxfilter.cs.hlsl
    // PublishDrawDemand; see the FilterPush tail + the demand API below).
    // meshBounds: per-mesh union bound over the mesh's clusters (xyz
    // center, w radius, OBJECT space), static after UploadStaticData.
    std::vector<glm::vec4> meshBounds;
    smvk::Holder<smvk::BufferHandle> meshBoundsBuffer;
    smvk::LogicalBufferHandle meshBoundsLogical{};
    // textureMeta/epoch/count: per-slot meta table, re-uploaded only when
    // the streamer's epoch bumps (register/unregister only; slots are
    // stable across retargets so retargets never re-upload).
    smvk::Holder<smvk::BufferHandle> textureMetaBuffer;
    uint64_t textureMetaEpoch = 0; // streamer epoch last uploaded (0 = none)
    uint32_t textureMetaCount = 0; // live slot count in the uploaded table
    // Demand ring: one uint per bindless slot per flight. SetDemandInputs
    // creates it pre-filled with kDemandNoReport (the first consume before
    // any GPU write observes "no demand", not garbage); ClearDemand
    // re-fills every frame AFTER the consumer's download submit
    // (download-before-clear; the transfer chain orders the fill after any
    // in-flight copy, and the frame's filter writes wait the chain, so a
    // stale min can never pin a texture sharp — the cadence the retired
    // fragment-feedback ring proved).
    smvk::Holder<smvk::BufferHandle> demandBuffers[smvk::limits::MaxFramesInFlight];
    uint64_t demandMaterialsBda = 0; // this frame's slice (SetDemandInputs)
    float demandMipBias = 0.0f;
    // The per-frame DEFORMED heap (the Northlight compute-skinning
    // output; the renderer's skinning driver sets these once per frame -
    // the skinned cluster's deformed buffer BDA + the skinned span's
    // heap-id base). 0/UINT32_MAX = no skinned cluster this frame
    // (GFX_DRAW_SKINNED never sets, every fetch stays static).
    uint64_t skinDeformedBda = 0;
    uint32_t skinDeformedBase = UINT32_MAX;
    // The PREV parity half (last frame's cur half of the SAME deformed
    // double buffer - the driver flips a per-frame parity and captures
    // the old cur here before overwriting). skinPrevValid gates it: 1
    // only while consecutive frames ran the skinning on the same heap
    // (same buffer + same span base); the FIRST animated frame, a heap
    // rebuild or a skinning gap leaves 0 and the motion leg falls back
    // to zero mesh motion (cur == prev). Read by the motion vector tail
    // only (FillMotionPrevTail) - every render fetch stays cur-only.
    uint64_t skinDeformedPrevBda = 0;
    uint32_t skinPrevValid = 0;
    // GfxSkinBounds: recomputes each SKINNED cluster's object-space sphere
    // + normal cone (the meshopt cone convention, on the deformed FACE
    // normals — see FitDeformedCone) and the per-draw deformed union bound
    // from the frame's deformed heap (the static clusters[] stays bind-pose)
    // BEFORE the batch cull reads it - so the cull sees the ANIMATED
    // silhouette. One GfxSkinBoundsTable per flight: the runtime
    // candidate-capacity GfxClusterData slots + the draw-capacity drawBound
    // tail (one buffer, one BDA — sized by skinBounds*Want/Want since the
    // dynamic cutover). Lazily created on the first frame a deformed buffer
    // exists.
    smvk::Holder<smvk::ShaderModuleHandle> skinBoundsModule;
    smvk::ShaderProgram skinBoundsProgram{};
    smvk::Holder<smvk::BufferHandle> skinBoundsBuffers[smvk::limits::MaxFramesInFlight];
    smvk::LogicalBufferHandle skinBoundsLogicals[smvk::limits::MaxFramesInFlight]{};
    bool skinBoundsReady = false;
    // Runtime skinBounds table capacities (the dynamic cutover — the table
    // is STATE-level: one per flight, shared across views, indexed by the
    // candidate space exactly as the fixed table was). *Cap = the
    // allocated capacity (0 = never allocated; EnsureSkinBounds sizes a
    // fresh table from *Want); *Want = the latest frames' requirement
    // (monotone high-water across FinalizeView calls, so a lazily created
    // table starts big enough for the frame that armed it).
    uint32_t skinBoundsCandidateCap[smvk::limits::MaxFramesInFlight] = {};
    uint32_t skinBoundsDrawCap[smvk::limits::MaxFramesInFlight] = {};
    uint32_t skinBoundsCandidateWant[smvk::limits::MaxFramesInFlight] = {};
    uint32_t skinBoundsDrawWant[smvk::limits::MaxFramesInFlight] = {};
    // GfxConeDebug (the cone-axis overlay's GPU leg): writes two packed
    // DebugLineVertex float3s per candidate slot — the ACTUAL cull cones
    // (baked bind-pose for static draws, this frame's deformed cones for
    // skinned draws) — GPU-driven end to end (the retired CPU readback path
    // corrupted the host heap and never landed). Lazily armed by the
    // overlay's UI toggle (SetConeDebug); one shared line buffer (the
    // candidate slot space is a dense prefix, so [0, 2*candidateTotal) is
    // exactly the written range).
    smvk::Holder<smvk::ShaderModuleHandle> coneDebugModule;
    smvk::ShaderProgram coneDebugProgram{};
    smvk::Holder<smvk::BufferHandle> coneDebugLines;
    smvk::LogicalBufferHandle coneDebugLinesLogical{};
    bool coneDebug = false; // the UI toggle's armed state
    // Runtime line-buffer capacity (the dynamic cutover — ONE shared
    // buffer, candidate-slot strided): cap = allocated capacity,
    // want = the monotone high-water requirement (SetConeDebug sizes a
    // fresh buffer from it; FinalizeView grows an armed one).
    uint32_t coneDebugCap = 0;
    uint32_t coneDebugWant = 0;
    // The STATIC heap BDAs for the VS legs' skinned fetch (the same heaps
    // every draw path reads - the RHI globals from vulkan_impl.h, stashed
    // once at EnsureMeshParams so the record sites' fills never reach for
    // the globals themselves). 0 = not stashed yet (static fetches).
    uint64_t vsPosHeapBda = 0;
    uint64_t vsShadeHeapBda = 0;
    smvk::Holder<smvk::ShaderModuleHandle> filterModule;
    smvk::ShaderProgram filterProgram{};
    smvk::Holder<smvk::ShaderModuleHandle> hizModule;
    smvk::ShaderProgram hizProgram{};
    // SPD twin (hzb_spd.cs.hlsl .min — HZB_REDUCE_MAX=0, the MIN-reduced
    // culling pyramid in ONE dispatch; same BDA buffer + mip layout +
    // clamped footprints as the per-mip hizProgram path). A/B + verify
    // switches (env, set ONCE via SetHizSpd from GfxRenderer::Ensure):
    //   hizSpd       — TOURMALINE_HIZ_SPD=1: SPD feeds the cull chain
    //                  (default OFF until validated, like the ssgi twin).
    //   hizSpdVerify — TOURMALINE_HIZ_SPD_VERIFY=1: per-mip feeds the
    //                  chain, SPD builds the hizVerify shadow from the
    //                  same depth, VerifyHizSpd byte-compares post-
    //                  Execute (stalls; 0 mismatching texels is the
    //                  contract — a wrong MIN pyramid makes geometry
    //                  disappear, so this gate stays until it is green).
    smvk::Holder<smvk::ShaderModuleHandle> hizSpdModule;
    smvk::ShaderProgram hizSpdProgram{};
    bool hizSpd = false;
    bool hizSpdVerify = false;
    bool hizSpdVerifyArmed = false;  // this frame built both pyramids
    uint64_t hizSpdVerifyFrames = 0; // running compare stats (report)
    uint64_t hizSpdVerifyMismatches = 0;
    smvk::Holder<smvk::BufferHandle> meshParamsBuffers[smvk::limits::MaxFramesInFlight];
    smvk::LogicalBufferHandle meshParamsLogicals[smvk::limits::MaxFramesInFlight]{};
    // Compacted eligible batch indices, rewritten per mesh dispatch set
    // (one upload per frame covering all variant spans; dispatches slice
    // it via the listBDA base). Candidate-slot capacity per flight (the
    // dynamic cutover — grows with the candidate capacity; the retired
    // fixed sizing was kMaxBatchSlots).
    smvk::Holder<smvk::BufferHandle> meshBatchLists[smvk::limits::MaxFramesInFlight];
    smvk::LogicalBufferHandle meshBatchListLogicals[smvk::limits::MaxFramesInFlight]{};
    uint32_t meshBatchListCap[smvk::limits::MaxFramesInFlight] = {};
    uint32_t meshBatchListWant[smvk::limits::MaxFramesInFlight] = {};
    std::vector<View> views;
    std::vector<TemporalView> temporals; // parallel to views (same slot)
    uint32_t viewCount = 0;
};

// ---------------------------------------------------------------------------
// Procedural API.
// ---------------------------------------------------------------------------

inline constexpr const char* kGfxFilterCsPath = "content/shaders/gfxfilter.cs.hlsl.spv";
inline constexpr const char* kGfxFilteredVsPath = "content/shaders/gfxfiltered.vert.hlsl.spv";
inline constexpr const char* kHizCsPath = "content/shaders/hzb.cs.hlsl.spv";
// The .min variant twin (CMake VARIANTS: HZB_REDUCE_MAX=0 — the culling
// MIN pyramid; the base .spv is the ssgi MAX build).
inline constexpr const char* kHizSpdCsPath = "content/shaders/hzb_spd.cs.hlsl.min.spv";
inline constexpr const char* kGfxSkinBoundsCsPath = "content/shaders/gfxskinbounds.cs.hlsl.spv";
inline constexpr const char* kGfxConeDebugCsPath = "content/shaders/gfxcone_debug.cs.hlsl.spv";

// ---- GPU cull chain pass shells (caller sets the recording func) ----
// Deformed cluster bounds: runs between the skinning pass and GfxBatchCull.
// One 128-thread workgroup per draw; threads stride the draw's clusters and
// write an object-space bounding sphere (cone degenerated) per candidate
// slot from the frame's deformed heap. No-op when no skinned cluster exists
// (the caller gates on State::skinDeformedBda).
smvk::RenderPass& AddSkinBoundsPass(State& s, smvk::RenderGraph& rg, uint32_t view);
void PushAndDispatchSkinBounds(State& s,
                               smvk::CommandBuffer& cmd,
                               smvk::ResourceManager& ctx,
                               uint32_t view);
// Creates the per-flight skinBounds ring + module lazily (idempotent).
bool EnsureSkinBounds(State& s, smvk::ResourceManager& ctx, smvk::RenderGraph& rg);
// Cone-axis overlay's GPU leg: runs after GfxSkinBounds, before Main's
// overlay draw. One 128-thread workgroup per draw; threads stride the
// draw's clusters and write TWO packed DebugLineVertex float3s per
// candidate slot (apex -> tip) — the ACTUAL cull cones (baked bind-pose
// for static draws, this frame's deformed cones for skinned draws). The
// candidate slot space is a dense prefix, so [0, 2*candidateTotal) is
// exactly the written range; Main Draws that count over the line buffer.
smvk::RenderPass& AddConeDebugPass(State& s, smvk::RenderGraph& rg, uint32_t view);
void PushAndDispatchConeDebug(State& s,
                              smvk::CommandBuffer& cmd,
                              smvk::ResourceManager& ctx,
                              uint32_t view);
// Arms/disarms the overlay (the UI toggle's handler): on enable, lazily
// creates the module/program + the shared line buffer + logical; on
// disable, only the flag flips (the buffer stays — re-arm is free). The
// per-frame pass is recorded only while ConeDebugReady(s).
void SetConeDebug(State& s, smvk::ResourceManager& ctx, smvk::RenderGraph& rg, bool enable);
[[nodiscard]] inline bool ConeDebugReady(const State& s) {
    return s.coneDebug && s.coneDebugProgram.shaderCount != 0 && !s.coneDebugLines.empty() &&
           !s.coneDebugLinesLogical.empty();
}
[[nodiscard]] inline smvk::LogicalBufferHandle ConeDebugLinesLogical(const State& s) {
    return s.coneDebugLinesLogical;
}
[[nodiscard]] inline smvk::BufferHandle ConeDebugLinesBuffer(const State& s) {
    return s.coneDebugLines; // Holder's implicit handle conversion
}
bool EnsureShaders(State& s, smvk::ResourceManager& ctx, const char* csPath = kGfxFilterCsPath);
// Task-less mesh path ring (per-flight 152B params blob + logicals).
// Best-effort: failure only disables the mesh record (indexed stays).
bool EnsureMeshParams(State& s, smvk::ResourceManager& ctx, smvk::RenderGraph& rg);
// Mesh record: upload params, bind mesh pipeline, push tails,
// DrawMeshTasks(2*batchCount). The full scene push must already be pushed
// (same as the indexed leg). Null-BDA guard refuses the dispatch (same
// MMU-fault rule as PushAndDispatch).
// Uploads the frame's mesh params blob once (shared BDAs + dims; the
// per-dispatch list slice rides the tail push, never this buffer).
// liveViewProj is the LIVE main-view matrix the output projection uses
// (== the view record's matrix when the culling freeze is off; the two
// differ while the freeze holds the filter). Returns the params BDA,
// 0 on null-BDA refuse (indexed covers).
uint64_t UploadMeshParams(State& s,
                          smvk::ResourceManager& ctx,
                          uint32_t view,
                          uint64_t listBDA,
                          uint64_t lmXformsAddr,
                          const glm::mat4& liveViewProj);
// One variant dispatch: bind mesh pipeline, push tails, DrawMeshTasks.
// Params must already be uploaded (UploadMeshParams); listBase/listCount
// slice this dispatch out of the shared compacted list. The raster comes
// from the span's material pipeline (double-sided materials dispatch
// CullMode::None — mirrors bindForMaterial for the indexed leg).
void RecordMeshVariant(State& s,
                       smvk::CommandBuffer& cmd,
                       smvk::ResourceManager& ctx,
                       uint32_t view,
                       smvk::ShaderProgram& meshProg,
                       uint64_t paramsBDA,
                       uint32_t listBase,
                       uint32_t listCount,
                       uint64_t renderViewsAddr,
                       uint32_t renderViewIndex,
                       uint64_t lmXformsAddr,
                       const smvk::DepthSettings& depth,
                       const smvk::MSAASettings& msaa,
                       const smvk::RasterSettings& raster);
// Creates shader only; per-(view, frame) buffers are allocated on demand
// by BeginView (pool) so unused views cost nothing.
bool Ensure(State& s,
            smvk::ResourceManager& ctx,
            smvk::RenderGraph& rg,
            const char* csPath = kGfxFilterCsPath);
void Destroy(State& s, smvk::RenderGraph& rg);
[[nodiscard]] inline bool Valid(const State& s) {
    return s.filterProgram.shaderCount != 0;
}

// Mesh registration (RegisterMeshFromUpload/FromCooked), UploadStaticData
// and ResetStaticRegistrations moved to gpu_scene_static.h (they operate on
// tour::gfx::StaticGeometry — the static registration half — not State).
[[nodiscard]] inline uint32_t MeshCount(const State& s) {
    return (uint32_t)s.meshes.size();
}
[[nodiscard]] inline uint32_t ClusterCount(const State& s) {
    return (uint32_t)s.clusters.size();
}

void BeginFrame(State& s);
// Starts one filtered view bound to an allocated RenderView (frustum planes
// come from it; eye = camera world position for the cone test).
// Returns the view slot, UINT32_MAX on failure.
uint32_t BeginView(State& s,
                   smvk::ResourceManager& ctx,
                   smvk::RenderGraph& rg,
                   const smvk::RenderView& renderView,
                   const glm::vec3& eye,
                   CullFlags cullFlags,
                   uint32_t width,
                   uint32_t height);
// Cone fit result (the object-space fields feed Cluster's cone tail exactly):
// degenerate = true keeps the keep-cone (axis 0 -> GfxConeCulled's len2 <
// 1e-12 keep; the value the cluster fields must hold in that case).
struct DeformedConeFit {
    glm::vec3 coneApex{0.0f};
    glm::vec3 coneAxis{0.0f}; // 0 = degenerate (the keep-cone)
    float coneCutoff = 1.0f;
    bool degenerate = true;
};
// Pure-CPU reference of GfxSkinBounds' cone rebuild (gfxskinbounds.cs.hlsl):
// the meshopt_computeMeshletBounds cone convention, on caller-supplied
// (deformed) triangle corners — the exact math the HLSL walks, guarded and
// thresholded identically, exported so unit tests pin the GPU fit (the HLSL
// is the convention-checked twin; a verdict that culls a viewpoint ANY face
// plane still sees is a fit bug). triCorners = 3*triCount positions;
// sphereCenter = the caller's conservative sphere center (the pass's AABB
// center — any anchor works: validity only needs the apex behind every
// face plane, which the maxt construction guarantees).
DeformedConeFit FitDeformedCone(const glm::vec3* triCorners,
                                uint32_t triCount,
                                const glm::vec3& sphereCenter);
// ---- Temporal visibility (T1 state, T3/T5 consumers) ----
// Accessor (resizes the sidecar on demand so hand-built unit-test Views
// without GPU buffers still work).
[[nodiscard]] inline TemporalView& Temporal(State& s, uint32_t view) {
    if (s.temporals.size() <= view)
        s.temporals.resize(view + 1);
    return s.temporals[view];
}
[[nodiscard]] inline const TemporalView& Temporal(const State& s, uint32_t view) {
    assert(view < s.temporals.size());
    return s.temporals[view];
}
// ---- Hierarchical-Z (T2 build, T4 consumer) ----
bool EnsureHizShaders(State& s, smvk::ResourceManager& ctx, const char* csPath = kHizCsPath);
// (Re)creates the view's HZB buffer for w/h (frees the old logical).
// Resets historyValid when the size drifts (stale seed geometry extent).
// Returns false on zero extent or allocation failure (history invalid).
bool EnsureHiz(State& s,
               smvk::ResourceManager& ctx,
               smvk::RenderGraph& rg,
               uint32_t view,
               uint32_t w,
               uint32_t h);
// Compute pass shell for one HZB level (caller sets the recording func
// to PushAndDispatchHiz). dstMip 0 additionally reads depthLogical.
// passName labels the node in the framegraph; the post-remainder refresh
// passes "GfxHizRefresh" so the two builds are distinguishable.
smvk::RenderPass& AddHizPass(State& s,
                             smvk::RenderGraph& rg,
                             uint32_t view,
                             uint32_t dstMip,
                             smvk::LogicalImageViewHandle depthLogical,
                             const char* passName = "GfxHiz");
// Recording helper: pushes HzbPush for dstMip and dispatches the level.
// depthBindlessIndex is consumed only for dstMip 0 (UINT32_MAX otherwise).
void PushAndDispatchHiz(State& s,
                        smvk::CommandBuffer& cmd,
                        smvk::ResourceManager& ctx,
                        uint32_t view,
                        uint32_t dstMip,
                        uint32_t depthBindlessIndex = UINT32_MAX);
// ---- single-dispatch SPD HZB (hzb_spd .min; the culling twin of the
// per-mip pair above — see HizResources' counter-ring comment and the
// shader header for the tile geometry + last-group protocol) ----
// Env-switch setter (call ONCE at startup; GfxRenderer::Ensure reads
// TOURMALINE_HIZ_SPD / TOURMALINE_HIZ_SPD_VERIFY).
void SetHizSpd(State& s, bool live, bool verify);
// Best-effort module load (gfx::Ensure calls it like the cull-chain
// programs; failure keeps the per-mip path).
bool EnsureHizSpdShaders(State& s, smvk::ResourceManager& ctx, const char* csPath = kHizSpdCsPath);
// Compute pass shell for the ONE-dispatch build: the whole pyramid in a
// single pass (caller sets the recording func to PushAndDispatchHizSpd).
// shadow=true targets the verify twin (t.hizVerify[frameIndex], the
// hizVerify "GfxHizSpdVerify" node) instead of the live pyramid.
// REQUIRES EnsureHiz to have built the view's counter ring (the caller
// gates on it — an absent ring falls back to the per-mip path).
smvk::RenderPass& AddHizSpdPass(State& s,
                                smvk::RenderGraph& rg,
                                uint32_t view,
                                smvk::LogicalImageViewHandle depthLogical,
                                const char* passName = "GfxHizSpd",
                                bool shadow = false);
// Recording helper: pushes HzbSpdPush and dispatches the whole build
// (one workgroup per 64x64 mip-0 tile; groupCount rides the push).
// depthBindlessIndex feeds the mip-0 copy (UINT32_MAX = undefined mip 0,
// same over-cull warning as the per-mip path).
void PushAndDispatchHizSpd(State& s,
                           smvk::CommandBuffer& cmd,
                           smvk::ResourceManager& ctx,
                           uint32_t view,
                           uint32_t depthBindlessIndex,
                           bool shadow = false);
// Verify compare (TOURMALINE_HIZ_SPD_VERIFY; post-Execute, STALLS):
// downloads the live per-mip pyramid + the SPD shadow of this flight
// and byte-compares every mip (exact equality expected — deterministic
// MIN over identical finite inputs). Prints per-frame + running totals.
void VerifyHizSpd(State& s, smvk::ResourceManager& ctx, uint32_t view);
// BDA of the view's HZB buffer (0 when unallocated; T4 skips occlusion then).
uint64_t HizAddress(State& s, smvk::ResourceManager& ctx, uint32_t view);
// DEBUG: Download and print HZB buffer contents for inspection.
void DumpHzb(State& s, smvk::ResourceManager& ctx, uint32_t view);

void AddConsumeHints(smvk::RenderPass& pass,
                     smvk::LogicalBufferHandle filteredLogical,
                     smvk::LogicalBufferHandle indirectLogical);

// ---- Draw-level texture demand (designated-publisher source) ----
// Per-frame demand inputs (app-resolved): THIS frame's materials BDA
// slice + the streamer's mip bias. Idempotent; call once per frame before
// AppendPasses records the filter (PushAndDispatch reads them). Creates
// the demand ring pre-filled with kDemandNoReport on first use.
void SetDemandInputs(State& s, smvk::ResourceManager& ctx, uint64_t materialsBda, float mipBias);
// Uploads the per-slot meta table when the streamer's epoch changes.
// Returns false on buffer/upload failure (retry next frame — the cached
// epoch stays stale so the call re-attempts).
bool UploadTextureMeta(State& s,
                       smvk::ResourceManager& ctx,
                       uint64_t epoch,
                       const TextureMeta* meta,
                       size_t count);
// Demand ring slice for the readback consumer (empty handle when the ring
// was never created). Slots hold wanted FULL-CHAIN mips;
// kDemandNoReport = no draw reported the texture this frame.
[[nodiscard]] inline smvk::BufferHandle DemandBuffer(const State& s, uint32_t frameIndex) {
    assert(frameIndex < smvk::limits::MaxFramesInFlight);
    return s.demandBuffers[frameIndex];
}
// GPU cull-state slice for the readback consumer (empty handle when the
// GPU cull never ran): word CullStateCounterWord(s, view, frameIndex)
// holds that dispatch chain's final KEPT-entry count (the filter's
// atomic filtered-offset counter - zeroed by the batch-cull dispatch,
// accumulated per kept entry), i.e. the GPU-side cull survivor count the
// CPU stats cannot see. MAIN view (views[0] - the filter's view 0; shadow
// legs keep their own).
[[nodiscard]] inline smvk::BufferHandle CullStateBuffer(const State& s, uint32_t frameIndex) {
    assert(frameIndex < smvk::limits::MaxFramesInFlight);
    if (s.views.empty())
        return smvk::BufferHandle{};
    return s.views[0].cullStateBuffers[frameIndex];
}
// RUNTIME cull-state word layout (the dynamic-capacity cutover — the
// retired compile-time kCullState* constants' successors): verdict words
// occupy [0, drawCapacity), the filtered-offset allocation counter sits
// at word drawCapacity, the four diag counters (empty/overflow/
// allocated/gated) follow it, and the slice spans drawCapacity + 5 words
// in total. Per (view, flight) — the readback consumers must capture the
// value that accompanied their SUBMITTED download (a later grow moves
// the suffix; the poll-side span only needs the stable dest + size).
[[nodiscard]] inline uint32_t CullStateCounterWord(const State& s,
                                                   uint32_t view,
                                                   uint32_t frameIndex) {
    assert(view < s.views.size());
    assert(frameIndex < smvk::limits::MaxFramesInFlight);
    return s.views[view].drawCapacity[frameIndex];
}
[[nodiscard]] inline uint32_t CullStateWords(const State& s, uint32_t view, uint32_t frameIndex) {
    return CullStateCounterWord(s, view, frameIndex) + 5u;
}
// Current-flight capacity accessors (the dispatch/push fill sites and
// the audits read these; drawCapacity doubles as the counter word base).
[[nodiscard]] inline uint32_t DrawCapacity(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].drawCapacity[s.views[view].frameIndex];
}
[[nodiscard]] inline uint32_t CandidateCapacity(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].candidateCapacity[s.views[view].frameIndex];
}
// Live slot count in the uploaded meta table (bounds the consumer's
// download + forward range; 0 = demand source inactive — the shader's
// publisher is gated off too).
[[nodiscard]] inline uint32_t TextureMetaCount(const State& s) {
    return s.textureMetaCount;
}
// Per-record demand clear: kDemandNoReport-fills the current flight
// slice. Call AFTER the consumer's download submit (download-before-
// clear, same queue-ordering the retired fragment-feedback ring used).
// No-op when the ring was never created.
void ClearDemand(State& s, smvk::ResourceManager& ctx, uint32_t frameIndex);

// ---- Accessors (frame slices resolved from the view's frameIndex) ----
[[nodiscard]] inline uint32_t ViewFrame(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].frameIndex;
}
// HZB pyramid slice for this view's current flight (same rule as every
// other per-frame mutable above).
[[nodiscard]] inline HizResources& Hiz(State& s, uint32_t view) {
    assert(view < s.temporals.size() && view < s.views.size());
    return s.temporals[view].hiz[s.views[view].frameIndex];
}
[[nodiscard]] inline const HizResources& Hiz(const State& s, uint32_t view) {
    assert(view < s.temporals.size() && view < s.views.size());
    return s.temporals[view].hiz[s.views[view].frameIndex];
}
[[nodiscard]] inline smvk::BufferHandle BatchBuffer(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].batchBuffers[s.views[view].frameIndex];
}
// Fills the FilteredVsPush tail's VS-skin fields (the skinned fetch
// swap's inputs) from the State. paramsValid gates the whole swap in
// the HLSL (a plain pointer compare is illegal on vk::BufferPointer):
// 1 = the frame carries a live deformed heap (the driver set
// skinDeformedBda). The bind-pose heap BDAs are eternal (stashed once
// at EnsureMeshParams); the deformed pair is per-frame (the renderer's
// skinning driver).
inline void FillVsSkinTail(FilteredVsPush& tail, const State& s) {
    tail.vsDeformed = s.skinDeformedBda;
    tail.vsDeformedBase = s.skinDeformedBase;
    tail.paramsValid = (s.skinDeformedBda != 0 && s.skinDeformedBase != UINT32_MAX) ? 1u : 0u;
}

// Motion-leg PREV tail (gfxfilteredmotion.vert.hlsl bytes 240..256, the
// free prevModel scratch's last 16B — the cur pair at 192..240 arrives for
// free via PushViewConstants inside PushMotionViewConstants: the motion VS
// re-declares the FilteredVsPush fields it reads instead of padding over
// them). Mirrors the HLSL FilteredMotionPush's prev fields exactly.
static constexpr uint32_t kFilteredMotionPrevTailOffset = 240;
struct FilteredMotionPrevTail {
    uint64_t vsPrevDeformed = 0; // the PREV parity half (last frame's pose)
    uint32_t vsPrevValid = 0;    // 1 = the prev half holds last frame's pose
    uint32_t pad = 0;
};
static_assert(sizeof(FilteredMotionPrevTail) == 16);

// Fills the motion leg's PREV tail from the State (the cur pair rides the
// FilteredVsPush via FillVsSkinTail). prevValid: consecutive skinning runs
// on the same heap — the first animated frame / a rebuild / a gap falls
// back to zero mesh motion in the VS (prevPos == curPos).
inline void FillMotionPrevTail(FilteredMotionPrevTail& tail, const State& s) {
    tail.vsPrevDeformed = s.skinDeformedPrevBda;
    tail.vsPrevValid = s.skinPrevValid;
}

[[nodiscard]] inline smvk::BufferHandle DrawBuffer(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].drawBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline smvk::BufferHandle IndirectBuffer(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].indirectBuffers[s.views[view].frameIndex];
}
// Static index monoblock accessor (mesh + cluster-ordered + filtered index
// regions share one VkBuffer; draws use absolute firstIndex, offset-0 bind).
[[nodiscard]] inline smvk::LogicalBufferHandle IndexHeapLogical(const State& s) {
    return s.indexHeapLogical;
}
[[nodiscard]] inline uint32_t ClusterIndexBase(const State& s) {
    return s.clusterIndexBase;
}
[[nodiscard]] inline uint32_t FilteredIndexBase(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].filteredBase[s.views[view].frameIndex];
}
[[nodiscard]] inline smvk::LogicalBufferHandle BatchLogical(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].batchLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline smvk::LogicalBufferHandle DrawLogical(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].drawLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline smvk::LogicalBufferHandle IndirectLogical(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].indirectLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline uint32_t BatchCount(const State& s, uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].batchCount;
}
[[nodiscard]] inline const std::vector<MaterialRange>& MaterialRanges(const State& s,
                                                                      uint32_t view) {
    assert(view < s.views.size());
    return s.views[view].materialRanges;
}
[[nodiscard]] inline uint32_t ViewCount(const State& s) {
    return s.viewCount;
}

} // namespace gfx
} // namespace tour

// The smvk::gfx = ::tour::gfx namespace alias is declared in
// gpu_scene_static.h (the registration surface this header builds on);
// it is NOT re-declared here to avoid a duplicate namespace-alias.
