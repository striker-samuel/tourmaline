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

// One GPU workgroup per cluster (128 threads, 1 thread per triangle).
static constexpr uint32_t kThreadsPerCluster = 128;
static constexpr uint32_t kMaxClusterTris = 128;
static constexpr uint32_t kMaxBatches = 4096; // surviving (draw x cluster) per view
static constexpr uint32_t kMaxDraws = 1024;   // draws pushed per view
static constexpr uint32_t kMaxMeshes = 4096;
static constexpr uint32_t kMaxViews = 64; // mirrors kMaxRenderViews

// GPU cluster entry (1x uvec4). Must match HLSL GfxClusters (16 bytes).
struct ClusterConstants {
	uint32_t indexOffset = 0; // into the cluster-ordered index buffer (uints)
	uint32_t triCount = 0;
	uint32_t pad[2] = {};
};
static_assert(sizeof(ClusterConstants) == 16);

// Per-draw frame entry. Must match HLSL GfxDraw (80 bytes).
struct alignas(16) Draw {
	glm::mat4 model{};
	uint32_t mesh = 0;
	uint32_t flags = 0; // DrawFlags (bit 0 = two-sided: skip backface/cone cull)
	uint32_t pad[2] = {};
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

// Compute push. Must match HLSL GfxFilterPush in gfxfilter.hlsli (168 bytes).
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
};
static_assert(sizeof(FilterPush) == 168);

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
	Holder<BufferHandle> filteredBuffers[MAX_FRAMES_IN_FLIGHT];
	Holder<BufferHandle> indirectBuffers[MAX_FRAMES_IN_FLIGHT];
	LogicalBufferHandle batchLogicals[MAX_FRAMES_IN_FLIGHT]{};
	LogicalBufferHandle drawLogicals[MAX_FRAMES_IN_FLIGHT]{};
	LogicalBufferHandle filteredLogicals[MAX_FRAMES_IN_FLIGHT]{};
	LogicalBufferHandle indirectLogicals[MAX_FRAMES_IN_FLIGHT]{};
};

struct State {
	// Static (load-time): cluster-ordered global vertex ids + cluster table.
	// Frozen after UploadStaticData, safe to share across frames.
	std::vector<uint32_t> filterIndices;
	std::vector<Cluster> clusters;
	std::vector<Mesh> meshes;
	Holder<BufferHandle> filterIndexBuffer;
	Holder<BufferHandle> clusterBuffer;
	LogicalBufferHandle filterIndexLogical{};
	LogicalBufferHandle clusterLogical{};
	Holder<ShaderModuleHandle> filterModule;
	ShaderProgram filterProgram{};
	std::vector<View> views;
	uint32_t viewCount = 0;
};

// ---------------------------------------------------------------------------
// Procedural API.
// ---------------------------------------------------------------------------

inline constexpr const char* kGfxFilterCsPath = "content/shaders/gfxfilter.cs.hlsl.spv";
inline constexpr const char* kGfxFilteredVsPath = "content/shaders/gfxfiltered.vert.hlsl.spv";

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
	const glm::mat4& model, uint32_t material, DrawFlags flags = 0);
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
[[nodiscard]] inline BufferHandle BatchBuffer(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].batchBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline BufferHandle DrawBuffer(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].drawBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline BufferHandle FilteredBuffer(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].filteredBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline BufferHandle IndirectBuffer(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].indirectBuffers[s.views[view].frameIndex];
}
[[nodiscard]] inline LogicalBufferHandle BatchLogical(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].batchLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline LogicalBufferHandle DrawLogical(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].drawLogicals[s.views[view].frameIndex];
}
[[nodiscard]] inline LogicalBufferHandle FilteredLogical(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].filteredLogicals[s.views[view].frameIndex];
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
