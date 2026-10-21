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
//                     world-space normal-cone -> kept bits (parity ring) +
//                     temporal seed/remainder commands (prev-kept region,
//                     GPU-resident: seed = kept && prevKept, remainder =
//                     kept && !prevKept — slot-stable identity, both drift
//                     directions conservative) + the batch array + the
//                     filter's indirect statics
//     GfxSeedDepth  — prev-kept intersection into depth (T3)
//     GfxHiz        — the MIN-reduced pyramid (T2)
//     GfxDrawCull   — one thread per draw: mesh-bound frustum (FROZEN
//                     view) + HZB draw-bound occlusion verdicts
//     GfxFilter     — per-triangle tests + per-cluster HZB occlusion
//                     (T4); surviving groups allocate filtered-index
//                     ranges through an atomic counter (clamped at the
//                     ring capacity); verdict-dropped slots exit before
//                     any projection work
//     GfxRemainderDepth — the disocclusion leg (T5)
//   Main          — the mesh-shader leg (its indexCount==0 gate keeps
//                     exact mesh/indexed parity) + indexed ranges
//
// Geometry layout ("rearranged heap", load-time): cluster-ordered global
// vertex ids are baked once into an owned index buffer (positions stay in
// the shared static heap, so the direct path is untouched).

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace smvk {
struct StaticMeshUpload;
struct CookedMeshlets;

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
static constexpr uint32_t kMaxClusterTris = 128;
// Bistro-scale scenes need headroom here: ~2.9k draws / ~49k meshlet
// clusters (Sponza fits the old 1k/4k). Cost is one filtered-index ring
// slice per flight (kMaxBatches * kMaxClusterTris * 3 uints = 96MB at 64k)
// inside the 512MB static index monoblock — keep monoblock sizing in sync
// (see AllocateStaticVertexFmt in engine/src/Renderer/mesh.cpp).
static constexpr uint32_t kMaxBatches = 65536; // surviving (draw x cluster) per view
// GPU-cull candidate capacity: per (draw x cluster) candidate slot (the
// batch array, the indirect array, the seed/remainder command arrays and
// the kept-bit ring are pre-sized by this; survivors stay bounded by
// kMaxBatches through the filter's atomic ring clamp). ~2.9k draws x the
// pushed meshes' full cluster lists at Bistro scale (~100k) with 2.5x
// headroom. Cheap buffers only — the filtered ring stays kMaxBatches.
static constexpr uint32_t kMaxBatchSlots = 262144;
static constexpr uint32_t kMaxDraws = 4096;   // draws pushed per view
// Sized for LOD chains: every cooked sibling registers its own gfx mesh
// (~3x the LOD0-only count), so headroom over the pre-LOD 4k budget.
static constexpr uint32_t kMaxMeshes = 8192;
static constexpr uint32_t kMaxViews = 64; // mirrors kMaxRenderViews
// Draw-cull verdict words ([0, kMaxDraws)) + the filter's atomic
// filtered-offset counter (word kMaxDraws) packed in one small buffer.
static constexpr uint32_t kCullStateWords = kMaxDraws + 1;
static constexpr uint32_t kCullStateCounterWord = kMaxDraws;

// GPU cluster entry (4x uvec4). Must match HLSL GfxClusterData (64 bytes):
// (indexOffset, triCount, 0, 0) + (boundCenter.xyz, boundRadius) for the
// per-cluster HZB occlusion / contribution tests (T4) +
// (coneApex.xyz, coneCutoff) + (coneAxis.xyz, pad) for the GPU cull
// chain's world-space normal-cone test (gfxbatchcull; C++ CullCluster
// mirrors the same math as the tested CPU reference).
struct ClusterConstants {
	uint32_t indexOffset = 0; // into the cluster-ordered index buffer (uints)
	uint32_t triCount = 0;
	uint32_t pad0[2] = {};
	glm::vec3 boundCenter{ 0.0f };
	float boundRadius = 0.0f;
	glm::vec3 coneApex{ 0.0f };
	float coneCutoff = 1.0f;
	glm::vec3 coneAxis{ 0.0f, 1.0f, 0.0f };
	float conePad = 0.0f;
};
static_assert(sizeof(ClusterConstants) == 64);

// Chart atlas transform: xy = scale, zw = offset, so atlasUv =
// chartUv * xy + zw. One entry per (mesh chart), concatenated scene-wide
// sorted by mesh then chart; each draw's lmChartOffset is its mesh's base.
struct LmChartXform {
	glm::vec4 scaleOffset{ 0.0f }; // xy scale, zw offset
};
static_assert(sizeof(LmChartXform) == 16);

// Per-draw frame entry. Must match HLSL GfxDraw (96 bytes).
struct alignas(16) Draw {
	glm::mat4 model{};
	uint32_t mesh = 0;
	uint32_t flags = 0; // DrawFlags (bit 2 = skinned, bit 1 = dither-closeup)
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
};
static_assert(sizeof(Draw) == 96);

// GPU batch entry (1x uvec4): (cluster, draw, outputOffset, flags).
struct Batch {
	uint32_t cluster = 0;
	uint32_t draw = 0;
	uint32_t outputOffset = 0; // in uints; batchSlot * kMaxClusterTris * 3
	uint32_t flags = 0; // BatchFlags (HLSL batch.w: GFX_BATCH_*)
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

// Compute push. Must match HLSL GfxFilterPush in gfxfilter.hlsli (256 bytes).
struct FilterPush {
	uint64_t positions = 0;
	uint64_t indices = 0; // cluster-ordered index buffer (owned)
	uint64_t clusters = 0;
	uint64_t batches = 0;
	uint64_t draws = 0;
	uint64_t filtered = 0;
	uint64_t indirect = 0;
	// Deformed per-cluster bounds + per-draw deformed union tail
	// (GfxSkinBounds output, one GfxSkinBoundsTable per flight — the
	// kMaxBatchSlots cluster entries + the kMaxDraws drawBound tail in ONE
	// buffer, so the one BDA addresses both regions). Skinned draws cull
	// against THIS table instead of the static bind-pose clusters[] - the
	// pose can move a cluster clean out of its bind-pose sphere and the
	// static test would drop it; the draw-level fast rejects + the demand
	// publisher read the drawBound tail instead of the static mesh unions.
	// skinBoundsValid = 0 (no skinned cluster this frame) leaves every draw
	// on the static tables.
	uint64_t skinBounds = 0;
	glm::mat4 viewProj{};
	glm::vec3 eye{ 0.0f };
	float pad1 = 0.0f;
	uint32_t cullFlags = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t batchCount = 0;
	// GPU cull chain: kept-bit parity (0/1) for the kept-region lookup.
	uint32_t keptParity = 0;
	uint32_t skinBoundsValid = 0;
	// T4 occlusion/contribution inputs (0/empty disables the tests).
	uint64_t hzb = 0; // BDA of the MIN-reduced HZB float buffer
	uint32_t hizWidth = 0;
	uint32_t hizHeight = 0;
	uint32_t hizMips = 0;
	float minContribution = 0.0f; // px^2 footprint threshold (0 = off)
	// Filtered-output region base (uint32 indices into the static index
	// monoblock) for this dispatch's flight slice. The CS writes indirect
	// firstIndex = indexBase + outputOffset so draws address the monoblock
	// absolutely and the index binding never switches buffers.
	uint32_t indexBase = 0;
	uint32_t pad3 = 0;
	// Draw-level texture-demand inputs (PushAndDispatch zero-gates
	// textureMetaCount when ANY of these BDAs is null — a null BDA
	// dereference is an MMU fault and takes the whole device down; same
	// belt-and-braces rule as the HZB inputs, and the shader's single
	// PublishDrawDemand gate).
	uint64_t meshBounds = 0;      // BDA of the per-mesh union-bound table
	uint64_t materials = 0;      // BDA of THIS frame's MaterialValues slice
	uint64_t textureMeta = 0;     // BDA of the per-bindless-slot meta table
	uint64_t textureDemand = 0;  // BDA of this flight's demand ring slice
	uint32_t textureMetaCount = 0; // live slot count (atomic range bound)
	float mipBias = 0.0f;         // mirrors TextureStreamerConfig::mipBias
	// GPU cull chain inputs (always the kept-bit region + cullState):
	//   kept      — this frame's kept-bit region (gfxbatchcull verdicts)
	//   cullState — draw verdicts (gfxdrawcull) + the filtered-offset
	//               counter (word kCullStateCounterWord) this dispatch
	//               allocates through atomically
	uint64_t kept = 0;
	uint64_t cullState = 0;
};
static_assert(sizeof(FilterPush) == 256);

// GfxBatchCull push (first compute pass of the frame). Must match HLSL
// GfxBatchCullPush in gfxfilter.hlsli (208 bytes). One 128-thread
// workgroup per material-sorted draw; LIVE view (seed/remainder semantics).
struct BatchCullPush {
	uint64_t draws = 0;        // this frame's draws (misc2.z = slot base)
	uint64_t meshes = 0;       // static per-mesh cluster ranges
	uint64_t clusters = 0;     // static bounds + cones
	uint64_t meshBounds = 0;   // static (draw-level fast-discard gate)
	uint64_t batches = 0;      // out: candidate slots
	uint64_t indirect = 0;     // out: filter indirect statics
	uint64_t seedCmds = 0;     // out: seed commands (instanceCount-gated)
	uint64_t remCmds = 0;      // out: remainder commands
	uint64_t kept = 0;         // out (parity region) + in (prev region)
	uint64_t cullState = 0;    // out: offset-counter zero (group 0)
	glm::mat4 viewProj{};      // LIVE view
	glm::vec3 eye{ 0.0f };
	float pad1 = 0.0f;
	uint32_t drawCount = 0;
	uint32_t drawSetMask = DrawSetMask_All; // TemporalView bucket gate
	uint32_t clusterIndexBase = 0;          // absolute seed firstIndex base
	uint32_t keptParity = 0;               // 0/1 (prev = 1-parity)
	uint32_t width = 0;                     // guard-band extent (LIVE view)
	uint32_t height = 0;
	// Deformed per-cluster bounds + per-draw deformed union tail
	// (GfxSkinBounds output, one GfxSkinBoundsTable per flight — the
	// kMaxBatchSlots cluster entries + the kMaxDraws drawBound tail in ONE
	// buffer, so the one BDA addresses both regions). Skinned draws
	// (GFX_DRAW_SKINNED) cull against THIS table instead of the static
	// bind-pose clusters[] - the pose can move a cluster clean out of its
	// bind-pose sphere and the static test would drop it, punching an
	// animated silhouette hole the seed depth fills with sky; the
	// draw-level fast discard reads the drawBound tail instead of the
	// static mesh union (a whole-mesh pose swing outside the bind-pose
	// union would otherwise drop every slot of the draw). skinBoundsValid =
	// 0 (no skinned cluster this frame) leaves every draw on the static
	// tables.
	uint64_t skinBounds = 0;
	uint32_t skinBoundsValid = 0;
	uint32_t pad2 = 0;
	uint32_t pad3[2] = {};
};
static_assert(sizeof(BatchCullPush) == 208);

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
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SkinBoundsPush) == 64);

// GfxDrawCull push (after GfxHiz, before the filter). Must match HLSL
// GfxDrawCullPush in gfxfilter.hlsli (144 bytes). One thread per draw;
// FROZEN view (the gfx view record matrix — the debug culling freeze).
// Skinned draws test the DEFORMED union bound (skinBounds' drawBound tail)
// instead of the static mesh union; skinBoundsValid = 0 keeps the blanket
// skip for skinned draws (the static bind-pose union can swing a whole
// animated mesh outside itself — skipping only loses the fast reject).
struct DrawCullPush {
	uint64_t draws = 0;
	uint64_t meshBounds = 0;   // static (object-space union bounds)
	uint64_t cullState = 0;    // out: verdict words [0, drawCount)
	uint64_t hzb = 0;
	uint64_t skinBounds = 0;   // deformed unions (skinned draws; the table tail)
	glm::mat4 viewProj{};      // FROZEN view
	glm::vec3 eye{ 0.0f };
	float pad1 = 0.0f;
	uint32_t cullFlags = 0;    // GFX_CULL_OCCLUSION / GFX_CULL_HZBSKIPMASKED
	uint32_t drawCount = 0;
	uint32_t width = 0;         // guard-band extent (== HZB target)
	uint32_t height = 0;
	uint32_t hizMips = 0;
	uint32_t skinBoundsValid = 0; // 0: skinned draws keep the blanket skip
};
static_assert(sizeof(DrawCullPush) == 144);

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

// Mesh params blob. Must match GfxMeshParamsData in gfxmesh.hlsli
// (two matrices first for 16-alignment, then 9 BDAs, then 6 uints = 224B).
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
static_assert(sizeof(GfxMeshParams) == 232);

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
	glm::mat4 model{ 1.0f };
	DrawFlags flags = 0;
	DrawSetMask drawSet = DrawSetBit_Opaque;
	uint32_t lmChunk = 0xFFFFFFFFu; // rides the draw ring (misc.w)
	uint32_t lmChartOffset = 0; // base into the chart transform buffer (misc2.x)
};

struct Cluster {
	uint32_t mesh = 0;
	uint32_t indexOffset = 0; // into State::filterIndices (uints)
	uint32_t triCount = 0;
	glm::vec3 boundCenter{ 0.0f };
	float boundRadius = 0.0f;
	glm::vec3 coneApex{ 0.0f };
	glm::vec3 coneAxis{ 0.0f, 1.0f, 0.0f };
	float coneCutoff = 1.0f;
};

struct Mesh {
	uint32_t clusterStart = 0;
	uint32_t clusterCount = 0;
};

// One indirect draw over the cluster-ordered index heap (20 bytes, matches
// VkDrawIndexedIndirectCommand; firstInstance indexes this frame's draws).
struct SeedCommand {
	uint32_t indexCount = 0;
	uint32_t instanceCount = 1;
	uint32_t firstIndex = 0; // clusters[c].indexOffset into filterIndices
	int32_t vertexOffset = 0; // always 0: indices are heap-global ids
	uint32_t firstInstance = 0; // draw index into the visDraws snapshot
};
static_assert(sizeof(SeedCommand) == 20);

// One seed multi-draw range: same (bucket, material) throughout.
struct SeedRange {
	uint32_t drawSet = DrawSetBit_Opaque;
	uint32_t material = 0;
	uint32_t firstCommand = 0;
	uint32_t commandCount = 0;
};

struct View {
	glm::vec3 eye{ 0.0f };
	CullFlags cullFlags = CullFlagBit_Degenerate;
	uint32_t width = 0, height = 0;
	glm::mat4 viewProj{ 1.0f };
	const RenderView* renderView = nullptr; // frustum planes (owned by RenderViewSet)
	std::vector<DrawRequest> draws;
	std::vector<Batch> batches;
	std::vector<Draw> drawConstants;
	std::vector<MaterialRange> materialRanges;
	uint32_t batchCount = 0;
	uint32_t frameIndex = 0; // which ring slice this frame's data lives in
	// Version stamp of the batches/drawConstants lists (bumped by BeginView
	// and BuildBatches). The temporal key cache gates on it: a stale
	// sorted-key array can never be reused against mutated batches.
	// Hand-built lists (unit tests) keep 0 and always rebuild.
	uint64_t batchesSerial = 0;
	Stats stats{};
	// Per-frame build scratch (capacity retained across frames: the
	// steady-state batch build allocates nothing).
	std::vector<uint32_t> drawOrderScratch; // material sort permutation
	std::vector<uint32_t> meshListScratch; // compacted mesh batch indices
	// GPU-cull mode seed/remainder ranges: positional (bucket, material)
	// spans over the candidate slot layout (identical for both passes —
	// the instanceCounts written by gfxbatchcull gate the actual draws).
	std::vector<SeedRange> cullRanges;
	uint32_t cullOpaqueCommands = 0;
	// Per-view outputs AND inputs, ringed by frame in flight.
	// Everything mutable is per (view, frame): frames overlap (the swapchain
	// only retires the same slot), and uploads flush immediately, so any
	// single-copy buffer would be clobbered by the next frame while the
	// current one still reads it. Same rule as RenderViewSet's slab.
	Holder<BufferHandle> batchBuffers[MAX_FRAMES_IN_FLIGHT];
	Holder<BufferHandle> drawBuffers[MAX_FRAMES_IN_FLIGHT];
	Holder<BufferHandle> indirectBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle batchLogicals[MAX_FRAMES_IN_FLIGHT]{};
	LogicalBufferHandle drawLogicals[MAX_FRAMES_IN_FLIGHT]{};
	LogicalBufferHandle indirectLogicals[MAX_FRAMES_IN_FLIGHT]{};
	// GPU cull chain state, per (view, flight):
	//   kept — kept-bit ring with TWO parity regions inside one buffer
	//          (the frame with parity P writes region P and reads 1-P,
	//          written MAX_FRAMES_IN_FLIGHT frames earlier — the flight
	//          fence guarantees that writer is complete; creation zeroes
	//          it so the first frames seed nothing). One buffer per flight
	//          is enough because the parity alternates between the
	//          flight's consecutive users.
	//   cullState — draw-cull verdict words + the filter's atomic
	//          filtered-offset counter (word kCullStateCounterWord).
	Holder<BufferHandle> keptBuffers[MAX_FRAMES_IN_FLIGHT];
	Holder<BufferHandle> cullStateBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle keptLogicals[MAX_FRAMES_IN_FLIGHT]{};
	LogicalBufferHandle cullStateLogicals[MAX_FRAMES_IN_FLIGHT]{};
	// Filtered-index scratch lives in the static index monoblock (one index
	// VkBuffer for mesh + cluster-ordered + filtered regions; firstIndex is
	// absolute, so the index binding never switches). Per (view, flight)
	// persistent regions, allocated on slot creation, freed on teardown.
	// Base in uint32 indices; allocBits is the VMA virtual-allocation
	// bit pattern (VmaVirtualAllocation is a uint64_t handle; this header
	// stays VMA-free, see the static_assert in gfx_filter.cpp).
	uint32_t filteredBase[MAX_FRAMES_IN_FLIGHT] = {};
	uint64_t filteredAllocBits[MAX_FRAMES_IN_FLIGHT] = {};
};

// GPU mirror of one surviving (cluster, draw) pair ("visible meshlet").
// Must match HLSL GfxVisible::vis elements (8 bytes).
struct VisibleCluster {
	uint32_t cluster = 0;
	uint32_t draw = 0;
};
static_assert(sizeof(VisibleCluster) == 8);

// Hierarchical-Z store for one view: MIN-reduced depth pyramid (reversed-Z
// conservative, see hzb.hlsli) in a Storage float buffer with manual mip
// layout (BDA style, no storage images / samplers needed). Mip 0 mirrors
// the depth texture 1:1; mip k dims are max(1, ceil(dim / 2^k)) - i.e.
// repeated dst = max((src + 1u) >> 1, 1u), clamped 2x2 covers odd edges.
static constexpr uint32_t kHizMaxMips = 16;
struct HizResources {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mipCount = 0;
	uint32_t mipOffsets[kHizMaxMips] = {}; // float offsets into buffer
	Holder<BufferHandle> buffer;
	LogicalBufferHandle logical{};
};
// Pure-CPU HZB layout (headless-testable). Mip dims clamp at 1x1; count
// stops when both dims are 1 (capped at kHizMaxMips).
uint32_t HzbMipCount(uint32_t w, uint32_t h);
void HzbMipDims(uint32_t w, uint32_t h, uint32_t mip, uint32_t& outW, uint32_t& outH);
uint32_t HzbMipOffsetFloats(uint32_t w, uint32_t h, uint32_t mip);
uint32_t HzbTotalFloats(uint32_t w, uint32_t h);
// Conservative 2x2 reduction for reversed-Z (MIN: one sky pixel poisons
// the texel to 0 -> keep; see hzb.hlsli derivation).
inline float HzbReduce2x2(float a, float b, float c, float d) {
	return std::min(std::min(a, b), std::min(c, d));
}

// Per-cluster screen-space footprint (T4). Pure-CPU mirror of the filter
// CS math (same formulas, same conservatism); the shader duplicates them
// in HLSL (see gfxfilter.cs.hlsl ClusterOcclusion).
struct ClusterFootprint {
	float centerX = 0.0f, centerY = 0.0f; // px, framebuffer y-down
	float radiusX = 0.0f, radiusY = 0.0f; // px semi-axes (conservative)
	float nearestDepth = 0.0f; // reversed-Z value of the nearest point
	bool behindCamera = true; // set when untestable (eye inside / w <= 0)
};
// Projects a world-space bounding sphere. viewProj is the Y-flipped
// main-view matrix; width/height is the HZB target extent. The footprint
// is the conservative clip-interval projection (radii from viewProj row
// lengths, exact under any rotation); the shader widens it by the jitter
// guard, the audit adds +1px to match (see GfxRenderer::DumpIndirectAudit).
ClusterFootprint ProjectClusterFootprint(const glm::vec3& worldCenter,
	float worldRadius, const glm::vec3& eye, const glm::mat4& viewProj,
	uint32_t width, uint32_t height);
// Mip whose texel covers a rectW x rectH px footprint (ceil(log2(max)),
// clamped to [0, mips); caller guarantees mips > 0.
uint32_t HzbMipForRect(float rectW, float rectH, uint32_t mips);
// Conservative occlusion test over an explicit HZB image (row-major
// floats in HzbMipOffsetFloats layout, MIN-reduced). True = provably
// hidden (nearest point behind every covered texel). Rects missing the
// target, behind-camera footprints and empty pyramids keep (false).
// outBindingStored (optional): the MIN stored depth over the covered
// texels — the value the cull decision hangs on (audit/debug).
bool HzbOccluded(const float* hzb, uint32_t w, uint32_t h, uint32_t mips,
	const ClusterFootprint& f, float* outBindingStored = nullptr);

// Compute push. Must match HLSL HzbPush in hzb.hlsli (48 bytes).
// srcMip = kHizSrcDepth copies mip 0 from the depth texture instead.
static constexpr uint32_t kHizSrcDepth = 0xFFFFFFFFu;
struct HzbPush {
	uint64_t hzb = 0;
	uint32_t srcMip = kHizSrcDepth;
	uint32_t dstMip = 0;
	uint32_t srcWidth = 0;
	uint32_t srcHeight = 0;
	uint32_t dstWidth = 0;
	uint32_t dstHeight = 0;
	uint32_t srcOffset = 0; // floats (ignored for depth source)
	uint32_t dstOffset = 0; // floats
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t pad = 0;
};
static_assert(sizeof(HzbPush) == 48);

// (cluster, model) identity reduced to one u64 sort key: 32-bit bitwise
// hash of the 16 model-matrix words in the high half, cluster id in the
// low half. Hashing raw bits is exactly consistent with memcmp equality
// (-0.0 vs +0.0 and NaN payloads included); MergeKeyMasks verifies key
// ties with a real compare, so hash collisions cannot change semantics.
// This replaces the old 72-byte VisKey struct + memcmp comparator: the
// seed/remainder set ops become one std::sort over u64s and one linear
// merge instead of two struct sorts plus ~2x batchCount cache-hostile
// binary searches per frame.
namespace detail {

struct VisKeyEntry {
	uint64_t key = 0;
	uint32_t batch = 0; // ORIGINAL slot in the batch list it was built from
};

[[nodiscard]] inline uint64_t MakeVisKey(const glm::mat4& model, uint32_t cluster) {
	uint32_t words[16];
	std::memcpy(words, &model, sizeof(words));
	uint32_t h = 0x9e3779b9u;
	for (const uint32_t w : words)
		h = (h ^ w) * 0x85ebca6bu;
	h ^= h >> 16; h *= 0x7feb352du;
	h ^= h >> 15; h *= 0x846ca68bu;
	h ^= h >> 16;
	return (uint64_t(h) << 32) | cluster;
}

// Linear merge of two key-sorted entry arrays (ascending key) into
// membership masks indexed by each entry's original batch slot:
//   outSeed[prev.batch]      = 1 when a bit-identical model matches in curr
//   outRemainder[curr.batch] = 1 when no bit-identical match exists in prev
// modelEq(prevBatch, currBatch) resolves key ties (hash collisions) with
// the exact memcmp semantics — also the seam unit tests use to force
// worst-case tie groups. O(prev + curr); tie groups are duplicates only.
template <typename ModelEq>
void MergeKeyMasks(const VisKeyEntry* prev, size_t prevCount,
	const VisKeyEntry* curr, size_t currCount, ModelEq modelEq,
	uint8_t* outSeed, uint8_t* outRemainder) {
	size_t i = 0, j = 0;
	while (i < prevCount && j < currCount) {
		if (prev[i].key < curr[j].key) {
			outSeed[prev[i++].batch] = 0;
		} else if (prev[i].key > curr[j].key) {
			outRemainder[curr[j++].batch] = 1;
		} else {
			// Tie group: every entry on both sides shares this key. Each
			// entry matches iff ANY cross pair has a bit-identical model
			// (the old binary_search "exists" semantics, duplicates
			// included).
			size_t i2 = i;
			while (i2 < prevCount && prev[i2].key == prev[i].key) ++i2;
			size_t j2 = j;
			while (j2 < currCount && curr[j2].key == curr[j].key) ++j2;
			for (size_t a = i; a < i2; ++a) {
				bool matched = false;
				for (size_t b = j; b < j2 && !matched; ++b)
					matched = modelEq(prev[a].batch, curr[b].batch);
				outSeed[prev[a].batch] = matched ? 1 : 0;
			}
			for (size_t b = j; b < j2; ++b) {
				bool matched = false;
				for (size_t a = i; a < i2 && !matched; ++a)
					matched = modelEq(prev[a].batch, curr[b].batch);
				outRemainder[curr[b].batch] = matched ? 0 : 1;
			}
			i = i2;
			j = j2;
		}
	}
	for (; i < prevCount; ++i)
		outSeed[prev[i].batch] = 0;
	for (; j < currCount; ++j)
		outRemainder[curr[j].batch] = 1;
}

// One bucket-major emitter input: resolved (cluster, draw, material, set).
struct EmitEntry {
	uint32_t cluster = 0;
	uint32_t draw = 0; // firstInstance: index into the draws array the VS reads
	uint32_t material = UINT32_MAX;
	uint32_t drawSet = DrawSetBit_Opaque;
};

} // namespace detail

// Per-view temporal visibility state (Alan Wake 2 style). Deliberately NOT
// part of RenderView: that struct is HLSL-mirrored (256 bytes,
// static_asserted), so history lives in this sidecar, parallel to views.
// prev* is last frame's surviving set (seed source for T3); curr* is this
// frame's (rotated into prev* by SnapshotVisible, called per frame in T5).
struct TemporalView {
	bool historyValid = false; // false until one full visibility frame lands
	float minimumContribution = 0.0f; // px-area threshold, T4 (0 = disabled)
	uint32_t drawSetMask = DrawSetMask_All;
	uint32_t width = 0, height = 0; // extent the history was built for
	// HZB pyramid, ringed per frame in flight (same rule as View buffers
	// and the vis/seed/remainder mirrors: frames overlap, uploads flush
	// immediately, and a single shared pyramid would be clobbered by the
	// next frame while the current one still reads it).
	HizResources hiz[MAX_FRAMES_IN_FLIGHT];
	// CPU snapshot (headless-testable; GPU mirror below is uploaded by
	// UploadVisible into the frameIndex ring slice).
	std::vector<Batch> prevBatches; // surviving (cluster, draw) pairs
	std::vector<Draw> prevDraws; // draw constants they index into
	std::vector<MaterialRange> prevRanges; // material buckets over prevBatches
	uint32_t prevBatchCount = 0;
	uint32_t prevDrawCount = 0;
	// Key-sorted (cluster, model) identities (detail::MakeVisKey).
	// prevKeys mirrors prevBatches whenever historyValid (stamped by
	// SnapshotVisible); currKeys is this frame's v.batches scratch, gated
	// by currKeysSerial == View::batchesSerial so a stale sort can never
	// be merged against mutated batches (hand-built serial 0 never
	// matches: the memo only holds stamped serials).
	std::vector<detail::VisKeyEntry> prevKeys;
	std::vector<detail::VisKeyEntry> currKeys;
	uint64_t currKeysSerial = UINT64_MAX; // UINT64_MAX = never built
	// Per-frame seed/remainder build scratch (capacity retained: the
	// steady-state rebuild allocates nothing). Masks are indexed by the
	// ORIGINAL batch slots; emitScratch feeds EmitBucketMajor in the
	// original batch order so (bucket, material) ranges stay coalesced.
	std::vector<uint8_t> seedMaskScratch;
	std::vector<uint8_t> remainderMaskScratch;
	std::vector<uint32_t> batchMaterialScratch;
	std::vector<detail::EmitEntry> emitScratch;
	// UploadVisible compaction scratch (prev batch pairs for the ring slice).
	std::vector<VisibleCluster> visPairScratch;
	// GPU mirror, ringed per frame in flight (same rule as View buffers:
	// frames overlap, uploads flush immediately).
	Holder<BufferHandle> visClusterBuffers[MAX_FRAMES_IN_FLIGHT];
	Holder<BufferHandle> visDrawBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle visClusterLogicals[MAX_FRAMES_IN_FLIGHT]{};
	LogicalBufferHandle visDrawLogicals[MAX_FRAMES_IN_FLIGHT]{};
	// Seed command list (T3): one indirect draw per prevBatch, reordered
	// bucket-major (opaque runs first) with per-(bucket, material) ranges.
	// Consumed by the seed depth pass; rebuilt from the snapshot each frame.
	std::vector<SeedCommand> seedCommands;
	std::vector<SeedRange> seedRanges;
	uint32_t seedOpaqueCommands = 0; // leading commands using the opaque PSO
	Holder<BufferHandle> seedIndirectBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle seedIndirectLogicals[MAX_FRAMES_IN_FLIGHT]{};
	// Remainder command list (T5): currVisible minus prevVisible, keyed by
	// (cluster, model matrix). Drawn by depth pass 2 after the filter so
	// newly exposed geometry lands in depth (and the HZB refresh) the same
	// frame it becomes visible — no one-frame holes.
	std::vector<SeedCommand> remainderCommands;
	std::vector<SeedRange> remainderRanges;
	uint32_t remainderOpaqueCommands = 0;
	Holder<BufferHandle> remainderIndirectBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle remainderIndirectLogicals[MAX_FRAMES_IN_FLIGHT]{};
};

// ---------------------------------------------------------------------------
// CPU-built shadow draw lists (cascade + spot tiles)
// ---------------------------------------------------------------------------

// Per-view shadow list: one indirect command per surviving (draw x
// cluster), bucket-major (opaque first, then per-material masked), same
// recording shape as the seed/remainder lists — but built from THIS
// frame's candidates with no temporal state and no GPU filter dispatch.
// The VS (gfxshadowseed*.vert.hlsl) resolves model matrices from the
// draws ring slice via BDA and the view-projection from the render-view
// slab, so the pass recording is one multi-draw per range.
//
// Threading contract: PushShadowDraw/BuildShadowList are designed to run
// one view per worker thread (each view owns its vectors; the mesh/
// cluster tables are read-only during the build; uploads ride the calling
// thread's staging shard). Claim ALL views for the frame (BeginShadow*)
// BEFORE dispatching any build: the slot vector must not grow while
// workers hold element references inside BuildShadowList.
struct ShadowView {
	const RenderView* renderView = nullptr; // frustum planes (RenderViewSet-owned)
	// Contribution gate (the main camera's view): non-null when the caller
	// wants the mesh-bound fast discard to drop casters that cannot
	// contribute to the main view (m_shadowContributionCulling's indirect
	// twin — the direct fallback path culls at item level).
	const RenderView* contributionView = nullptr;
	CullFlags cullFlags = CullFlagBit_Frustum;
	uint32_t width = 0, height = 0;
	uint32_t frameIndex = 0; // set by BuildShadowList
	// Per-frame CPU state (capacity retained across frames).
	std::vector<DrawRequest> draws;
	std::vector<Draw> drawConstants; // material-sorted GPU mirror
	std::vector<uint32_t> drawOrderScratch; // material sort permutation
	std::vector<SeedCommand> commands;
	std::vector<SeedRange> ranges;
	uint32_t opaqueCommands = 0;
	Stats stats{};
	// Build scratch (parallel to the emit walk; capacity retained).
	std::vector<detail::EmitEntry> emitScratch;
	// Ringed per-frame GPU mirrors (draws BDA + indirect commands).
	Holder<BufferHandle> drawBuffers[MAX_FRAMES_IN_FLIGHT];
	Holder<BufferHandle> indirectBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle drawLogicals[MAX_FRAMES_IN_FLIGHT]{};
	LogicalBufferHandle indirectLogicals[MAX_FRAMES_IN_FLIGHT]{};
	bool buffersReady = false;
};

struct State {
	// Static (load-time): cluster-ordered global vertex ids + cluster table.
	// Frozen after UploadStaticData, safe to share across frames.
	// The cluster-ordered array lives in the static index monoblock (same
	// VkBuffer as mesh + filtered regions); indexBase is region-relative in
	// Cluster::indexOffset, absolute firstIndex = clusterIndexBase + offset.
	std::vector<uint32_t> filterIndices;
	std::vector<Cluster> clusters;
	std::vector<Mesh> meshes;
	uint32_t clusterIndexBase = 0; // region base in uint32 indices (0 = none)
	uint64_t clusterIndexAllocBits = 0; // VMA virtual-allocation bits (see View)
	// Rendergraph logical for the static index monoblock (all index streams
	// share one buffer, so one hint covers mesh/cluster/filtered reads).
	LogicalBufferHandle indexHeapLogical{};
	Holder<BufferHandle> clusterBuffer;
	LogicalBufferHandle clusterLogical{};
	// Static per-mesh cluster ranges (GPU mirror of `meshes`; the GPU cull
	// chain derives each draw group's cluster window from it).
	Holder<BufferHandle> meshesBuffer;
	LogicalBufferHandle meshesLogical{};
	// GPU cull chain (Northlight style, the only path): the CPU seeds
	// the draw list + candidate slot layout, then GfxBatchCull ->
	// GfxSeedDepth -> GfxHiz -> GfxDrawCull -> GfxFilter all run GPU-side
	// with zero mid-pipeline CPU readbacks (the temporal seed/remainder
	// link stays GPU-resident through the kept-bit parity rings on the
	// views).
	uint32_t frameCounter = 0; // BeginFrame ++; keptParity source
	Holder<ShaderModuleHandle> batchCullModule;
	ShaderProgram batchCullProgram{};
	Holder<ShaderModuleHandle> drawCullModule;
	ShaderProgram drawCullProgram{};
	// Draw-level texture-demand source (gfxfilter.cs.hlsl
	// PublishDrawDemand; see the FilterPush tail + the demand API below).
	// meshBounds: per-mesh union bound over the mesh's clusters (xyz
	// center, w radius, OBJECT space), static after UploadStaticData.
	std::vector<glm::vec4> meshBounds;
	Holder<BufferHandle> meshBoundsBuffer;
	LogicalBufferHandle meshBoundsLogical{};
	// textureMeta/epoch/count: per-slot meta table, re-uploaded only when
	// the streamer's epoch bumps (register/unregister only; slots are
	// stable across retargets so retargets never re-upload).
	Holder<BufferHandle> textureMetaBuffer;
	uint64_t textureMetaEpoch = 0;  // streamer epoch last uploaded (0 = none)
	uint32_t textureMetaCount = 0; // live slot count in the uploaded table
	// Demand ring: one uint per bindless slot per flight. SetDemandInputs
	// creates it pre-filled with kDemandNoReport (the first consume before
	// any GPU write observes "no demand", not garbage); ClearDemand
	// re-fills every frame AFTER the consumer's download submit
	// (download-before-clear; the transfer chain orders the fill after any
	// in-flight copy, and the frame's filter writes wait the chain, so a
	// stale min can never pin a texture sharp — the cadence the retired
	// fragment-feedback ring proved).
	Holder<BufferHandle> demandBuffers[MAX_FRAMES_IN_FLIGHT];
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
	// silhouette. One GfxSkinBoundsTable per flight: the kMaxBatchSlots
	// GfxClusterData candidate slots + the kMaxDraws drawBound tail (one
	// buffer, one BDA). Lazily created on the first frame a deformed buffer
	// exists.
	Holder<ShaderModuleHandle> skinBoundsModule;
	ShaderProgram skinBoundsProgram{};
	Holder<BufferHandle> skinBoundsBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle skinBoundsLogicals[MAX_FRAMES_IN_FLIGHT]{};
	bool skinBoundsReady = false;
	// The STATIC heap BDAs for the VS legs' skinned fetch (the same heaps
	// every draw path reads - the RHI globals from vulkan_impl.h, stashed
	// once at EnsureMeshParams so the record sites' fills never reach for
	// the globals themselves). 0 = not stashed yet (static fetches).
	uint64_t vsPosHeapBda = 0;
	uint64_t vsShadeHeapBda = 0;
	Holder<ShaderModuleHandle> filterModule;
	ShaderProgram filterProgram{};
	Holder<ShaderModuleHandle> hizModule;
	ShaderProgram hizProgram{};
	// Task-less mesh path (main-leg opaque only): mesh module + base-lit
	// twin program { mesh, gfxmesh_lit FS }. Per-flight params blob +
	// rendergraph aliases (uploaded per mesh dispatch).
	Holder<ShaderModuleHandle> meshModule;
	Holder<ShaderModuleHandle> meshFsModule;
	ShaderProgram meshProgram{};
	Holder<BufferHandle> meshParamsBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle meshParamsLogicals[MAX_FRAMES_IN_FLIGHT]{};
	// Compacted eligible batch indices, rewritten per mesh dispatch set
	// (one upload per frame covering all variant spans; dispatches slice
	// it via the listBDA base). kMaxBatches uints per flight.
	Holder<BufferHandle> meshBatchLists[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle meshBatchListLogicals[MAX_FRAMES_IN_FLIGHT]{};
	std::vector<View> views;
	std::vector<TemporalView> temporals; // parallel to views (same slot)
	uint32_t viewCount = 0;
	std::vector<ShadowView> shadowViews;
	uint32_t shadowViewCount = 0;
	uint64_t nextBatchesSerial = 1; // batchesSerial source (0 = hand-built)
};

// ---------------------------------------------------------------------------
// Procedural API.
// ---------------------------------------------------------------------------

inline constexpr const char* kGfxFilterCsPath = "content/shaders/gfxfilter.cs.hlsl.spv";
inline constexpr const char* kGfxFilteredVsPath = "content/shaders/gfxfiltered.vert.hlsl.spv";
inline constexpr const char* kHizCsPath = "content/shaders/hzb.cs.hlsl.spv";
inline constexpr const char* kGfxBatchCullCsPath = "content/shaders/gfxbatchcull.cs.hlsl.spv";
inline constexpr const char* kGfxDrawCullCsPath = "content/shaders/gfxdrawcull.cs.hlsl.spv";
inline constexpr const char* kGfxSkinBoundsCsPath = "content/shaders/gfxskinbounds.cs.hlsl.spv";

// Mesh-bound fast-discard gate for shadow lists (pure CPU, testable):
// true = discard the whole draw before the per-cluster meshlet loop.
//   1. the mesh union bound misses the SHADOW view's frustum, or
//   2. contributionView set + the bound misses it (a caster outside the
//      main camera's frustum cannot contribute to it — the indirect-path
//      twin of the direct fallback's item-level contribution cull).
bool CullShadowMeshBound(const glm::vec4& meshBound, const glm::mat4& model,
	const RenderView& shadowView, const RenderView* contributionView);

// ---- GPU cull chain pass shells (caller sets the recording func) ----
// Deformed cluster bounds: runs between the skinning pass and GfxBatchCull.
// One 128-thread workgroup per draw; threads stride the draw's clusters and
// write an object-space bounding sphere (cone degenerated) per candidate
// slot from the frame's deformed heap. No-op when no skinned cluster exists
// (the caller gates on State::skinDeformedBda).
RenderPass& AddSkinBoundsPass(State& s, RenderGraph& rg, uint32_t view);
void PushAndDispatchSkinBounds(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view);
// Creates the per-flight skinBounds ring + module lazily (idempotent).
bool EnsureSkinBounds(State& s, ResourceManager& ctx, RenderGraph& rg);
// First compute pass of the frame (before the seed pass): one workgroup
// per draw; sphere + cone verdicts, temporal seed/remainder commands,
// batch array + filter indirect statics, kept bits.
RenderPass& AddBatchCullPass(State& s, RenderGraph& rg, uint32_t view);
void PushAndDispatchBatchCull(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view);
// After the HZB passes, before the filter: one thread per draw; mesh-bound
// frustum (frozen view) + HZB occlusion verdicts.
RenderPass& AddDrawCullPass(State& s, RenderGraph& rg, uint32_t view);
void PushAndDispatchDrawCull(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view);
// GPU-mode seed/remainder ranges (positional candidate spans, bucket-major;
// identical for both passes — the GPU-written instanceCounts gate draws).
[[nodiscard]] const std::vector<SeedRange>& CullRanges(const State& s, uint32_t view);
[[nodiscard]] uint32_t CullOpaqueCommands(const State& s, uint32_t view);

bool EnsureShaders(State& s, ResourceManager& ctx,
	const char* csPath = kGfxFilterCsPath);
// Task-less mesh path ring (per-flight 152B params blob + logicals).
// Best-effort: failure only disables the mesh record (indexed stays).
bool EnsureMeshParams(State& s, ResourceManager& ctx, RenderGraph& rg);
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
uint64_t UploadMeshParams(State& s, ResourceManager& ctx, uint32_t view,
	uint64_t listBDA, uint64_t lmXformsAddr, const glm::mat4& liveViewProj);
// One variant dispatch: bind mesh pipeline, push tails, DrawMeshTasks.
// Params must already be uploaded (UploadMeshParams); listBase/listCount
// slice this dispatch out of the shared compacted list.
void RecordMeshVariant(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view, ShaderProgram& meshProg, uint64_t paramsBDA,
	uint32_t listBase, uint32_t listCount,
	uint64_t renderViewsAddr, uint32_t renderViewIndex, uint64_t lmXformsAddr,
	const DepthSettings& depth, const MSAASettings& msaa);
// Creates shader only; per-(view, frame) buffers are allocated on demand
// by BeginView (pool) so unused views cost nothing.
bool Ensure(State& s, ResourceManager& ctx, RenderGraph& rg,
	const char* csPath = kGfxFilterCsPath);
void Destroy(State& s, RenderGraph& rg);
[[nodiscard]] inline bool Valid(const State& s) {
	return s.filterProgram.shaderCount != 0;
}

// Mesh registration (load-time). baseVertex maps upload/cooked-local vertex
// ids to heap-global ids. FromUpload cooks meshlets via meshoptimizer
// (falls back to <=128-tri chunks with sphere bounds when cooking fails);
// FromCooked reuses persisted CookedMeshlets (pak path, no CPU verts needed).
// Returns the mesh index for PushDraw, UINT32_MAX on failure.
uint32_t RegisterMeshFromUpload(State& s, const StaticMeshUpload& upload,
	uint32_t baseVertex);
uint32_t RegisterMeshFromCooked(State& s, const CookedMeshlets& cooked,
	uint32_t baseVertex);
// Bakes filterIndices/clusters into GPU buffers (call once after all
// Register* calls). Re-uploads on every call; load-time only.
void UploadStaticData(State& s, ResourceManager& ctx, RenderGraph& rg);
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
uint32_t BeginView(State& s, ResourceManager& ctx, RenderGraph& rg,
	const RenderView& renderView, const glm::vec3& eye,
	CullFlags cullFlags, uint32_t width, uint32_t height);
void PushDraw(State& s, uint32_t view, uint32_t mesh,
	const glm::mat4& model, uint32_t material, DrawFlags flags = 0,
	DrawSetMask drawSet = DrawSetBit_Opaque,
	uint32_t lmChunk = 0xFFFFFFFFu, uint32_t lmChartOffset = 0);
// Pure-CPU cluster test (meshoptimizer perspective cone + sphere vs planes).
// Exported for unit tests; eye must be the camera world position.
// The normal-cone (backface) and sphere-vs-frustum parts always run. The
// cone test runs in world space (apex via model, axis via inverse-transpose)
// so mirrored / non-uniform instances cull the same side the rasterizer keeps.
bool CullCluster(const Cluster& c, const glm::mat4& model,
	const glm::mat4& invModel, const glm::vec3& eye,
	const RenderView& renderView);
// Cone fit result (the object-space fields feed Cluster's cone tail exactly):
// degenerate = true keeps the keep-cone (axis 0 -> GfxConeCulled's len2 <
// 1e-12 keep; the value the cluster fields must hold in that case).
struct DeformedConeFit {
	glm::vec3 coneApex{ 0.0f };
	glm::vec3 coneAxis{ 0.0f }; // 0 = degenerate (the keep-cone)
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
	uint32_t triCount, const glm::vec3& sphereCenter);
// Pure-CPU batch expansion: sorts draws by material, expands every
// (draw x cluster) candidate (no pre-cull — gfxbatchcull owns verdicts),
// fills batches/drawConstants/materialRanges/stats. No GPU upload (called
// by FinalizeView; unit tests drive it directly on a hand-built View).
void BuildBatches(State& s, uint32_t view);
// Sorts draws by material, expands every (draw x cluster) candidate,
// uploads the draws ring slice into the frameIndex slot (everything else
// is GPU-written by the cull chain). Returns exact counters
// (trianglesProcessed counts emitted triangles).
Stats FinalizeView(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view);

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
// Drops history (first frame, extent drift, camera teleport, quality
// change). Buffers are kept; the next frame rebuilds from the full path.
void InvalidateHistory(State& s, uint32_t view);
// CPU snapshot of this frame's surviving set into the temporal sidecar
// (overwrites; pure CPU, headless-testable). T5 calls this per frame after
// FinalizeView; UploadVisible mirrors it to the GPU ring slice.
void SnapshotVisible(State& s, uint32_t view);
// Uploads the snapshot (prevBatches as compact VisibleCluster pairs +
// prevDraws) into the frameIndex ring slice. No-op when empty/unallocated.
void UploadVisible(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view);
// Builds the seed command list from the snapshot intersected with the
// current view's batches (same cluster + same transform only — a stale
// prev-LOD sibling or moved instance would otherwise replay nearer depth
// that the read-only Main fails against; see BuildSeedCommands). One
// indirect draw per surviving prevBatch, reordered bucket-major (opaque
// first) with per-(bucket, material) ranges (pure CPU, headless-testable).
// Bucket comes from each draw's drawSet (set at PushDraw from the material).
void BuildSeedCommands(State& s, uint32_t view);
// Fused steady-state path: one sorted-key merge builds BOTH lists (sorts
// the current batches' (cluster, model) keys once, merges them against
// the snapshot's cached prevKeys). Output identical to running the two
// builders back to back; AppendPasses uses this one (pure CPU,
// headless-testable).
void BuildSeedAndRemainder(State& s, uint32_t view);
// Builds the remainder list: current batches minus the snapshot, keyed by
// (cluster, model matrix), same bucket-major layout (pure CPU,
// headless-testable). Must run before SnapshotVisible overwrites prev*.
void BuildRemainderCommands(State& s, uint32_t view);
// Uploads the seed commands into the frameIndex ring slice. No-op when
// empty/unallocated.
void UploadSeed(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view);
// Uploads the remainder commands. No-op when empty/unallocated.
void UploadRemainder(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view);
// Depth-seed pass shell over the render-depth target (caller sets the
// recording func to PushSeedConstants + DrawSeedRange per range).
RenderPass& AddSeedPass(State& s, RenderGraph& rg, uint32_t view,
	LogicalImageViewHandle depthLogical);
// Depth pass 2 shell (T5): draws the remainder list over seeded depth
// (Load/Store). Same record pattern with the remainder ranges/buffer.
RenderPass& AddRemainderPass(State& s, RenderGraph& rg, uint32_t view,
	LogicalImageViewHandle depthLogical);
// Consume-side barrier helper for the seed depth pass: cluster-ordered
// index heap + vis draws + seed indirect as vertex-stage reads.
void AddSeedConsumeHints(RenderPass& pass,
	LogicalBufferHandle indexLogical, LogicalBufferHandle visDrawLogical,
	LogicalBufferHandle seedIndirectLogical);

// ---- CPU-built shadow draw lists (cascade + spot tiles) --------------
// Resets the per-frame shadow slot cursor (slots persist; buffers are
// created lazily by BeginShadowView on first claim of each slot).
void BeginShadowFrame(State& s);
// Claims the next shadow slot bound to an allocated RenderView (frustum
// planes). contributionView: the main camera's view for the mesh-bound
// contribution fast discard (null = no contribution culling). Main
// thread only: the slot vector grows here, so claim every view of the
// frame before dispatching BuildShadowList tasks. Returns the slot,
// UINT32_MAX on failure.
uint32_t BeginShadowView(State& s, ResourceManager& ctx, RenderGraph& rg,
	const RenderView& renderView, CullFlags cullFlags,
	uint32_t width, uint32_t height,
	const RenderView* contributionView = nullptr);
// Pushes one caster candidate (mesh = REGISTERED gfx mesh index from
// RegisterMeshFrom*; UINT32_MAX casters stay on the caller's direct
// fallback). Thread-safe per view: distinct slots only, no shared
// mutable state besides the read-only mesh/cluster tables.
void PushShadowDraw(State& s, uint32_t sview, uint32_t mesh,
	const glm::mat4& model, uint32_t material, DrawFlags flags = 0,
	DrawSetMask drawSet = DrawSetBit_Opaque);
// Sorts the view's draws by material, expands draw x cluster with the
// per-cluster sphere test against the RenderView planes (the normal-cone
// test is deliberately skipped: directional cascade views have no valid
// eye position), emits bucket-major commands (opaque VS-only phase
// first, then per-material masked ranges). Pure CPU, no GPU access:
// safe to run one view per worker thread.
void BuildShadowList(State& s, uint32_t sview);
// Uploads the built lists into the frameIndex ring slice (draws as VS
// shader reads, commands as draw-indirect reads). Main thread only: the
// global buffer sync-state tables are sharded by handle, not by thread,
// so concurrent uploads on same-shard buffers would race.
void UploadShadowList(State& s, ResourceManager& ctx,
	uint32_t frameIndex, uint32_t sview);
// Accessors (frame slice resolved from the view's frameIndex, stamped by
// BuildShadowList).
[[nodiscard]] const std::vector<SeedRange>& ShadowRanges(
	const State& s, uint32_t sview);
[[nodiscard]] uint32_t ShadowOpaqueCommands(const State& s, uint32_t sview);
[[nodiscard]] BufferHandle ShadowDrawBuffer(const State& s, uint32_t sview);
[[nodiscard]] BufferHandle ShadowIndirectBuffer(const State& s, uint32_t sview);
[[nodiscard]] LogicalBufferHandle ShadowDrawLogical(const State& s, uint32_t sview);
[[nodiscard]] LogicalBufferHandle ShadowIndirectLogical(const State& s, uint32_t sview);
// CPU-side draw-constants mirror (diagnostics; the GPU reads the uploaded
// ring slice).
[[nodiscard]] const std::vector<Draw>& ShadowDrawConstants(
	const State& s, uint32_t sview);

// ---- Hierarchical-Z (T2 build, T4 consumer) ----
bool EnsureHizShaders(State& s, ResourceManager& ctx,
	const char* csPath = kHizCsPath);
// Cull chain programs (best effort: failure skips the chain's passes
// loudly at record time — AppendPasses gates on the programs).
bool EnsureBatchCullShaders(State& s, ResourceManager& ctx,
	const char* csPath = kGfxBatchCullCsPath);
bool EnsureDrawCullShaders(State& s, ResourceManager& ctx,
	const char* csPath = kGfxDrawCullCsPath);
// (Re)creates the view's HZB buffer for w/h (frees the old logical).
// Resets historyValid when the size drifts (stale seed geometry extent).
// Returns false on zero extent or allocation failure (history invalid).
bool EnsureHiz(State& s, ResourceManager& ctx, RenderGraph& rg,
	uint32_t view, uint32_t w, uint32_t h);
// Compute pass shell for one HZB level (caller sets the recording func
// to PushAndDispatchHiz). dstMip 0 additionally reads depthLogical.
// passName labels the node in the framegraph; the post-remainder refresh
// passes "GfxHizRefresh" so the two builds are distinguishable.
RenderPass& AddHizPass(State& s, RenderGraph& rg, uint32_t view,
	uint32_t dstMip, LogicalImageViewHandle depthLogical,
	const char* passName = "GfxHiz");
// Recording helper: pushes HzbPush for dstMip and dispatches the level.
// depthBindlessIndex is consumed only for dstMip 0 (UINT32_MAX otherwise).
void PushAndDispatchHiz(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view, uint32_t dstMip, uint32_t depthBindlessIndex = UINT32_MAX);
// BDA of the view's HZB buffer (0 when unallocated; T4 skips occlusion then).
uint64_t HizAddress(State& s, ResourceManager& ctx, uint32_t view);
// DEBUG: Download and print HZB buffer contents for inspection.
void DumpHzb(State& s, ResourceManager& ctx, uint32_t view);

// ---- Pass shells (caller sets the recording func) ----
RenderPass& AddFilterPass(State& s, RenderGraph& rg, uint32_t view);
void AddConsumeHints(RenderPass& pass,
	LogicalBufferHandle filteredLogical, LogicalBufferHandle indirectLogical);
// Recording helper: pushes FilterPush and dispatches batchCount. No-op when empty.
void PushAndDispatch(State& s, CommandBuffer& cmd, ResourceManager& ctx, uint32_t view);

// ---- Draw-level texture demand (designated-publisher source) ----
// Per-frame demand inputs (app-resolved): THIS frame's materials BDA
// slice + the streamer's mip bias. Idempotent; call once per frame before
// AppendPasses records the filter (PushAndDispatch reads them). Creates
// the demand ring pre-filled with kDemandNoReport on first use.
void SetDemandInputs(State& s, ResourceManager& ctx,
	uint64_t materialsBda, float mipBias);
// Uploads the per-slot meta table when the streamer's epoch changes.
// Returns false on buffer/upload failure (retry next frame — the cached
// epoch stays stale so the call re-attempts).
bool UploadTextureMeta(State& s, ResourceManager& ctx, uint64_t epoch,
	const TextureMeta* meta, size_t count);
// Demand ring slice for the readback consumer (empty handle when the ring
// was never created). Slots hold wanted FULL-CHAIN mips;
// kDemandNoReport = no draw reported the texture this frame.
[[nodiscard]] inline BufferHandle DemandBuffer(
		const State& s, uint32_t frameIndex) {
	assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
	return s.demandBuffers[frameIndex];
}
// GPU cull-state slice for the readback consumer (empty handle when the
// GPU cull never ran): word kCullStateCounterWord holds that dispatch
// chain's final KEPT-entry count (the filter's atomic filtered-offset
// counter - zeroed by the batch-cull dispatch, accumulated per kept
// entry), i.e. the GPU-side cull survivor count the CPU stats cannot see.
// MAIN view (views[0] - the filter's view 0; shadow legs keep their own).
[[nodiscard]] inline BufferHandle CullStateBuffer(
		const State& s, uint32_t frameIndex) {
	assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
	if (s.views.empty())
		return BufferHandle{};
	return s.views[0].cullStateBuffers[frameIndex];
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
void ClearDemand(State& s, ResourceManager& ctx, uint32_t frameIndex);

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
[[nodiscard]] inline BufferHandle BatchBuffer(const State& s, uint32_t view) {
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
	tail.paramsValid = (s.skinDeformedBda != 0
		&& s.skinDeformedBase != UINT32_MAX) ? 1u : 0u;
}

// Motion-leg PREV tail (gfxfilteredmotion.vert.hlsl bytes 240..256, the
// free prevModel scratch's last 16B — the cur pair at 192..240 arrives for
// free via PushViewConstants inside PushMotionViewConstants: the motion VS
// re-declares the FilteredVsPush fields it reads instead of padding over
// them). Mirrors the HLSL FilteredMotionPush's prev fields exactly.
static constexpr uint32_t kFilteredMotionPrevTailOffset = 240;
struct FilteredMotionPrevTail {
	uint64_t vsPrevDeformed = 0; // the PREV parity half (last frame's pose)
	uint32_t vsPrevValid = 0;     // 1 = the prev half holds last frame's pose
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

[[nodiscard]] inline BufferHandle DrawBuffer(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].drawBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline BufferHandle IndirectBuffer(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].indirectBuffers[s.views[view].frameIndex];
}
// Static index monoblock accessor (mesh + cluster-ordered + filtered index
// regions share one VkBuffer; draws use absolute firstIndex, offset-0 bind).
[[nodiscard]] inline LogicalBufferHandle IndexHeapLogical(const State& s) {
	return s.indexHeapLogical;
}
[[nodiscard]] inline uint32_t ClusterIndexBase(const State& s) {
	return s.clusterIndexBase;
}
[[nodiscard]] inline uint32_t FilteredIndexBase(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].filteredBase[s.views[view].frameIndex];
}
[[nodiscard]] inline BufferHandle SeedIndirectBuffer(const State& s, uint32_t view) {
	assert(view < s.temporals.size());
	return s.temporals[view].seedIndirectBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline LogicalBufferHandle SeedIndirectLogical(const State& s, uint32_t view) {
	assert(view < s.temporals.size());
	return s.temporals[view].seedIndirectLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline BufferHandle VisDrawBuffer(const State& s, uint32_t view) {
	assert(view < s.temporals.size());
	return s.temporals[view].visDrawBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline LogicalBufferHandle VisDrawLogical(const State& s, uint32_t view) {
	assert(view < s.temporals.size());
	return s.temporals[view].visDrawLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline const std::vector<SeedRange>& SeedRanges(
	const State& s, uint32_t view) {
	// The positional candidate ranges (the GPU-written instanceCounts
	// gate the actual draws; firstInstance indexes the current draws).
	// Identical list for seed and remainder.
	assert(view < s.views.size());
	return s.views[view].cullRanges;
}
[[nodiscard]] inline uint32_t SeedOpaqueCommands(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].cullOpaqueCommands;
}
[[nodiscard]] inline BufferHandle RemainderIndirectBuffer(const State& s, uint32_t view) {
	assert(view < s.temporals.size());
	return s.temporals[view].remainderIndirectBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline LogicalBufferHandle RemainderIndirectLogical(const State& s, uint32_t view) {
	assert(view < s.temporals.size());
	return s.temporals[view].remainderIndirectLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline const std::vector<SeedRange>& RemainderRanges(
	const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].cullRanges;
}
[[nodiscard]] inline uint32_t RemainderOpaqueCommands(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].cullOpaqueCommands;
}
[[nodiscard]] inline LogicalBufferHandle BatchLogical(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].batchLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline LogicalBufferHandle DrawLogical(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].drawLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline LogicalBufferHandle IndirectLogical(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].indirectLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline uint32_t BatchCount(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].batchCount;
}
[[nodiscard]] inline const std::vector<MaterialRange>& MaterialRanges(
	const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].materialRanges;
}
[[nodiscard]] inline uint32_t ViewCount(const State& s) { return s.viewCount; }

} // namespace gfx
} // namespace smvk
