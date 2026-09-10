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
// meshes: the CPU pre-culls per (draw x cluster) with sphere + normal-cone
// tests (meshoptimizer bounds, perspective formulas from meshoptimizer.h),
// and the GPU compacts one workgroup per surviving cluster. One indirect
// command per surviving (draw x cluster); commands are sorted by material
// so each material draws with a single multi-draw range.
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
#include <cstdint>
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
	// depth pyramid. Facing-agnostic: applies to two-sided draws too.
	CullFlagBit_HzbOcclusion = 0x40,
	// Contribution (T4): reject clusters whose screen footprint is below
	// TemporalView::minimumContribution px^2 (0 = disabled).
	CullFlagBit_Contribution = 0x80,
	// Selects the opposite homogeneous-winding sign (engine Y-flip).
	// Verify once: closed sphere must cull ~50% via backface alone.
	CullFlagBit_FlipWinding = 0x100,
} CullFlagBits;
typedef uint32_t CullFlags;

// Per-draw flags (stored in Draw::flags / HLSL GfxDraw misc.y).
// TwoSided (double-sided material, raster Cull None): backface and
// normal-cone culling are disabled for the draw (both would otherwise
// delete geometry the rasterizer is supposed to keep). Frustum,
// degenerate and small-prim tests still apply.
inline constexpr uint32_t DrawFlagBit_TwoSided = 0x1;
typedef uint32_t DrawFlags;

// Draw-set buckets: geometry grouped by properties that affect
// rasterization (Alan Wake 2 style). The seed depth pass (T3) issues one
// PSO per bucket (opaque single-sided vs. alpha-tested double-sided);
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
static constexpr uint32_t kMaxDraws = 4096;   // draws pushed per view
// Sized for LOD chains: every cooked sibling registers its own gfx mesh
// (~3x the LOD0-only count), so headroom over the pre-LOD 4k budget.
static constexpr uint32_t kMaxMeshes = 8192;
static constexpr uint32_t kMaxViews = 64; // mirrors kMaxRenderViews

// GPU cluster entry (2x uvec4). Must match HLSL GfxClusters (32 bytes):
// (indexOffset, triCount, 0, 0) + (boundCenter.xyz, boundRadius) for the
// per-cluster HZB occlusion / contribution tests (T4).
struct ClusterConstants {
	uint32_t indexOffset = 0; // into the cluster-ordered index buffer (uints)
	uint32_t triCount = 0;
	uint32_t pad0[2] = {};
	glm::vec3 boundCenter{ 0.0f };
	float boundRadius = 0.0f;
};
static_assert(sizeof(ClusterConstants) == 32);

// Per-draw frame entry. Must match HLSL GfxDraw (80 bytes).
struct alignas(16) Draw {
	glm::mat4 model{};
	uint32_t mesh = 0;
	uint32_t flags = 0; // DrawFlags (bit 0 = two-sided: skip backface/cone cull)
	uint32_t drawSet = DrawSetBit_Opaque; // DrawSetBits bucket (seed PSO select)
	uint32_t pad = 0;
};
static_assert(sizeof(Draw) == 80);

// GPU batch entry (1x uvec4): cluster + draw + reserved output slot.
struct Batch {
	uint32_t cluster = 0;
	uint32_t draw = 0;
	uint32_t outputOffset = 0; // in uints; batchSlot * kMaxClusterTris * 3
	uint32_t pad = 0;
};
static_assert(sizeof(Batch) == 16);

// Compute push. Must match HLSL GfxFilterPush in gfxfilter.hlsli (192 bytes).
struct FilterPush {
	uint64_t positions = 0;
	uint64_t indices = 0; // cluster-ordered index buffer (owned)
	uint64_t clusters = 0;
	uint64_t batches = 0;
	uint64_t draws = 0;
	uint64_t filtered = 0;
	uint64_t indirect = 0;
	uint32_t pad0[2] = {}; // HLSL uint2 (raw uint64 would emit Capability Int64)
	glm::mat4 viewProj{};
	glm::vec3 eye{ 0.0f };
	float pad1 = 0.0f;
	uint32_t cullFlags = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t batchCount = 0;
	uint32_t pad2[2] = {};
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
};
static_assert(sizeof(FilterPush) == 200);

// Filtered-path VS tail push (gfxfiltered.vert.hlsl, 24 bytes).
// Pushed at kFilteredVsPushOffset (the main pass' unused prevModel scratch,
// bytes 192..216) so the full 256B scene push stays intact for the fragment
// shader. The VS resolves viewProj through the render-view slab.
static constexpr uint32_t kFilteredVsPushOffset = 192;
struct FilteredVsPush {
	uint64_t draws = 0;
	uint64_t renderViews = 0;
	uint32_t renderViewIndex = 0;
	uint32_t pad = 0;
};
static_assert(sizeof(FilteredVsPush) == 24);

// One indirect command range per material (commands sorted by material).
struct MaterialRange {
	uint32_t material = 0;
	uint32_t firstCommand = 0;
	uint32_t commandCount = 0;
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
	Stats stats{};
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

// One indirect draw over the cluster-ordered index heap (20 bytes, matches
// VkDrawIndexedIndirectCommand; firstInstance indexes the visDraws snapshot).
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
	Holder<ShaderModuleHandle> filterModule;
	ShaderProgram filterProgram{};
	Holder<ShaderModuleHandle> hizModule;
	ShaderProgram hizProgram{};
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

bool EnsureShaders(State& s, ResourceManager& ctx,
	const char* csPath = kGfxFilterCsPath);
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
	DrawSetMask drawSet = DrawSetBit_Opaque);
// Pure-CPU cluster test (meshoptimizer perspective cone + sphere vs planes).
// Exported for unit tests; eye must be the camera world position.
// twoSided skips the normal-cone (backface) part; the sphere-vs-frustum
// part always runs. The cone test runs in world space (apex via model,
// axis via inverse-transpose) so mirrored / non-uniform instances cull
// the same side the rasterizer keeps.
bool CullCluster(const Cluster& c, const glm::mat4& model,
	const glm::mat4& invModel, const glm::vec3& eye,
	const RenderView& renderView, bool twoSided = false);
// Pure-CPU batch expansion: sorts draws by material, expands surviving
// (draw x cluster) with CullCluster, fills batches/drawConstants/
// materialRanges/stats. No GPU upload (called by FinalizeView; unit tests
// drive it directly on a hand-built View).
void BuildBatches(State& s, uint32_t view);
// Sorts draws by material, expands surviving (draw x cluster) with CPU
// sphere + cone pre-cull, uploads batches/draws/indirect zero-fill into
// the frameIndex ring slice. Returns exact counters (trianglesProcessed
// counts emitted triangles).
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

// ---- Hierarchical-Z (T2 build, T4 consumer) ----
bool EnsureHizShaders(State& s, ResourceManager& ctx,
	const char* csPath = kHizCsPath);
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
	assert(view < s.temporals.size());
	return s.temporals[view].seedRanges;
}
[[nodiscard]] inline uint32_t SeedOpaqueCommands(const State& s, uint32_t view) {
	assert(view < s.temporals.size());
	return s.temporals[view].seedOpaqueCommands;
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
	assert(view < s.temporals.size());
	return s.temporals[view].remainderRanges;
}
[[nodiscard]] inline uint32_t RemainderOpaqueCommands(const State& s, uint32_t view) {
	assert(view < s.temporals.size());
	return s.temporals[view].remainderOpaqueCommands;
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
