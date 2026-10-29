// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/gfx_filter.h>
#include <Engine/debug_log.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Profile/profile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>
#include <chrono>

namespace smvk {
namespace gfx {

// TMP-PROBE: list-mechanism breakdown for the traversal investigation.
// Accumulates per-scope ms, prints averages every 240 BeginViews.
// Remove once the optimization lands.
namespace {
struct GfxProbeAccum {
	double batches = 0.0, seedRem = 0.0, snap = 0.0, uploads = 0.0;
	int frames = 0;
};
GfxProbeAccum g_probe;
using GfxProbeClock = std::chrono::steady_clock;
struct GfxProbeScope {
	GfxProbeClock::time_point t0 = GfxProbeClock::now();
	double& acc;
	explicit GfxProbeScope(double& a) : acc(a) {}
	~GfxProbeScope() {
		acc += std::chrono::duration<double, std::milli>(GfxProbeClock::now() - t0).count();
	}
};
void GfxProbeReport() {
	if (++g_probe.frames % 240 != 0)
		return;
	const double n = 240.0;
	SMVK_TRACE_LOG(
		"[GfxProbe] avg/frame: batches %.3f seedrem %.3f snap %.3f uploads %.3f ms\n",
		g_probe.batches / n, g_probe.seedRem / n, g_probe.snap / n, g_probe.uploads / n);
	std::fflush(stderr);
	g_probe.batches = g_probe.seedRem = g_probe.snap = g_probe.uploads = 0.0;
}
} // namespace

static constexpr size_t kBatchBufferBytes = kMaxBatchSlots * sizeof(Batch);
static constexpr size_t kDrawBufferBytes = kMaxDraws * sizeof(Draw);
// The filtered ring stays sized by SURVIVORS (kMaxBatches): candidate
// slots allocate their filtered ranges through the filter's atomic counter,
// clamped at this capacity (the GPU twin of the old CPU cap + warning).
static constexpr size_t kFilteredBytes = kMaxBatches * kMaxClusterTris * 3 * sizeof(uint32_t);
static constexpr size_t kIndirectBytes = kMaxBatchSlots * 20; // VkDrawIndexedIndirectCommand
static constexpr size_t kVisClusterBytes = kMaxBatches * sizeof(VisibleCluster);
static constexpr size_t kVisDrawBytes = kMaxDraws * sizeof(Draw);
// Temporal seed/remainder command arrays: candidate-slot capacity (one
// positional command per candidate).
static constexpr size_t kSeedIndirectBytes = kMaxBatchSlots * sizeof(SeedCommand);
// Shadow-list indirect arrays: survivor-capped (the CPU shadow path emits
// only surviving commands, kMaxBatches — no candidate positional layout).
static constexpr size_t kShadowIndirectBytes = kMaxBatches * sizeof(SeedCommand);
// GPU cull chain per-(view, flight) buffers: the kept-bit ring (two parity
// regions inside one buffer — the frame with parity P writes region P and
// reads 1-P, written smvk::limits::MaxFramesInFlight frames earlier, flight-fenced
// complete) and the cullState words (draw verdicts + the filter's atomic
// filtered-offset counter).
static constexpr size_t kKeptBytes = kMaxBatchSlots * 2 * sizeof(uint32_t);
static constexpr size_t kCullStateBytes = kCullStateWords * sizeof(uint32_t);
// Deformed cluster bounds (GfxSkinBounds output, the GfxSkinBoundsTable): one
// GfxClusterData per candidate slot + the per-draw deformed union-bound tail
// (one vec4 per draw index), indexed exactly like the batch cull's candidate
// slots so multi-instance skinned draws stay independent.
static constexpr size_t kSkinBoundsBytes =
	kMaxBatchSlots * sizeof(ClusterConstants) + kMaxDraws * sizeof(glm::vec4);
// Cone-axis debug lines (GfxConeDebug output): two packed DebugLineVertex
// float3s (12B) per candidate slot — the dense candidate-space prefix, so
// [0, 2*candidateTotal) is exactly the written range. Lazily allocated
// once (the overlay's UI toggle arms it) and reused every debug frame.
static constexpr size_t kConeDebugLinesBytes = kMaxBatchSlots * 2 * 12;

static_assert(sizeof(smvk::StaticHeapArena::AllocId) == sizeof(uint64_t),
	"the arena token must fit the opaque uint64_t region fields in gfx_filter.h");

// Suballocates indexCount uint32 indices from the static index monoblock
// (one index VkBuffer for mesh + cluster-ordered + filtered regions, so
// draws use absolute firstIndex and the index binding never switches).
// False when the reservation is exhausted.
static bool AllocIndexRegion(uint32_t indexCount, uint32_t& outBaseIndices, uint64_t& outAllocBits) {
	outBaseIndices = 0;
	outAllocBits = 0;
	if (indexCount == 0)
		return true;
	// The static index monoblock's arena (the VmaVirtualBlock replacement —
	// see static_vertex_heap.h): the token stays an opaque uint64, so the
	// per-flight allocBits packing keeps working verbatim.
	StaticHeapArena& block = staticVertexFmtHeap.m_staticIndexBlock;
	uint64_t byteOffset = 0;
	const StaticHeapArena::AllocId alloc =
		block.Allocate(uint64_t(indexCount) * sizeof(uint32_t),
		               alignof(uint32_t), byteOffset);
	if (alloc == 0)
		return false;
	outBaseIndices = (uint32_t)(byteOffset / sizeof(uint32_t));
	outAllocBits = (uint64_t)alloc;
	return true;
}

static void FreeIndexRegion(uint64_t& allocBits) {
	if (allocBits == 0)
		return;
	staticVertexFmtHeap.m_staticIndexBlock.Free(
		(StaticHeapArena::AllocId)allocBits);
	allocBits = 0;
}

// Retire-ordered index-region free (cluster.cpp's
// DeferFreeStaticMeshRanges discipline): the arena free rides
// DeferDestroyTask, so the range returns to the free list only after
// every in-flight frame's reads have retired (margin
// +MaxFramesInFlight - see VulkanResourceManager::DeferDestroy). The
// LOAD-TIME UploadStaticData call has nothing in flight, where the
// deferral is a harmless delay; the GEOMETRY-STREAMING promote path
// rebuilds these tables BETWEEN frames with live flights, and an
// in-place free would hand the old region to the new filterIndices
// upload while an in-flight cull still reads the old table through the
// old base (garbage draws for a frame - the suballoc-vs-frames-in-
// flight hazard the mesh heap frees already document).
static void DeferFreeIndexRegion(ResourceManager& ctx, uint64_t& allocBits) {
	if (allocBits == 0)
		return;
	const uint64_t bits = allocBits;
	allocBits = 0;
	StaticHeapArena* block = &staticVertexFmtHeap.m_staticIndexBlock;
	ctx.DeferDestroyTask(std::packaged_task<void()>(
		[block, bits] {
			if (bits)
				block->Free((StaticHeapArena::AllocId)bits);
		}));
}

static void RollbackSlotBuffers(RenderGraph& rg, View& v, uint32_t doneFrames) {
	for (uint32_t f = 0; f < doneFrames; ++f) {
		if (!v.batchLogicals[f].empty()) rg.FreeBuffer(v.batchLogicals[f]);
		if (!v.drawLogicals[f].empty()) rg.FreeBuffer(v.drawLogicals[f]);
		if (!v.indirectLogicals[f].empty()) rg.FreeBuffer(v.indirectLogicals[f]);
		if (!v.keptLogicals[f].empty()) rg.FreeBuffer(v.keptLogicals[f]);
		if (!v.cullStateLogicals[f].empty()) rg.FreeBuffer(v.cullStateLogicals[f]);
		v.batchLogicals[f] = {};
		v.drawLogicals[f] = {};
		v.indirectLogicals[f] = {};
		v.keptLogicals[f] = {};
		v.cullStateLogicals[f] = {};
		FreeIndexRegion(v.filteredAllocBits[f]);
		v.filteredBase[f] = 0;
	}
	// Holders release their buffers automatically on scope exit.
}

static bool CreateSlotBuffers(ResourceManager& ctx, RenderGraph& rg, View& v) {
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
		v.batchBuffers[f] = ctx.CreateBuffer(
			kBatchBufferBytes, BufferType_Storage, "gfx batches");
		v.drawBuffers[f] = ctx.CreateBuffer(
			kDrawBufferBytes, BufferType_Storage, "gfx draws");
		// Filtered-index scratch is a persistent region in the static index
		// monoblock (not a VkBuffer): draws address it absolutely, so the
		// index binding never switches. One region per flight slice.
		if (!AllocIndexRegion(
			(uint32_t)(kFilteredBytes / sizeof(uint32_t)),
			v.filteredBase[f], v.filteredAllocBits[f])) {
			RollbackSlotBuffers(rg, v, f);
			return false;
		}
		// Storage, NOT Indirect: per-frame zero-fill uploads run on the
		// transfer queue while compute/draw run on graphics. Storage gets
		// CONCURRENT sharing (see CreateBuffer); Indirect is EXCLUSIVE and
		// would need explicit queue-ownership transfers every frame.
		// Usage flags cover INDIRECT_BUFFER_BIT regardless (see
		// bufferTypeToUsageFlags).
		v.indirectBuffers[f] = ctx.CreateBuffer(
			kIndirectBytes, BufferType_Storage, "gfx indirect");
		// GPU cull chain state (kept-bit parity ring + verdict/counter
		// words). Zero-filled at creation so the first frames seed nothing
		// (prev-kept = 0 -> empty seed, remainder = all kept — the same
		// arming behavior InvalidateHistory gives); every
		// candidate slot gets a definitive 0/1 write per frame afterwards,
		// so no per-frame clearing upload exists.
		v.keptBuffers[f] = ctx.CreateBuffer(
			kKeptBytes, BufferType_Storage, "gfx keptBits");
		v.cullStateBuffers[f] = ctx.CreateBuffer(
			kCullStateBytes, BufferType_Storage, "gfx cullState");
		if (v.batchBuffers[f].empty() || v.drawBuffers[f].empty()
			|| v.indirectBuffers[f].empty() || v.keptBuffers[f].empty()
			|| v.cullStateBuffers[f].empty()) {
			RollbackSlotBuffers(rg, v, f);
			return false;
		}
		// Creation zero-fill (the kept ring is 2MB — one shared static fill).
		static std::vector<uint8_t> s_zeroFill;
		if (s_zeroFill.size() < kKeptBytes)
			s_zeroFill.assign(kKeptBytes, 0);
		ctx.UploadDataToBuffer(v.keptBuffers[f], s_zeroFill.data(), kKeptBytes,
			PipelineStage(PipelineStageBit_Compute),
			AccessFlags::ShaderRead | AccessFlags::ShaderWrite, 0, 0);
		ctx.UploadDataToBuffer(v.cullStateBuffers[f], s_zeroFill.data(), kCullStateBytes,
			PipelineStage(PipelineStageBit_Compute),
			AccessFlags::ShaderRead | AccessFlags::ShaderWrite, 0, 0);
		v.batchLogicals[f] = rg.AddExternalBuffer(v.batchBuffers[f], "gfx batches");
		v.drawLogicals[f] = rg.AddExternalBuffer(v.drawBuffers[f], "gfx draws");
		v.indirectLogicals[f] = rg.AddExternalBuffer(v.indirectBuffers[f], "gfx indirect");
		v.keptLogicals[f] = rg.AddExternalBuffer(v.keptBuffers[f], "gfx keptBits");
		v.cullStateLogicals[f] = rg.AddExternalBuffer(v.cullStateBuffers[f], "gfx cullState");
		if (v.batchLogicals[f].empty() || v.drawLogicals[f].empty()
			|| v.indirectLogicals[f].empty() || v.keptLogicals[f].empty()
			|| v.cullStateLogicals[f].empty()) {
			RollbackSlotBuffers(rg, v, f + 1);
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool EnsureShaders(State& s, ResourceManager& ctx, const char* csPath) {
	if (s.filterProgram.shaderCount != 0 && !s.filterModule.empty())
		return true;
	try {
		if (s.filterModule.empty())
			s.filterModule = ctx.CreateShaderModule(
				readFile(csPath ? csPath : kGfxFilterCsPath), "gfxFilterCS");
	}
	catch (...) {
		return false;
	}
	if (s.filterModule.empty())
		return false;
	Shader cs{ .module = s.filterModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
	s.filterProgram = { .shaders = { cs }, .shaderCount = 1 };
	return true;
}

bool Ensure(State& s, ResourceManager& ctx, RenderGraph& rg, const char* csPath) {
	(void)rg; // per-(view, frame) buffers are pooled on demand by BeginView
	if (!EnsureShaders(s, ctx, csPath))
		return false;
	if (!EnsureHizShaders(s, ctx))
		return false;
	// GPU cull chain programs (missing .spv fails the chain's passes
	// loudly at record time — best effort, never fatal; there is no
	// fallback path anymore, so a broken install draws holes, not
	// silently-uncalled geometry).
	(void)EnsureBatchCullShaders(s, ctx);
	(void)EnsureDrawCullShaders(s, ctx);
	// SPD HZB twin (hzb_spd .min): best effort too — a missing module
	// keeps the per-mip culling build (the A/B switch falls back at
	// AppendHizPasses record time).
	(void)EnsureHizSpdShaders(s, ctx);
	return true;
}

// Kept-bit parity for the frame being recorded: alternates between a
// flight's consecutive users (smvk::limits::MaxFramesInFlight frames apart), so the
// region this frame writes was last written by the previous same-flight
// user — fence-complete, race-free (the rendergraph never needs to track
// the cross-frame dependency; the flight submission ordering carries it).
static uint32_t KeptParity(const State& s) {
	return (s.frameCounter / smvk::limits::MaxFramesInFlight) % 2u;
}

bool EnsureBatchCullShaders(State& s, ResourceManager& ctx, const char* csPath) {
	if (s.batchCullProgram.shaderCount != 0 && !s.batchCullModule.empty())
		return true;
	try {
		if (s.batchCullModule.empty())
			s.batchCullModule = ctx.CreateShaderModule(
				readFile(csPath ? csPath : kGfxBatchCullCsPath), "gfxBatchCullCS");
	}
	catch (...) {
		return false;
	}
	if (s.batchCullModule.empty())
		return false;
	Shader cs{ .module = s.batchCullModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
	s.batchCullProgram = { .shaders = { cs }, .shaderCount = 1 };
	return true;
}

bool EnsureDrawCullShaders(State& s, ResourceManager& ctx, const char* csPath) {
	if (s.drawCullProgram.shaderCount != 0 && !s.drawCullModule.empty())
		return true;
	try {
		if (s.drawCullModule.empty())
			s.drawCullModule = ctx.CreateShaderModule(
				readFile(csPath ? csPath : kGfxDrawCullCsPath), "gfxDrawCullCS");
	}
	catch (...) {
		return false;
	}
	if (s.drawCullModule.empty())
		return false;
	Shader cs{ .module = s.drawCullModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
	s.drawCullProgram = { .shaders = { cs }, .shaderCount = 1 };
	return true;
}

bool EnsureSkinBounds(State& s, ResourceManager& ctx, RenderGraph& rg) {
	if (s.skinBoundsReady)
		return true;
	if (s.skinBoundsProgram.shaderCount == 0 || s.skinBoundsModule.empty()) {
		try {
			if (s.skinBoundsModule.empty())
				s.skinBoundsModule = ctx.CreateShaderModule(
					readFile(kGfxSkinBoundsCsPath), "gfxSkinBoundsCS");
		}
		catch (...) {
			return false;
		}
		if (s.skinBoundsModule.empty())
			return false;
		Shader cs{ .module = s.skinBoundsModule, .entryPoint = "mainCS",
			.stage = ShaderStage::Compute };
		s.skinBoundsProgram = { .shaders = { cs }, .shaderCount = 1 };
	}
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
		if (s.skinBoundsBuffers[f].empty()) {
			s.skinBoundsBuffers[f] = ctx.CreateBuffer(
				kSkinBoundsBytes, BufferType_Storage, "gfx skinBounds");
			if (s.skinBoundsBuffers[f].empty())
				return false;
		}
		if (s.skinBoundsLogicals[f].empty()) {
			s.skinBoundsLogicals[f] = rg.AddExternalBuffer(
				s.skinBoundsBuffers[f], "gfx skinBounds");
			if (s.skinBoundsLogicals[f].empty())
				return false;
		}
	}
	s.skinBoundsReady = true;
	return true;
}

bool EnsureHizShaders(State& s, ResourceManager& ctx, const char* csPath) {
	if (s.hizProgram.shaderCount != 0 && !s.hizModule.empty())
		return true;
	try {
		if (s.hizModule.empty())
			s.hizModule = ctx.CreateShaderModule(
				readFile(csPath ? csPath : kHizCsPath), "hizBuildCS");
	}
	catch (...) {
		return false;
	}
	if (s.hizModule.empty())
		return false;
	Shader cs{ .module = s.hizModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
	s.hizProgram = { .shaders = { cs }, .shaderCount = 1 };
	return true;
}

void SetHizSpd(State& s, bool live, bool verify) {
	s.hizSpd = live;
	s.hizSpdVerify = verify;
}

bool EnsureHizSpdShaders(State& s, ResourceManager& ctx, const char* csPath) {
	if (s.hizSpdProgram.shaderCount != 0 && !s.hizSpdModule.empty())
		return true;
	try {
		if (s.hizSpdModule.empty())
			s.hizSpdModule = ctx.CreateShaderModule(
				readFile(csPath ? csPath : kHizSpdCsPath), "hizSpdBuildCS");
	}
	catch (...) {
		return false;
	}
	if (s.hizSpdModule.empty())
		return false;
	Shader cs{ .module = s.hizSpdModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
	s.hizSpdProgram = { .shaders = { cs }, .shaderCount = 1 };
	return true;
}

static void RollbackTemporalBuffers(RenderGraph& rg, TemporalView& t, uint32_t doneFrames) {
	for (uint32_t f = 0; f < doneFrames; ++f) {
		if (!t.visClusterLogicals[f].empty()) rg.FreeBuffer(t.visClusterLogicals[f]);
		if (!t.visDrawLogicals[f].empty()) rg.FreeBuffer(t.visDrawLogicals[f]);
		if (!t.seedIndirectLogicals[f].empty()) rg.FreeBuffer(t.seedIndirectLogicals[f]);
		if (!t.remainderIndirectLogicals[f].empty()) rg.FreeBuffer(t.remainderIndirectLogicals[f]);
		t.visClusterLogicals[f] = {};
		t.visDrawLogicals[f] = {};
		t.seedIndirectLogicals[f] = {};
		t.remainderIndirectLogicals[f] = {};
	}
	// Holders release their buffers automatically on scope exit.
}

static bool CreateTemporalBuffers(ResourceManager& ctx, RenderGraph& rg, TemporalView& t) {
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
		t.visClusterBuffers[f] = ctx.CreateBuffer(
			kVisClusterBytes, BufferType_Storage, "gfx visibleClusters");
		t.visDrawBuffers[f] = ctx.CreateBuffer(
			kVisDrawBytes, BufferType_Storage, "gfx visibleDraws");
		// Storage, NOT Indirect (same queue-sharing argument as the filter
		// indirect buffers in CreateSlotBuffers); INDIRECT usage is covered
		// by the type's flags regardless (see bufferTypeToUsageFlags).
		t.seedIndirectBuffers[f] = ctx.CreateBuffer(
			kSeedIndirectBytes, BufferType_Storage, "gfx seedIndirect");
		t.remainderIndirectBuffers[f] = ctx.CreateBuffer(
			kSeedIndirectBytes, BufferType_Storage, "gfx remainderIndirect");
		if (t.visClusterBuffers[f].empty() || t.visDrawBuffers[f].empty()
			|| t.seedIndirectBuffers[f].empty() || t.remainderIndirectBuffers[f].empty()) {
			RollbackTemporalBuffers(rg, t, f);
			return false;
		}
		t.visClusterLogicals[f] = rg.AddExternalBuffer(t.visClusterBuffers[f], "gfx visibleClusters");
		t.visDrawLogicals[f] = rg.AddExternalBuffer(t.visDrawBuffers[f], "gfx visibleDraws");
		t.seedIndirectLogicals[f] = rg.AddExternalBuffer(t.seedIndirectBuffers[f], "gfx seedIndirect");
		t.remainderIndirectLogicals[f] = rg.AddExternalBuffer(t.remainderIndirectBuffers[f], "gfx remainderIndirect");
		if (t.visClusterLogicals[f].empty() || t.visDrawLogicals[f].empty()
			|| t.seedIndirectLogicals[f].empty() || t.remainderIndirectLogicals[f].empty()) {
			RollbackTemporalBuffers(rg, t, f + 1);
			return false;
		}
	}
	return true;
}

void Destroy(State& s, RenderGraph& rg) {
	for (auto& v : s.views) {
		for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
			if (!v.batchLogicals[f].empty()) rg.FreeBuffer(v.batchLogicals[f]);
			if (!v.drawLogicals[f].empty()) rg.FreeBuffer(v.drawLogicals[f]);
			if (!v.indirectLogicals[f].empty()) rg.FreeBuffer(v.indirectLogicals[f]);
			if (!v.keptLogicals[f].empty()) rg.FreeBuffer(v.keptLogicals[f]);
			if (!v.cullStateLogicals[f].empty()) rg.FreeBuffer(v.cullStateLogicals[f]);
			v.keptLogicals[f] = {};
			v.cullStateLogicals[f] = {};
			FreeIndexRegion(v.filteredAllocBits[f]);
			v.filteredBase[f] = 0;
		}
	}
	for (auto& t : s.temporals) {
		for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
			if (!t.visClusterLogicals[f].empty()) rg.FreeBuffer(t.visClusterLogicals[f]);
			if (!t.visDrawLogicals[f].empty()) rg.FreeBuffer(t.visDrawLogicals[f]);
			if (!t.seedIndirectLogicals[f].empty()) rg.FreeBuffer(t.seedIndirectLogicals[f]);
			if (!t.remainderIndirectLogicals[f].empty()) rg.FreeBuffer(t.remainderIndirectLogicals[f]);
			if (!t.hiz[f].logical.empty()) rg.FreeBuffer(t.hiz[f].logical);
			t.hiz[f].logical = {};
			t.hiz[f].buffer = {};
			// The SPD verify shadow (same Holder/logical split) + the
			// view's counter ring (extent-independent, one logical).
			if (!t.hizVerify[f].logical.empty()) rg.FreeBuffer(t.hizVerify[f].logical);
			t.hizVerify[f].logical = {};
			t.hizVerify[f].buffer = {};
		}
		if (!t.hizSpdCounterLogical.empty()) rg.FreeBuffer(t.hizSpdCounterLogical);
		t.hizSpdCounterLogical = {};
		t.hizSpdCounter = {};
	}
	for (auto& sv : s.shadowViews) {
		for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
			if (!sv.drawLogicals[f].empty()) rg.FreeBuffer(sv.drawLogicals[f]);
			if (!sv.indirectLogicals[f].empty()) rg.FreeBuffer(sv.indirectLogicals[f]);
			sv.drawLogicals[f] = {};
			sv.indirectLogicals[f] = {};
		}
		sv.buffersReady = false;
	}
	s.temporals.clear();
	s.shadowViews.clear();
	s.shadowViewCount = 0;
	if (!s.indexHeapLogical.empty()) rg.FreeBuffer(s.indexHeapLogical);
	if (!s.clusterLogical.empty()) rg.FreeBuffer(s.clusterLogical);
	if (!s.meshBoundsLogical.empty()) rg.FreeBuffer(s.meshBoundsLogical);
	// The GfxSkinBounds table logicals (the buffers themselves retire via
	// their Holders; the rendergraph logicals need the explicit free, the
	// same as every other per-flight external buffer above).
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
		if (!s.skinBoundsLogicals[f].empty())
			rg.FreeBuffer(s.skinBoundsLogicals[f]);
		s.skinBoundsLogicals[f] = {};
		s.skinBoundsBuffers[f] = {};
	}
	s.skinBoundsReady = false;
	// The cone-debug line buffer's logical (same Holder/logical split; the
	// buffer retires via its Holder).
	if (!s.coneDebugLinesLogical.empty())
		rg.FreeBuffer(s.coneDebugLinesLogical);
	s.coneDebugLinesLogical = {};
	s.coneDebugLines = {};
	s.coneDebug = false;
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
		if (!s.meshParamsLogicals[f].empty()) rg.FreeBuffer(s.meshParamsLogicals[f]);
		if (!s.meshBatchListLogicals[f].empty()) rg.FreeBuffer(s.meshBatchListLogicals[f]);
		s.meshParamsLogicals[f] = {};
		s.meshParamsBuffers[f] = {};
		s.meshBatchListLogicals[f] = {};
		s.meshBatchLists[f] = {};
	}
	s.indexHeapLogical = {};
	s.clusterLogical = {};
	s.meshBoundsLogical = {};
	s.meshBoundsBuffer = {};
	if (!s.meshesLogical.empty()) rg.FreeBuffer(s.meshesLogical);
	s.meshesLogical = {};
	s.meshesBuffer = {};
	// Demand source (no rendergraph logicals by design — the retired
	// feedback-ring pattern: transfer fills + the async readback chain
	// carry the sync, the rendergraph never tracks these buffers).
	s.textureMetaBuffer = {};
	s.textureMetaEpoch = 0;
	s.textureMetaCount = 0;
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f)
		s.demandBuffers[f] = {};
	s.demandMaterialsBda = 0;
	s.demandMipBias = 0.0f;
	s.views.clear();
	s.viewCount = 0;
	FreeIndexRegion(s.clusterIndexAllocBits);
	s.clusterIndexBase = 0;
	s.clusterBuffer = {};
	// (ShaderProgram() - the paren value-init form: GCC 13 ICEs on the
	// brace temporary; see the NOTE at struct ShaderProgram in smvk.h.)
	s.filterProgram = ShaderProgram();
	s.filterModule = {};
	s.hizProgram = ShaderProgram();
	s.hizModule = {};
	s.batchCullProgram = ShaderProgram();
	s.batchCullModule = {};
	s.drawCullProgram = ShaderProgram();
	s.drawCullModule = {};
	s.frameCounter = 0;
}

// ---------------------------------------------------------------------------
// Hierarchical-Z (T2)
// ---------------------------------------------------------------------------

uint32_t HzbMipCount(uint32_t w, uint32_t h) {
	if (w == 0 || h == 0)
		return 0;
	uint32_t count = 1;
	while ((w > 1 || h > 1) && count < kHizMaxMips) {
		w = std::max(1u, (w + 1u) >> 1);
		h = std::max(1u, (h + 1u) >> 1);
		++count;
	}
	return count;
}

void HzbMipDims(uint32_t w, uint32_t h, uint32_t mip, uint32_t& outW, uint32_t& outH) {
	for (uint32_t k = 0; k < mip; ++k) {
		w = std::max(1u, (w + 1u) >> 1);
		h = std::max(1u, (h + 1u) >> 1);
	}
	outW = w;
	outH = h;
}

uint32_t HzbMipOffsetFloats(uint32_t w, uint32_t h, uint32_t mip) {
	uint32_t off = 0;
	for (uint32_t k = 0; k < mip; ++k) {
		uint32_t kw = w, kh = h;
		HzbMipDims(w, h, k, kw, kh);
		off += kw * kh;
	}
	return off;
}

uint32_t HzbTotalFloats(uint32_t w, uint32_t h) {
	const uint32_t count = HzbMipCount(w, h);
	if (count == 0)
		return 0;
	uint32_t lw = 0, lh = 0;
	HzbMipDims(w, h, count - 1, lw, lh);
	return HzbMipOffsetFloats(w, h, count - 1) + lw * lh;
}

ClusterFootprint ProjectClusterFootprint(const glm::vec3& worldCenter,
	float worldRadius, const glm::vec3& eye, const glm::mat4& viewProj,
	uint32_t width, uint32_t height) {
	ClusterFootprint f{};
	const glm::vec3 toEye = eye - worldCenter;
	const float dist = glm::length(toEye);
	if (dist <= worldRadius || dist < 1e-6f)
		return f; // eye inside the sphere: cannot be occluded, keep
	// Clip-space intervals (same math as gfxfilter.cs.hlsl
	// ClusterOccluded, unguarded — callers add the jitter guard): pinhole
	// focal lengths (viewProj[0][0] as fx) are only valid for axis-aligned
	// views (yaw mixes rotation into them and collapses the footprint),
	// while row lengths are exact under any rotation. Rows use glm
	// m[col][row]: row r is (m[0][r], m[1][r], m[2][r]).
	const glm::vec4 clipC = viewProj * glm::vec4(worldCenter, 1.0f);
	const float xRadius = worldRadius * glm::length(
		glm::vec3(viewProj[0][0], viewProj[1][0], viewProj[2][0]));
	const float yRadius = worldRadius * glm::length(
		glm::vec3(viewProj[0][1], viewProj[1][1], viewProj[2][1]));
	const float zRadius = worldRadius * glm::length(
		glm::vec3(viewProj[0][2], viewProj[1][2], viewProj[2][2]));
	const float wRadius = worldRadius * glm::length(
		glm::vec3(viewProj[0][3], viewProj[1][3], viewProj[2][3]));
	const float wMin = clipC.w - wRadius;
	const float wMax = clipC.w + wRadius;
	if (!(wMin > 1e-6f) || !(wMax >= wMin))
		return f; // touches/crosses the camera plane: keep
	auto interval = [](float nc, float nr, float wLo, float wHi) {
		const float r0 = (nc - nr) / wLo, r1 = (nc - nr) / wHi;
		const float r2 = (nc + nr) / wLo, r3 = (nc + nr) / wHi;
		return std::pair<float, float>{
			std::min(std::min(r0, r1), std::min(r2, r3)),
			std::max(std::max(r0, r1), std::max(r2, r3)) };
	};
	const auto ndcX = interval(clipC.x, xRadius, wMin, wMax);
	const auto ndcY = interval(clipC.y, yRadius, wMin, wMax);
	const auto ndcZ = interval(clipC.z, zRadius, wMin, wMax);
	const float dNear = ndcZ.second; // reversed-Z nearest over the sphere
	if (!(dNear >= 0.0f) || dNear > 1.0f || !std::isfinite(dNear))
		return f;
	// Framebuffer y-down == NDC-down post-Y-flip (cf. forwardplus.cs.hlsl).
	const float w = (float)width, h = (float)height;
	const float x0 = (ndcX.first * 0.5f + 0.5f) * w;
	const float x1 = (ndcX.second * 0.5f + 0.5f) * w;
	const float y0 = (ndcY.first * 0.5f + 0.5f) * h;
	const float y1 = (ndcY.second * 0.5f + 0.5f) * h;
	if (!std::isfinite(x0) || !std::isfinite(x1)
		|| !std::isfinite(y0) || !std::isfinite(y1))
		return f;
	f.centerX = (x0 + x1) * 0.5f;
	f.centerY = (y0 + y1) * 0.5f;
	f.radiusX = std::max((x1 - x0) * 0.5f, 0.0f);
	f.radiusY = std::max((y1 - y0) * 0.5f, 0.0f);
	f.nearestDepth = dNear;
	// Fully off-target footprints stay testable (behindCamera=false) so the
	// audit tags them offscreen; HzbOccluded keeps them like the frustum
	// stages would.
	f.behindCamera = false;
	return f;
}

uint32_t HzbMipForRect(float rectW, float rectH, uint32_t mips) {
	assert(mips > 0);
	const float m = std::max(rectW, rectH);
	uint32_t level = 0;
	float texel = 1.0f;
	while (texel < m && level + 1 < mips) {
		texel *= 2.0f;
		++level;
	}
	return level;
}

bool HzbOccluded(const float* hzb, uint32_t w, uint32_t h, uint32_t mips,
	const ClusterFootprint& f, float* outBindingStored) {
	if (outBindingStored)
		*outBindingStored = -1.0f;
	if (hzb == nullptr || mips == 0 || w == 0 || h == 0)
		return false;
	if (f.behindCamera)
		return false;
	const float rectW = f.radiusX * 2.0f;
	const float rectH = f.radiusY * 2.0f;
	if (rectW <= 0.0f || rectH <= 0.0f)
		return false;
	// Rect missing the target entirely: the frustum stages own this case.
	if (f.centerX + f.radiusX < 0.0f || f.centerX - f.radiusX > (float)w
		|| f.centerY + f.radiusY < 0.0f || f.centerY - f.radiusY > (float)h)
		return false;
	const uint32_t level = HzbMipForRect(rectW, rectH, mips);
	uint32_t lw = 0, lh = 0;
	HzbMipDims(w, h, level, lw, lh);
	if (lw == 0 || lh == 0)
		return false;
	// Conservative cover: the rect can straddle up to 2x2 texels at the
	// covering mip (texel >= rect, but alignment is arbitrary). Hidden only
	// if EVERY overlapped texel occludes; any far texel keeps.
	// Clamp BEFORE the float->uint conversion: a negative edge (rect
	// hanging off the left/top) would otherwise wrap to a huge index.
	// Reversed-Z: hidden iff the nearest point is farther (smaller value)
	// than every overlapped stored closest.
	const float texelSize = (float)(1u << level);
	const uint32_t tx0 = std::min(
		(uint32_t)std::max(std::floor((f.centerX - f.radiusX) / texelSize), 0.0f), lw - 1);
	const uint32_t tx1 = std::min(
		(uint32_t)std::max(std::floor((f.centerX + f.radiusX) / texelSize), 0.0f), lw - 1);
	const uint32_t ty0 = std::min(
		(uint32_t)std::max(std::floor((f.centerY - f.radiusY) / texelSize), 0.0f), lh - 1);
	const uint32_t ty1 = std::min(
		(uint32_t)std::max(std::floor((f.centerY + f.radiusY) / texelSize), 0.0f), lh - 1);
	// Bounded: rect <= texel guarantees at most 2 per axis; cap anyway.
	float bindingStored = std::numeric_limits<float>::max();
	bool occluded = true;
	for (uint32_t ty = ty0; ty <= ty1 && ty < ty0 + 3; ++ty) {
		for (uint32_t tx = tx0; tx <= tx1 && tx < tx0 + 3; ++tx) {
			const float stored = hzb[HzbMipOffsetFloats(w, h, level) + ty * lw + tx];
			bindingStored = std::min(bindingStored, stored);
			if (f.nearestDepth >= stored) {
				occluded = false;
			}
		}
	}
	if (outBindingStored)
		*outBindingStored = bindingStored;
	return occluded;
}

// (Re)builds ONE HizResources slice for w/h: frees the old logical,
// zero-fills the fresh buffer (0 = far under reversed-Z: keep-safe, no
// garbage MIN-reducing into a 1-frame over-cull pop), registers it.
// Shared by the live pyramid and the SPD verify shadow. Returns false on
// allocation failure (the slice is left with mips=0 — callers gate on it).
static bool RebuildHizSlice(ResourceManager& ctx, RenderGraph& rg,
	HizResources& hz, uint32_t w, uint32_t h, const char* name) {
	const uint32_t wantMips = HzbMipCount(w, h);
	if (!hz.logical.empty()) rg.FreeBuffer(hz.logical);
	hz.logical = {};
	hz.buffer = {};
	hz.width = w;
	hz.height = h;
	hz.mipCount = wantMips;
	for (uint32_t m = 0; m < wantMips; ++m)
		hz.mipOffsets[m] = HzbMipOffsetFloats(w, h, m);
	for (uint32_t m = wantMips; m < kHizMaxMips; ++m)
		hz.mipOffsets[m] = 0;
	// Diagnose-only: shader-side array cap (hzb.hlsli HZB_MAX_FLOATS).
	// Exceeding it reads/writes out of bounds -> undefined HZB -> over-cull.
	static constexpr uint32_t kHzbShaderMaxFloats = 12582912;
	if ((uint64_t)HzbTotalFloats(w, h) > kHzbShaderMaxFloats) {
		SMVK_DEBUG_LOG(
			"[GfxHiz] target %ux%u needs %u floats, exceeds shader cap %u: "
			"expect pyramid corruption.\n",
			w, h, HzbTotalFloats(w, h), kHzbShaderMaxFloats);
	}
	hz.buffer = ctx.CreateBuffer(
		(size_t)HzbTotalFloats(w, h) * sizeof(float),
		BufferType_Storage, name);
	if (hz.buffer.empty()) {
		// Leave no half-built pyramid behind: the shader gates on mips.
		hz.width = hz.height = hz.mipCount = 0;
		return false;
	}
	// Fresh pyramid starts as far (0 under reversed-Z = keep-safe). Without
	// this, a rebuild leaves garbage that MIN-reduces upward into a 1-frame
	// over-cull pop on fast yaw / fly-through (Release exposes it; Debug
	// validation + timing usually masks it). Resizes are rare, so a full
	// zero-fill is affordable.
	{
		std::vector<uint8_t> zeros((size_t)HzbTotalFloats(w, h) * sizeof(float), 0);
		if (!zeros.empty()) {
			ctx.UploadDataToBuffer(hz.buffer, zeros.data(), zeros.size(),
				PipelineStage(PipelineStageBit_Compute),
				AccessFlags::ShaderRead | AccessFlags::ShaderWrite);
		}
	}
	hz.logical = rg.AddExternalBuffer(hz.buffer, name);
	if (hz.logical.empty()) {
		hz.buffer = {};
		hz.width = hz.height = hz.mipCount = 0;
		return false;
	}
	return true;
}

// SPD counter slice stride (mirrors Ssgi::Hzb::kHzbCounterSliceBytes —
// one 16-byte word set per frame in flight).
static constexpr uint32_t kHizSpdCounterSliceBytes = 16;

bool EnsureHiz(State& s, ResourceManager& ctx, RenderGraph& rg,
	uint32_t view, uint32_t w, uint32_t h) {
	TemporalView& t = Temporal(s, view);
	assert(view < s.views.size());
	const uint32_t flight = s.views[view].frameIndex;
	HizResources& hz = t.hiz[flight];
	if (w == 0 || h == 0) {
		t.historyValid = false;
		return false;
	}
	// SPD completion-counter ring: extent-independent, one buffer per
	// view, created lazily while SPD (or its verify) is armed. Creation-
	// zero-filled per the GPU-counter convention (a recycled garbage
	// count breaks the last-group test and leaves the tail mips stale);
	// the shader's last group resets the slice after its bump. Failure
	// is non-fatal: the ring stays empty and AppendHizPasses falls back
	// to the per-mip path at record time.
	if ((s.hizSpd || s.hizSpdVerify) && t.hizSpdCounter.empty()) {
		const size_t counterBytes =
			(size_t)kHizSpdCounterSliceBytes * smvk::limits::MaxFramesInFlight;
		t.hizSpdCounter = ctx.CreateBuffer(counterBytes,
			BufferType_Storage, "gfx hizSpd counter");
		if (!t.hizSpdCounter.empty()) {
			std::vector<uint8_t> zeros(counterBytes, 0);
			ctx.UploadDataToBuffer(t.hizSpdCounter, zeros.data(), zeros.size(),
				PipelineStage(PipelineStageBit_Compute),
				AccessFlags::ShaderRead | AccessFlags::ShaderWrite);
			t.hizSpdCounterLogical = rg.AddExternalBuffer(t.hizSpdCounter,
				"gfx hizSpd counter");
			if (t.hizSpdCounterLogical.empty())
				t.hizSpdCounter = {};
		}
	}
	const uint32_t wantMips = HzbMipCount(w, h);
	const bool hzOk = hz.width == w && hz.height == h && hz.mipCount == wantMips
		&& !hz.buffer.empty() && !hz.logical.empty();
	// The verify shadow only exists while the switch is on; toggling it
	// off just orphans the old slice (freed on the next rebuild/destroy).
	HizResources& hzV = t.hizVerify[flight];
	const bool verifyOn = s.hizSpdVerify;
	const bool hzVOk = !verifyOn || (hzV.width == w && hzV.height == h
		&& hzV.mipCount == wantMips && !hzV.buffer.empty()
		&& !hzV.logical.empty());
	if (hzOk && hzVOk)
		return true; // steady state: keep history
	if (!hzOk) {
		// Size drift: rebuild + disarm (stale seed extent).
		t.historyValid = false;
		if (!RebuildHizSlice(ctx, rg, hz, w, h, "gfx hiz"))
			return false;
	}
	if (verifyOn && !hzVOk) {
		// Shadow failure is non-fatal (verify just stays unarmed — the
		// record-time gate checks the slice); the live pyramid stands.
		(void)RebuildHizSlice(ctx, rg, hzV, w, h, "gfx hizVerify");
	}
	return true;
}

uint64_t HizAddress(State& s, ResourceManager& ctx, uint32_t view) {
	if (view >= s.temporals.size() || view >= s.views.size())
		return 0;
	return ctx.GetBufferDeviceAddress(s.temporals[view].hiz[s.views[view].frameIndex].buffer);
}

RenderPass& AddHizPass(State& s, RenderGraph& rg, uint32_t view,
	uint32_t dstMip, LogicalImageViewHandle depthLogical, const char* passName) {
	assert(view < s.temporals.size());
	assert(view < s.views.size());
	TemporalView& t = s.temporals[view];
	HizResources& hz = t.hiz[s.views[view].frameIndex];

	SMVK_PROFILE_RENDER("GfxAddHizPass");
	SMVK_PROFILE_TAG("mip", dstMip);
	SMVK_PROFILE_TAG("mipCount", hz.mipCount);

	RenderPass& pass = rg.AddComputePass(passName ? passName : "GfxHiz");
	BufferSyncRequest hizRW{};
	hizRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	hizRW.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(hz.logical, hizRW);
	if (dstMip == 0 && !depthLogical.empty()) {
		ImageSyncRequest depthReq{};
		depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
		depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
		// Use DepthStencilReadOnlyOptimal to match the bindless descriptor layout
		// (created with DepthStencilReadOnlyOptimal in DepthTarget::Create and ForwardPlus::Ensure).
		// The render graph's depth fixup would promote ShaderReadOnlyOptimal, but being
		// explicit avoids any layout mismatch between descriptor and actual image layout.
		depthReq.layout = ImageLayout::DepthStencilReadOnlyOptimal;
		pass.AddImageHint(depthLogical, depthReq);
	}

	// DEBUG: Log HZB pass creation
	static uint64_t s_hizPassCount = 0;
	if ((s_hizPassCount++ % 60) == 0) {
		SMVK_DEBUG_LOG(
			"[GfxHiz] AddHizPass view=%u mip=%u/%u depthValid=%d passName=%s\n",
			view, dstMip, hz.mipCount, !depthLogical.empty(), passName ? passName : "GfxHiz");
	}

	return pass;
}

void PushAndDispatchHiz(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view, uint32_t dstMip, uint32_t depthBindlessIndex) {
	assert(view < s.temporals.size());
	assert(view < s.views.size());
	TemporalView& t = s.temporals[view];
	HizResources& hz = t.hiz[s.views[view].frameIndex];
	if (dstMip >= hz.mipCount)
		return;

	SMVK_PROFILE_SCOPE("GfxPushAndDispatchHiz");
	SMVK_PROFILE_TAG("dstMip", dstMip);
	SMVK_PROFILE_TAG("mipCount", hz.mipCount);

	uint32_t dw = 0, dh = 0;
	HzbMipDims(hz.width, hz.height, dstMip, dw, dh);
	if (dw == 0 || dh == 0)
		return;
	const uint64_t addr = ctx.GetBufferDeviceAddress(hz.buffer);
	// Null-BDA guard (same device-lost lesson as PushAndDispatch): skip the
	// level instead of faulting; the HZB stays stale and T4 keeps geometry.
	// Explicit fprintf (not SMVK_DEBUG_LOG): this fires on real faults and
	// must be visible in Release builds too.
	if (addr == 0) {
		static uint64_t s_hizNullWarns = 0;
		if ((s_hizNullWarns++ % 300) == 0) {
			std::fprintf(stderr,
				"[GfxHiz] refusing dispatch: null HZB address (view %u mip %u).\n",
				view, dstMip);
		}
		return;
	}
	// Diagnose-only: invalid depth bindless for the mip0 copy reads garbage
	// (undefined values); garbage-near MIN-reduces upward and over-culls.
	// Throttled explicit fprintf (Release-visible); no behavior change.
	if (dstMip == 0 && depthBindlessIndex == UINT32_MAX) {
		static uint64_t s_hizDepthWarns = 0;
		if ((s_hizDepthWarns++ % 300) == 0) {
			std::fprintf(stderr,
				"[GfxHiz] mip0 copy with invalid depth bindless (view %u): "
				"HZB content undefined, expect over-cull.\n",
				view);
		}
	}
	// Diagnose-only: HZB extent vs view extent mismatch misaligns footprint
	// rows/cols with depth rows (wrong-texel over-cull). Throttled explicit
	// fprintf (Release-visible).
	{
		const View& v = s.views[view];
		if ((hz.width != v.width || hz.height != v.height)
			&& v.width != 0 && v.height != 0) {
			static uint64_t s_hizExtentWarns = 0;
			if ((s_hizExtentWarns++ % 300) == 0) {
				std::fprintf(stderr,
					"[GfxHiz] extent mismatch: hiz %ux%u vs view %ux%u (view %u).\n",
					hz.width, hz.height, v.width, v.height, view);
			}
		}
	}
	HzbPush push{};
	push.hzb = addr;
	push.dstMip = dstMip;
	push.dstWidth = dw;
	push.dstHeight = dh;
	push.dstOffset = hz.mipOffsets[dstMip];
	push.depthTextureIndex = depthBindlessIndex;
	if (dstMip == 0) {
		push.srcMip = kHizSrcDepth;
		push.srcWidth = hz.width;
		push.srcHeight = hz.height;
		push.srcOffset = 0;
	}
	else {
		uint32_t sw = 0, sh = 0;
		HzbMipDims(hz.width, hz.height, dstMip - 1, sw, sh);
		push.srcMip = dstMip - 1;
		push.srcWidth = sw;
		push.srcHeight = sh;
		push.srcOffset = hz.mipOffsets[dstMip - 1];
	}

	// DEBUG: Log HZB dispatch parameters
	static uint64_t s_hizDispatchCount = 0;
	if ((s_hizDispatchCount++ % 60) == 0) {
		SMVK_DEBUG_LOG(
			"[GfxHiz] Dispatch view=%u mip=%u/%u dst=%ux%u off=%u srcMip=%u src=%ux%u srcOff=%u depthIdx=%u BDA=0x%llx\n",
			view, dstMip, hz.mipCount, dw, dh, push.dstOffset,
			push.srcMip, push.srcWidth, push.srcHeight, push.srcOffset,
			push.depthTextureIndex, (unsigned long long)addr);
	}

	cmd.BindComputePipeline(s.hizProgram);
	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	cmd.Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
}

RenderPass& AddHizSpdPass(State& s, RenderGraph& rg, uint32_t view,
	LogicalImageViewHandle depthLogical, const char* passName, bool shadow) {
	assert(view < s.temporals.size());
	assert(view < s.views.size());
	TemporalView& t = s.temporals[view];
	const uint32_t flight = s.views[view].frameIndex;
	HizResources& hz = shadow ? t.hizVerify[flight] : t.hiz[flight];
	assert(hz.mipCount != 0 && !hz.logical.empty());
	assert(!t.hizSpdCounterLogical.empty()); // caller gates on the ring

	SMVK_PROFILE_RENDER("GfxAddHizSpdPass");
	SMVK_PROFILE_TAG("mipCount", hz.mipCount);
	SMVK_PROFILE_TAG("shadow", shadow ? 1 : 0);

	RenderPass& pass = rg.AddComputePass(passName ? passName : "GfxHizSpd");
	{
		// The pyramid: read (footprints) + write (every level) within the
		// same dispatch — the same RW hint the per-mip passes carry.
		BufferSyncRequest hizRW{};
		hizRW.barrierMasks.accessMask =
			AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		hizRW.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(hz.logical, hizRW);
	}
	{
		// The completion counter: InterlockedAdd/Exchange from every
		// group (read-modify-write — atomics need the RW hint).
		BufferSyncRequest counterRW{};
		counterRW.barrierMasks.accessMask =
			AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		counterRW.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(t.hizSpdCounterLogical, counterRW);
	}
	{
		// The single dispatch ALWAYS reads the depth for its mip-0 copy
		// (DepthStencilReadOnlyOptimal matches the bindless descriptor
		// layout — the same note as AddHizPass).
		ImageSyncRequest depthReq{};
		depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
		depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
		depthReq.layout = ImageLayout::DepthStencilReadOnlyOptimal;
		if (!depthLogical.empty())
			pass.AddImageHint(depthLogical, depthReq);
	}
	return pass;
}

void PushAndDispatchHizSpd(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view, uint32_t depthBindlessIndex, bool shadow) {
	assert(view < s.temporals.size());
	assert(view < s.views.size());
	TemporalView& t = s.temporals[view];
	const uint32_t flight = s.views[view].frameIndex;
	HizResources& hz = shadow ? t.hizVerify[flight] : t.hiz[flight];
	if (hz.mipCount == 0 || hz.width == 0 || hz.height == 0)
		return;
	if (t.hizSpdCounter.empty() || t.hizSpdCounterLogical.empty()) {
		// No counter ring (EnsureHiz allocation failure): refuse the
		// dispatch — the caller's fallback should have routed per-mip,
		// this is the belt-and-suspenders guard.
		static uint64_t s_spdNoCounterWarns = 0;
		if ((s_spdNoCounterWarns++ % 300) == 0)
			std::fprintf(stderr,
				"[GfxHizSpd] refusing dispatch: no counter ring (view %u).\n",
				view);
		return;
	}

	SMVK_PROFILE_SCOPE("GfxPushAndDispatchHizSpd");
	SMVK_PROFILE_TAG("mipCount", hz.mipCount);
	SMVK_PROFILE_TAG("shadow", shadow ? 1 : 0);

	const uint64_t addr = ctx.GetBufferDeviceAddress(hz.buffer);
	// Null-BDA guard (the same device-lost lesson as PushAndDispatchHiz).
	if (addr == 0) {
		static uint64_t s_spdNullWarns = 0;
		if ((s_spdNullWarns++ % 300) == 0)
			std::fprintf(stderr,
				"[GfxHizSpd] refusing dispatch: null HZB address (view %u).\n",
				view);
		return;
	}
	if (depthBindlessIndex == UINT32_MAX) {
		static uint64_t s_spdDepthWarns = 0;
		if ((s_spdDepthWarns++ % 300) == 0)
			std::fprintf(stderr,
				"[GfxHizSpd] dispatch with invalid depth bindless (view %u): "
				"mip 0 content undefined, expect over-cull.\n",
				view);
	}
	const uint64_t counterAddr =
		ctx.GetBufferDeviceAddress(t.hizSpdCounter)
		+ (uint64_t)flight * kHizSpdCounterSliceBytes;
	const uint32_t groupX = (hz.width + 63u) / 64u;
	const uint32_t groupY = (hz.height + 63u) / 64u;

	HzbSpdPush push{};
	push.hzb = addr;
	push.counter = counterAddr;
	push.width = hz.width;
	push.height = hz.height;
	push.mipCount = hz.mipCount;
	push.depthTextureIndex = depthBindlessIndex;
	for (uint32_t k = 0; k < kHizMaxMips; ++k)
		push.mipOffsets[k] = hz.mipOffsets[k];
	push.groupCount = groupX * groupY;

	static uint64_t s_spdDispatchCount = 0;
	if ((s_spdDispatchCount++ % 60) == 0) {
		SMVK_DEBUG_LOG(
			"[GfxHizSpd] Dispatch view=%u shadow=%d %ux%u mips=%u "
			"groups=%ux%u depthIdx=%u BDA=0x%llx counter=0x%llx\n",
			view, shadow ? 1 : 0, hz.width, hz.height, hz.mipCount,
			groupX, groupY, depthBindlessIndex,
			(unsigned long long)addr, (unsigned long long)counterAddr);
	}

	cmd.BindComputePipeline(s.hizSpdProgram);
	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	cmd.Dispatch(groupX, groupY, 1);
}

void VerifyHizSpd(State& s, ResourceManager& ctx, uint32_t view) {
	if (view >= s.temporals.size() || view >= s.views.size())
		return;
	TemporalView& t = s.temporals[view];
	const uint32_t flight = s.views[view].frameIndex;
	const HizResources& hz = t.hiz[flight];
	const HizResources& hzV = t.hizVerify[flight];
	if (hz.mipCount == 0 || hzV.mipCount == 0 || hz.width != hzV.width
		|| hz.height != hzV.height || hz.mipCount != hzV.mipCount) {
		std::fprintf(stderr, "[GfxHizSpdVerify] pyramids unavailable or "
			"drifted - skipped\n");
		return;
	}
	const uint32_t w = hz.width;
	const uint32_t h = hz.height;
	const uint32_t mips = hz.mipCount;
	const size_t floats = (size_t)HzbTotalFloats(w, h);
	std::vector<float> perMip(floats, 0.0f);
	std::vector<float> spd(floats, 0.0f);
	if (!ctx.DownloadDataFromBuffer(hz.buffer, perMip.data(),
			floats * sizeof(float))
		|| !ctx.DownloadDataFromBuffer(hzV.buffer, spd.data(),
			floats * sizeof(float))) {
		std::fprintf(stderr, "[GfxHizSpdVerify] download failed - skipped\n");
		return;
	}
	uint64_t mismatches = 0;
	uint32_t mismatchedMips = 0;
	uint32_t printed = 0;
	constexpr uint32_t kPrintLimit = 8;
	for (uint32_t m = 0; m < mips; ++m) {
		uint32_t mw = 0, mh = 0;
		HzbMipDims(w, h, m, mw, mh);
		const uint32_t off = hz.mipOffsets[m];
		uint64_t mipMismatch = 0;
		for (uint32_t y = 0; y < mh; ++y) {
			for (uint32_t x = 0; x < mw; ++x) {
				const size_t i = (size_t)off + y * mw + x;
				if (perMip[i] != spd[i]) {
					++mipMismatch;
					if (printed < kPrintLimit) {
						++printed;
						std::fprintf(stderr,
							"[GfxHizSpdVerify]   mip %u (%u,%u): per-mip=%.6g spd=%.6g\n",
							m, x, y, perMip[i], spd[i]);
					}
				}
			}
		}
		if (mipMismatch != 0) {
			++mismatchedMips;
			mismatches += mipMismatch;
			std::fprintf(stderr,
				"[GfxHizSpdVerify]   mip %u (%ux%u): %llu mismatching texels\n",
				m, mw, mh, (unsigned long long)mipMismatch);
		}
	}
	++s.hizSpdVerifyFrames;
	s.hizSpdVerifyMismatches += mismatches;
	std::fprintf(stderr,
		"[GfxHizSpdVerify] view %u %ux%u mips=%u: %s"
		" (%llu mismatching texels across %u mips; running: %llu over %llu frames)\n",
		view, w, h, mips,
		mismatches == 0 ? "MATCH" : "MISMATCH",
		(unsigned long long)mismatches, mismatchedMips,
		(unsigned long long)s.hizSpdVerifyMismatches,
		(unsigned long long)s.hizSpdVerifyFrames);
}

// ---------------------------------------------------------------------------
// Registration (load-time)
// ---------------------------------------------------------------------------

static void AppendCluster(State& s, uint32_t mesh,
	const uint32_t* globalIds, uint32_t triCount,
	const glm::vec3& center, float radius,
	const glm::vec3& apex, const glm::vec3& axis, float cutoff) {
	Cluster cl{};
	cl.mesh = mesh;
	cl.indexOffset = (uint32_t)s.filterIndices.size();
	cl.triCount = triCount;
	cl.boundCenter = center;
	cl.boundRadius = radius;
	cl.coneApex = apex;
	cl.coneAxis = axis;
	cl.coneCutoff = cutoff;
	s.filterIndices.insert(s.filterIndices.end(), globalIds, globalIds + triCount * 3);
	s.clusters.push_back(cl);
}

// Expands one meshlet (local-id triangles + vertex window of upload-local
// ids) into heap-global ids. window = cooked.vertices + vertexOffset,
// baseVertex lifts upload-local ids to heap-global ids.
static void AppendMeshlet(State& s, uint32_t mesh, const Meshlet& m,
	const uint8_t* localTris, const uint32_t* window, uint32_t baseVertex) {
	if (m.triangleCount == 0 || m.triangleCount > kMaxClusterTris)
		return;
	std::vector<uint32_t> ids;
	ids.reserve(m.triangleCount * 3);
	for (uint32_t t = 0; t < m.triangleCount; ++t) {
		for (uint32_t k = 0; k < 3; ++k) {
			const uint32_t local = localTris[t * 3 + k];
			if (local >= m.vertexCount)
				return; // corrupt: drop the whole meshlet
			ids.push_back(baseVertex + window[local]);
		}
	}
	AppendCluster(s, mesh, ids.data(), m.triangleCount,
		m.boundCenter, m.boundRadius, m.coneApex, m.coneAxis, m.coneCutoff);
}

static uint32_t FinishMesh(State& s, uint32_t clusterStart) {
	Mesh entry{};
	entry.clusterStart = clusterStart;
	entry.clusterCount = (uint32_t)s.clusters.size() - clusterStart;
	// Union bound over the mesh's clusters (object space) for the
	// draw-level demand publisher (PublishDrawDemand projects it by the
	// draw's model, exactly like ClusterOccluded does with cluster
	// bounds). AABB of the cluster spheres; the half-diagonal radius
	// covers the box conservatively — a too-large bound only over-requests
	// texture detail (the safe direction), never misses a visible one.
	glm::vec3 mn(std::numeric_limits<float>::infinity());
	glm::vec3 mx(-std::numeric_limits<float>::infinity());
	for (uint32_t ci = clusterStart; ci < (uint32_t)s.clusters.size(); ++ci) {
		const Cluster& c = s.clusters[ci];
		mn = glm::min(mn, c.boundCenter - glm::vec3(c.boundRadius));
		mx = glm::max(mx, c.boundCenter + glm::vec3(c.boundRadius));
	}
	glm::vec4 bound(0.0f, 0.0f, 0.0f, 0.0f);
	if (!(mn.x <= mx.x))
		// NaN cluster bounds (cooker bug): force the untestable-sharpest
		// path (radius so large the eye is inside the sphere).
		bound = glm::vec4(0.0f, 0.0f, 0.0f, 1e30f);
	else
		bound = glm::vec4((mn + mx) * 0.5f, glm::length((mx - mn) * 0.5f));
	s.meshBounds.push_back(bound);
	s.meshes.push_back(entry);
	return (uint32_t)s.meshes.size() - 1;
}

uint32_t RegisterMeshFromUpload(State& s, const StaticMeshUpload& upload,
	uint32_t baseVertex) {
	if (upload.vertices.empty() || upload.indices.empty()
		|| (upload.indices.size() % 3) != 0)
		return UINT32_MAX;
	if (s.meshes.size() >= kMaxMeshes)
		return UINT32_MAX;
	const uint32_t clusterStart = (uint32_t)s.clusters.size();
	const uint32_t meshIndex = (uint32_t)s.meshes.size();

	MeshletConfig cfg{};
	cfg.enabled = true; // MeshletConfig defaults to off (cooker opts in)
	CookedMeshlets cooked{};
	const bool cookedOk = BuildMeshletsForUpload(upload, cfg, &cooked, nullptr, nullptr)
		&& !cooked.empty();

	bool useCooked = cookedOk;
	if (useCooked) {
		for (const Meshlet& m : cooked.meshlets) {
			if (m.triangleCount == 0 || m.triangleCount > kMaxClusterTris
				|| m.vertexCount == 0
				|| m.vertexOffset + m.vertexCount > (uint32_t)cooked.vertices.size()
				|| m.triangleOffset + m.triangleCount * 3 > (uint32_t)cooked.triangles.size()) {
				useCooked = false;
				break;
			}
		}
	}

	if (useCooked) {
		for (const Meshlet& m : cooked.meshlets) {
			AppendMeshlet(s, meshIndex, m,
				cooked.triangles.data() + m.triangleOffset,
				cooked.vertices.data() + m.vertexOffset, baseVertex);
		}
	}
	else {
		// Fallback: <=128-tri chunks with AABB-sphere bounds; cone cutoff
		// 1.0 disables the cone test (matches meshopt degenerate bounds).
		s.clusters.reserve(s.clusters.size() + upload.indices.size() / (kMaxClusterTris * 3) + 1);
		const uint32_t triTotal = (uint32_t)upload.indices.size() / 3;
		for (uint32_t first = 0; first < triTotal; first += kMaxClusterTris) {
			const uint32_t count = std::min(kMaxClusterTris, triTotal - first);
			glm::vec3 mn(std::numeric_limits<float>::infinity());
			glm::vec3 mx(-std::numeric_limits<float>::infinity());
			std::vector<uint32_t> ids;
			ids.reserve(count * 3);
			for (uint32_t t = 0; t < count; ++t) {
				for (uint32_t k = 0; k < 3; ++k) {
					const uint32_t local = upload.indices[(first + t) * 3 + k];
					ids.push_back(baseVertex + local);
					if (local < upload.vertices.size()) {
						const glm::vec3& p = upload.vertices[local].pos;
						mn = glm::min(mn, p);
						mx = glm::max(mx, p);
					}
				}
			}
			const glm::vec3 center = (mn + mx) * 0.5f;
			AppendCluster(s, meshIndex, ids.data(), count, center,
				glm::length(mx - center), center,
				glm::vec3(0.0f, 1.0f, 0.0f), 1.0f);
		}
	}

	if (s.clusters.size() == clusterStart)
		return UINT32_MAX; // nothing emitted
	return FinishMesh(s, clusterStart);
}

uint32_t RegisterMeshFromCooked(State& s, const CookedMeshlets& cooked,
	uint32_t baseVertex) {
	if (cooked.empty() || s.meshes.size() >= kMaxMeshes)
		return UINT32_MAX;
	const uint32_t clusterStart = (uint32_t)s.clusters.size();
	const uint32_t meshIndex = (uint32_t)s.meshes.size();
	for (const Meshlet& m : cooked.meshlets) {
		if (m.triangleCount == 0 || m.triangleCount > kMaxClusterTris
			|| m.vertexCount == 0)
			continue;
		if (m.vertexOffset + m.vertexCount > (uint32_t)cooked.vertices.size())
			continue;
		if (m.triangleOffset + m.triangleCount * 3 > (uint32_t)cooked.triangles.size())
			continue;
		// Local ids index the meshlet's vertex window (upload-local ids);
		// lift to heap-global ids with baseVertex.
		const uint32_t* window = cooked.vertices.data() + m.vertexOffset;
		const uint8_t* localTris = cooked.triangles.data() + m.triangleOffset;
		std::vector<uint32_t> globalIds;
		globalIds.reserve(m.triangleCount * 3);
		bool valid = true;
		for (uint32_t i = 0; i < m.triangleCount * 3; ++i) {
			if (localTris[i] >= m.vertexCount) {
				valid = false;
				break;
			}
			globalIds.push_back(baseVertex + window[localTris[i]]);
		}
		if (!valid)
			continue;
		AppendCluster(s, meshIndex, globalIds.data(), m.triangleCount,
			m.boundCenter, m.boundRadius, m.coneApex, m.coneAxis, m.coneCutoff);
	}
	if (s.clusters.size() == clusterStart)
		return UINT32_MAX;
	return FinishMesh(s, clusterStart);
}

void ResetStaticRegistrations(State& s) {
	s.filterIndices.clear();
	s.clusters.clear();
	s.meshes.clear();
	s.meshBounds.clear();
	// clusterIndexBase/AllocBits + the three table buffers stay - they
	// are UploadStaticData's to retire/replace (alloc-new-first, the
	// old region frees deferred).
}

void UploadStaticData(State& s, ResourceManager& ctx, RenderGraph& rg) {
	if (!s.clusterLogical.empty()) rg.FreeBuffer(s.clusterLogical);
	s.clusterLogical = {};
	s.clusterBuffer = {};
	if (!s.meshBoundsLogical.empty()) rg.FreeBuffer(s.meshBoundsLogical);
	s.meshBoundsLogical = {};
	s.meshBoundsBuffer = {};
	// The cluster-index region: the NEW region allocates FIRST and the
	// old one retires DEFERRED (DeferFreeIndexRegion) - never free-then-
	// realloc in place. The streaming promote path rebuilds between
	// frames with up to MaxFramesInFlight flights still reading the old
	// region through the old clusterIndexBase; alloc-first also means a
	// failed re-allocation keeps the OLD region live instead of losing
	// it (the pre-streaming code freed first and dropped the range on
	// the exhaustion path).
	if (s.indexHeapLogical.empty())
		s.indexHeapLogical = rg.AddExternalBuffer(
			staticVertexFmtHeap.m_staticIndexBuffer, "static index monoblock");
	uint32_t newIndexBase = 0;
	uint64_t newIndexBits = 0;
	if (!s.filterIndices.empty() && !s.clusters.empty()
		&& AllocIndexRegion((uint32_t)s.filterIndices.size(),
			newIndexBase, newIndexBits)) {
		DeferFreeIndexRegion(ctx, s.clusterIndexAllocBits);
		s.clusterIndexBase = newIndexBase;
		s.clusterIndexAllocBits = newIndexBits;
	}
	else if (s.filterIndices.empty() || s.clusters.empty()) {
		// Nothing to upload: retire the old region too (deferred).
		DeferFreeIndexRegion(ctx, s.clusterIndexAllocBits);
		s.clusterIndexBase = 0;
	}
	else {
		// Region exhausted: KEEP the old region (it still backs the
		// previous content - the tables below were already cleared, so
		// the filter reports unavailable, same failure mode as before,
		// but the range is not leaked).
		std::fprintf(stderr,
			"[GfxFilter] cluster index region exhausted the static index "
			"monoblock (%u indices): increase the reservation.\n",
			(unsigned)s.filterIndices.size());
		return;
	}
	if (s.filterIndices.empty() || s.clusters.empty())
		return;
	std::vector<ClusterConstants> table(s.clusters.size());
	for (size_t i = 0; i < s.clusters.size(); ++i) {
		table[i].indexOffset = s.clusters[i].indexOffset;
		table[i].triCount = s.clusters[i].triCount;
		table[i].boundCenter = s.clusters[i].boundCenter;
		table[i].boundRadius = s.clusters[i].boundRadius;
		table[i].coneApex = s.clusters[i].coneApex;
		table[i].coneAxis = s.clusters[i].coneAxis;
		table[i].coneCutoff = s.clusters[i].coneCutoff;
	}
	s.clusterBuffer = ctx.CreateBuffer(
		table.size() * sizeof(ClusterConstants),
		BufferType_Storage, "gfx clusters");
	if (s.clusterBuffer.empty())
		return;
	ctx.UploadDataToBuffer(staticVertexFmtHeap.m_staticIndexBuffer,
		s.filterIndices.data(), s.filterIndices.size() * sizeof(uint32_t),
		PipelineStage(PipelineStageBit_Vertex), AccessFlags::IndexRead,
		0, (size_t)s.clusterIndexBase * sizeof(uint32_t));
	ctx.UploadDataToBuffer(s.clusterBuffer,
		table.data(), table.size() * sizeof(ClusterConstants),
		PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	s.clusterLogical = rg.AddExternalBuffer(s.clusterBuffer, "gfx clusters");
	// Per-mesh union bounds (static like the cluster table; the designated
	// demand-publisher groups and the GPU cull chain's draw gates read them
	// every dispatch).
	if (!s.meshBounds.empty()) {
		s.meshBoundsBuffer = ctx.CreateBuffer(
			s.meshBounds.size() * sizeof(glm::vec4),
			BufferType_Storage, "gfx mesh bounds");
		if (!s.meshBoundsBuffer.empty()) {
			ctx.UploadDataToBuffer(s.meshBoundsBuffer,
				s.meshBounds.data(), s.meshBounds.size() * sizeof(glm::vec4),
				PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
			s.meshBoundsLogical = rg.AddExternalBuffer(
				s.meshBoundsBuffer, "gfx mesh bounds");
		}
	}
	// Per-mesh cluster ranges (the GPU cull chain's workgroup-per-draw
	// derives each draw's cluster window from it; static like the rest).
	if (!s.meshesLogical.empty()) rg.FreeBuffer(s.meshesLogical);
	s.meshesLogical = {};
	s.meshesBuffer = {};
	if (!s.meshes.empty()) {
		s.meshesBuffer = ctx.CreateBuffer(
			s.meshes.size() * 2 * sizeof(uint32_t),
			BufferType_Storage, "gfx mesh ranges");
		if (!s.meshesBuffer.empty()) {
			std::vector<uint32_t> meshRanges(s.meshes.size() * 2);
			for (size_t i = 0; i < s.meshes.size(); ++i) {
				meshRanges[i * 2 + 0] = s.meshes[i].clusterStart;
				meshRanges[i * 2 + 1] = s.meshes[i].clusterCount;
			}
			ctx.UploadDataToBuffer(s.meshesBuffer,
				meshRanges.data(), meshRanges.size() * sizeof(uint32_t),
				PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
			s.meshesLogical = rg.AddExternalBuffer(
				s.meshesBuffer, "gfx mesh ranges");
		}
	}
}

// ---------------------------------------------------------------------------
// Per-frame views
// ---------------------------------------------------------------------------

void BeginFrame(State& s) {
	s.viewCount = 0;
	// Kept-parity source: one BeginFrame per rendered frame
	// (FilterMainView), so the parity alternates between a flight's
	// consecutive users exactly smvk::limits::MaxFramesInFlight frames apart.
	++s.frameCounter;
}

static bool EnsureViewSlots(State& s, ResourceManager& ctx, RenderGraph& rg, uint32_t count) {
	if (count > kMaxViews)
		return false;
	while ((uint32_t)s.views.size() < count) {
		View slot{};
		if (!CreateSlotBuffers(ctx, rg, slot))
			return false;
		TemporalView temporal{};
		if (!CreateTemporalBuffers(ctx, rg, temporal)) {
			// Unwind the view slot we just allocated (all its ring slices).
			RollbackSlotBuffers(rg, slot, smvk::limits::MaxFramesInFlight);
			return false;
		}
		s.views.push_back(std::move(slot));
		s.temporals.push_back(std::move(temporal));
	}
	return true;
}

void InvalidateHistory(State& s, uint32_t view) {
	if (view >= s.temporals.size())
		return;
	s.temporals[view].historyValid = false;
}

// Key-sorted (cluster, model) entries for one batch list (same skips the
// pre-merge VisKey paths made: invalid draw index, invalid cluster).
// batch keeps the ORIGINAL list slot so the emit walks below preserve
// BuildBatches' material grouping (ranges stay coalesced).
static void BuildVisKeys(const std::vector<Batch>& batches,
	const std::vector<Draw>& draws, uint32_t clusterCount,
	std::vector<detail::VisKeyEntry>& out) {
	out.clear();
	out.reserve(batches.size());
	for (uint32_t i = 0; i < (uint32_t)batches.size(); ++i) {
		const Batch& b = batches[i];
		if (b.draw >= (uint32_t)draws.size())
			continue;
		if (b.cluster >= clusterCount)
			continue;
		detail::VisKeyEntry e{};
		e.key = detail::MakeVisKey(draws[b.draw].model, b.cluster);
		e.batch = i;
		out.push_back(e);
	}
	std::sort(out.begin(), out.end(),
		[](const detail::VisKeyEntry& a, const detail::VisKeyEntry& b) {
			return a.key < b.key;
		});
}

void SnapshotVisible(State& s, uint32_t view) {
	assert(view < s.views.size());
	GfxProbeScope probeScope(g_probe.snap); // TMP-PROBE
	TemporalView& t = Temporal(s, view);
	const View& v = s.views[view];

	SMVK_PROFILE_SCOPE("GfxSnapshotVisible");
	SMVK_PROFILE_TAG("batches", v.batches.size());
	SMVK_PROFILE_TAG("draws", v.drawConstants.size());

	t.prevBatches = v.batches;
	t.prevDraws = v.drawConstants;
	t.prevRanges = v.materialRanges;
	t.prevBatchCount = v.batchCount;
	t.prevDrawCount = (uint32_t)v.drawConstants.size();
	t.width = v.width;
	t.height = v.height;
	// Key-sorted (cluster, model) identities of the new snapshot: reuse
	// this frame's curr keys when a builder already sorted them (they
	// index the same batch list prevBatches just copied), else sort.
	// Swap keeps both vectors' capacity alive across frames.
	if (t.currKeysSerial == v.batchesSerial) {
		t.prevKeys.swap(t.currKeys);
		t.currKeys.clear();
		t.currKeysSerial = UINT64_MAX;
	} else {
		BuildVisKeys(v.batches, v.drawConstants, (uint32_t)s.clusters.size(), t.prevKeys);
	}
	// historyValid flips true here; the seed pass (T3) consumes prev* next
	// frame. First snapshot after InvalidateHistory re-arms history.
	t.historyValid = true;
}

void UploadVisible(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view) {
	assert(view < s.views.size());
	assert(frameIndex < smvk::limits::MaxFramesInFlight);
	if (view >= s.temporals.size())
		return;
	TemporalView& t = s.temporals[view];
	if (t.visClusterBuffers[frameIndex].empty() || t.visDrawBuffers[frameIndex].empty())
		return; // headless / unallocated: CPU snapshot still stands
	GfxProbeScope probeScope(g_probe.uploads); // TMP-PROBE

	SMVK_PROFILE_SCOPE("GfxUploadVisible");
	SMVK_PROFILE_TAG("prevBatchCount", t.prevBatchCount);
	SMVK_PROFILE_TAG("prevDrawCount", t.prevDrawCount);

	if (t.prevBatchCount > 0) {
		SMVK_PROFILE_SCOPE("GfxUploadVisClusters");
		t.visPairScratch.clear();
		t.visPairScratch.reserve(t.prevBatchCount);
		for (const Batch& b : t.prevBatches)
			t.visPairScratch.push_back(VisibleCluster{ b.cluster, b.draw });
		ctx.UploadDataToBuffer(t.visClusterBuffers[frameIndex],
			t.visPairScratch.data(), t.visPairScratch.size() * sizeof(VisibleCluster),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	}
	if (!t.prevDraws.empty()) {
		SMVK_PROFILE_SCOPE("GfxUploadVisDraws");
		ctx.UploadDataToBuffer(t.visDrawBuffers[frameIndex],
			t.prevDraws.data(), t.prevDraws.size() * sizeof(Draw),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	}
}

// Shared bucket-major emitter: opaque commands first (single VS-only
// multi-draw), then masked (per-material ranges for the alpha-test FS
// push). Runs coalesce on (bucket, material).
static void EmitBucketMajor(State& s, const std::vector<detail::EmitEntry>& entries,
	std::vector<SeedCommand>& outCmds, std::vector<SeedRange>& outRanges,
	uint32_t& outOpaque) {
	outCmds.clear();
	outRanges.clear();
	outOpaque = 0;
	for (uint32_t pass = 0; pass < 2; ++pass) {
		const uint32_t wantSet = (pass == 0) ? DrawSetBit_Opaque : DrawSetBit_Masked;
		for (const detail::EmitEntry& e : entries) {
			if (e.cluster >= (uint32_t)s.clusters.size())
				continue;
			const bool masked = (e.drawSet & DrawSetBit_Masked) != 0;
			if ((wantSet == DrawSetBit_Masked) != masked)
				continue;
		const Cluster& c = s.clusters[e.cluster];
		SeedCommand cmd{};
		cmd.indexCount = c.triCount * 3;
		cmd.instanceCount = 1;
		// Absolute into the static index monoblock (region base + offset).
		cmd.firstIndex = s.clusterIndexBase + c.indexOffset;
		cmd.vertexOffset = 0;
		cmd.firstInstance = e.draw;
		const uint32_t slot = (uint32_t)outCmds.size();
		if (slot >= kMaxBatches) {
			static bool warnedSeedCap = false;
			if (!warnedSeedCap) {
				warnedSeedCap = true;
				SMVK_DEBUG_LOG(
					"[GfxFilter] seed emitter: batch cap %u hit, dropping "
					"commands (increase kMaxBatches).\n", kMaxBatches);
			}
			break;
		}
			outCmds.push_back(cmd);
			if (!outRanges.empty()) {
				SeedRange& tail = outRanges.back();
				if (tail.drawSet == wantSet && tail.material == e.material
					&& tail.firstCommand + tail.commandCount == slot) {
					++tail.commandCount;
					continue;
				}
			}
			SeedRange r{};
			r.drawSet = wantSet;
			r.material = e.material;
			r.firstCommand = slot;
			r.commandCount = 1;
			outRanges.push_back(r);
		}
		if (pass == 0)
			outOpaque = (uint32_t)outCmds.size();
	}
}

// Material per batch from covering ranges (ranges span batches in order).
static void ResolveBatchMaterials(const std::vector<MaterialRange>& ranges,
	size_t batchCount, std::vector<uint32_t>& out) {
	out.assign(batchCount, UINT32_MAX);
	for (const MaterialRange& r : ranges) {
		for (uint32_t k = 0; k < r.commandCount; ++k) {
			if (r.firstCommand + k < (uint32_t)out.size())
				out[r.firstCommand + k] = r.material;
		}
	}
}

// Seed ONLY the intersection of the snapshot with this frame's batches
// (same cluster, same transform). The prepass Loads seeded depth and
// draws full current geometry over it (nearest-wins), and 1x Main then
// tests read-only (GreaterEqual) against that buffer — so any stale
// seed content that is NEARER than current geometry kills Main pixels
// (skybox holes for a frame). Stale == LOD flip (sibling clusters have
// disjoint ids) or moved instance (model differs): both miss the
// intersection and fall through to the prepass + remainder instead.
// Same cluster + same model replays bit-identical depth, which can
// only ever tie (pass) — never poison.
//
// One sorted-key merge (detail::MergeKeyMasks) yields membership masks
// for both outputs; the emit walks keep the ORIGINAL batch order so
// material grouping (BuildBatches sorts draws by material) leaves the
// (bucket, material) ranges coalesced exactly as before. All scratch
// lives in TemporalView: the steady-state rebuild allocates nothing.
static void BuildBothCommands(State& s, uint32_t view, bool wantSeed, bool wantRemainder) {
	assert(view < s.views.size());
	GfxProbeScope probeScope(g_probe.seedRem); // TMP-PROBE
	TemporalView& t = Temporal(s, view);
	const View& v = s.views[view];

	if (wantSeed) {
		t.seedCommands.clear();
		t.seedRanges.clear();
		t.seedOpaqueCommands = 0;
	}
	if (wantRemainder) {
		t.remainderCommands.clear();
		t.remainderRanges.clear();
		t.remainderOpaqueCommands = 0;
	}
	// Same guards as the pre-merge builders.
	const bool doSeed = wantSeed && t.historyValid && !t.prevBatches.empty();
	const bool doRemainder = wantRemainder && t.historyValid && !v.batches.empty();
	if (!doSeed && !doRemainder)
		return;

	SMVK_PROFILE_RENDER("GfxBuildSeedAndRemainder");
	SMVK_PROFILE_TAG("currBatches", v.batches.size());
	SMVK_PROFILE_TAG("prevBatches", t.prevBatches.size());

	// This frame's sorted curr keys: one sort serves both outputs and is
	// rotated into the snapshot by SnapshotVisible (serial-gated; the
	// memo never holds hand-built serial 0).
	if (t.currKeysSerial != v.batchesSerial) {
		BuildVisKeys(v.batches, v.drawConstants, (uint32_t)s.clusters.size(), t.currKeys);
		t.currKeysSerial = v.batchesSerial != 0 ? v.batchesSerial : UINT64_MAX;
	}

	// Membership masks over the ORIGINAL batch lists (zero default:
	// entries absent from the key arrays — invalid draw/cluster — never
	// seed and never remainder, like the pre-merge guards).
	t.seedMaskScratch.assign(t.prevBatches.size(), 0);
	t.remainderMaskScratch.assign(v.batches.size(), 0);
	const auto modelEq = [&](uint32_t prevBatch, uint32_t currBatch) {
		const glm::mat4& prevModel = t.prevDraws[t.prevBatches[prevBatch].draw].model;
		const glm::mat4& currModel = v.drawConstants[v.batches[currBatch].draw].model;
		return std::memcmp(&prevModel, &currModel, sizeof(glm::mat4)) == 0;
	};
	detail::MergeKeyMasks(t.prevKeys.data(), t.prevKeys.size(),
		t.currKeys.data(), t.currKeys.size(), modelEq,
		t.seedMaskScratch.data(), t.remainderMaskScratch.data());

	if (doSeed) {
		ResolveBatchMaterials(t.prevRanges, t.prevBatches.size(), t.batchMaterialScratch);
		t.emitScratch.clear();
		t.emitScratch.reserve(t.prevBatches.size());
		for (uint32_t i = 0; i < (uint32_t)t.prevBatches.size(); ++i) {
			if (!t.seedMaskScratch[i])
				continue; // LOD-flipped or moved since: prepass owns it
			const Batch& b = t.prevBatches[i];
			detail::EmitEntry e{};
			e.cluster = b.cluster;
			e.draw = b.draw; // into the visDraws snapshot
			e.material = t.batchMaterialScratch[i];
			e.drawSet = t.prevDraws[b.draw].drawSet;
			t.emitScratch.push_back(e);
		}
		EmitBucketMajor(s, t.emitScratch, t.seedCommands, t.seedRanges, t.seedOpaqueCommands);
	}
	if (doRemainder) {
		ResolveBatchMaterials(v.materialRanges, v.batches.size(), t.batchMaterialScratch);
		t.emitScratch.clear();
		t.emitScratch.reserve(v.batches.size());
		for (uint32_t i = 0; i < (uint32_t)v.batches.size(); ++i) {
			if (!t.remainderMaskScratch[i])
				continue; // seeded already: same cluster, same transform
			const Batch& b = v.batches[i];
			detail::EmitEntry e{};
			e.cluster = b.cluster;
			e.draw = b.draw; // into the CURRENT draw constants (DrawBuffer)
			e.material = t.batchMaterialScratch[i];
			e.drawSet = v.drawConstants[b.draw].drawSet;
			t.emitScratch.push_back(e);
		}
		EmitBucketMajor(s, t.emitScratch,
			t.remainderCommands, t.remainderRanges, t.remainderOpaqueCommands);
	}
}

void BuildSeedCommands(State& s, uint32_t view) {
	BuildBothCommands(s, view, true, false);
}

void BuildSeedAndRemainder(State& s, uint32_t view) {
	BuildBothCommands(s, view, true, true);
}

void BuildRemainderCommands(State& s, uint32_t view) {
	BuildBothCommands(s, view, false, true);
}

void UploadSeed(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view) {
	assert(view < s.views.size());
	assert(frameIndex < smvk::limits::MaxFramesInFlight);
	if (view >= s.temporals.size())
		return;
	TemporalView& t = s.temporals[view];
	if (t.seedIndirectBuffers[frameIndex].empty())
		return;
	if (t.seedCommands.empty())
		return;
	GfxProbeScope probeScope(g_probe.uploads); // TMP-PROBE

	SMVK_PROFILE_SCOPE("GfxUploadSeed");
	SMVK_PROFILE_TAG("commands", t.seedCommands.size());

	ctx.UploadDataToBuffer(t.seedIndirectBuffers[frameIndex],
		t.seedCommands.data(), t.seedCommands.size() * sizeof(SeedCommand),
		PipelineStage(PipelineStageBit_DrawIndirect), AccessFlags::IndirectCommandRead);
}

void UploadRemainder(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view) {
	assert(view < s.views.size());
	assert(frameIndex < smvk::limits::MaxFramesInFlight);
	if (view >= s.temporals.size())
		return;
	TemporalView& t = s.temporals[view];
	if (t.remainderIndirectBuffers[frameIndex].empty())
		return;
	if (t.remainderCommands.empty())
		return;
	GfxProbeScope probeScope(g_probe.uploads); // TMP-PROBE

	SMVK_PROFILE_SCOPE("GfxUploadRemainder");
	SMVK_PROFILE_TAG("commands", t.remainderCommands.size());

	ctx.UploadDataToBuffer(t.remainderIndirectBuffers[frameIndex],
		t.remainderCommands.data(), t.remainderCommands.size() * sizeof(SeedCommand),
		PipelineStage(PipelineStageBit_DrawIndirect), AccessFlags::IndirectCommandRead);
}

RenderPass& AddSeedPass(State& s, RenderGraph& rg, uint32_t view,
	LogicalImageViewHandle depthLogical) {
	assert(view < s.temporals.size());
	TemporalView& t = s.temporals[view];
	const View& v = s.views[view];
	RenderPass& pass = rg.AddPass("GfxSeedDepth");
	Extent2D extent{ v.width, v.height };
	pass.SetOffset(0, 0)
		.SetExtent(extent)
		.SetDepth(depthLogical,
			AttachmentLoadOp::Clear,
			AttachmentStoreOp::Store,
			ClearDepthStencil(0.0f, 0));
	BufferSyncRequest indirectRead{};
	indirectRead.barrierMasks.accessMask = AccessFlags::IndirectCommandRead;
	indirectRead.barrierMasks.stageMask = PipelineStageBit_DrawIndirect;
	pass.AddBufferHint(t.seedIndirectLogicals[v.frameIndex], indirectRead);
	return pass;
}

// Depth pass 2 (T5): draws the remainder list over seeded depth
// (Load/Store). Same consume pattern as the seed pass.
RenderPass& AddRemainderPass(State& s, RenderGraph& rg, uint32_t view,
	LogicalImageViewHandle depthLogical) {
	assert(view < s.temporals.size());
	TemporalView& t = s.temporals[view];
	const View& v = s.views[view];
	RenderPass& pass = rg.AddPass("GfxRemainderDepth");
	Extent2D extent{ v.width, v.height };
	pass.SetOffset(0, 0)
		.SetExtent(extent)
		.SetDepth(depthLogical,
			AttachmentLoadOp::Load,
			AttachmentStoreOp::Store,
			ClearDepthStencil(0.0f, 0));
	BufferSyncRequest indirectRead{};
	indirectRead.barrierMasks.accessMask = AccessFlags::IndirectCommandRead;
	indirectRead.barrierMasks.stageMask = PipelineStageBit_DrawIndirect;
	pass.AddBufferHint(t.remainderIndirectLogicals[v.frameIndex], indirectRead);
	return pass;
}

void AddSeedConsumeHints(RenderPass& pass,
	LogicalBufferHandle indexLogical, LogicalBufferHandle visDrawLogical,
	LogicalBufferHandle seedIndirectLogical) {
	BufferSyncRequest indexRead{};
	indexRead.barrierMasks.accessMask = AccessFlags::IndexRead;
	indexRead.barrierMasks.stageMask = PipelineStageBit_Vertex;
	pass.AddBufferHint(indexLogical, indexRead);
	if (!visDrawLogical.empty()) {
		BufferSyncRequest drawsRead{};
		drawsRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		drawsRead.barrierMasks.stageMask = PipelineStageBit_Vertex;
		pass.AddBufferHint(visDrawLogical, drawsRead);
	}
	if (!seedIndirectLogical.empty()) {
		BufferSyncRequest indirectRead{};
		indirectRead.barrierMasks.accessMask = AccessFlags::IndirectCommandRead;
		indirectRead.barrierMasks.stageMask = PipelineStageBit_DrawIndirect;
		pass.AddBufferHint(seedIndirectLogical, indirectRead);
	}
}

uint32_t BeginView(State& s, ResourceManager& ctx, RenderGraph& rg,
	const RenderView& renderView, const glm::vec3& eye,
	CullFlags cullFlags, uint32_t width, uint32_t height) {
	GfxProbeReport(); // TMP-PROBE: reports previous 240 frames
	const uint32_t slot = s.viewCount;
	if (!EnsureViewSlots(s, ctx, rg, slot + 1))
		return UINT32_MAX;
	View& v = s.views[slot];
	v.eye = eye;
	v.cullFlags = cullFlags;
	v.width = width;
	v.height = height;
	v.viewProj = renderView.m_viewProjectionMatrix;
	v.renderView = &renderView;
	v.draws.clear();
	v.batches.clear();
	v.drawConstants.clear();
	v.materialRanges.clear();
	v.cullRanges.clear();
	v.cullOpaqueCommands = 0;
	v.batchCount = 0;
	v.stats = Stats{};
	// Fresh stamp: cached curr keys from a previous frame of this slot
	// are now invalid (the lists were just cleared).
	v.batchesSerial = s.nextBatchesSerial++;
	++s.viewCount;
	return slot;
}

void PushDraw(State& s, uint32_t view, uint32_t mesh,
	const glm::mat4& model, uint32_t material, DrawFlags flags,
	DrawSetMask drawSet, uint32_t lmChunk, uint32_t lmChartOffset,
	uint32_t lmDelta) {
	assert(view < s.viewCount);
	assert(mesh < s.meshes.size());
	View& v = s.views[view];
	if (v.draws.size() >= kMaxDraws) {
		static bool warnedDrawCap = false;
		if (!warnedDrawCap) {
			warnedDrawCap = true;
			std::fprintf(stderr,
				"[GfxFilter] PushDraw: draw cap %u hit, dropping draws "
				"(increase kMaxDraws).\n", kMaxDraws);
		}
		return;
	}
	DrawRequest d{};
	d.mesh = mesh;
	d.material = material;
	d.model = model;
	d.flags = flags;
	d.drawSet = drawSet;
	d.lmChunk = lmChunk;
	d.lmChartOffset = lmChartOffset;
	d.lmDelta = lmDelta;
	v.draws.push_back(d);
}

static float MaxBasisScale(const glm::mat4& m) {
	return std::max({
		glm::length(glm::vec3(m[0])),
		glm::length(glm::vec3(m[1])),
		glm::length(glm::vec3(m[2])) });
}

// CPU pre-cull port of GeometryFX's cluster test (AddRequest cone check)
// using meshoptimizer perspective formulas (see meshoptimizer.h:
// dot(normalize(apex - eye), axis) >= cutoff culls).
// The cone test runs in world space so mirrored / non-uniform-scale
// instances agree with the rasterizer (object-space testing flips the
// answer under negative-determinant transforms).
bool CullCluster(const Cluster& c, const glm::mat4& model,
	const glm::mat4& invModel, const glm::vec3& eye,
	const RenderView& renderView) {
	const glm::vec3 worldCenter = glm::vec3(model * glm::vec4(c.boundCenter, 1.0f));
	const float worldRadius = c.boundRadius * MaxBasisScale(model);
	if (!RenderViewIntersectsSphere(renderView, worldCenter, worldRadius))
		return true;
	const glm::vec3 worldApex = glm::vec3(model * glm::vec4(c.coneApex, 1.0f));
	// Normal transform (inverse-transpose, direction only, renormalized).
	const glm::vec3 axisW =
		glm::vec3(glm::transpose(invModel) * glm::vec4(c.coneAxis, 0.0f));
	const float axisLen2 = glm::dot(axisW, axisW);
	if (axisLen2 < 1e-12f)
		return false; // degenerate cone (e.g. fallback clusters): no cone cull
	const glm::vec3 worldAxis = axisW / std::sqrt(axisLen2);
	const glm::vec3 d = worldApex - eye;
	const float len2 = glm::dot(d, d);
	if (len2 < 1e-12f)
		return false; // eye inside the apex: keep (avoids NaN)
	if (glm::dot(d / std::sqrt(len2), worldAxis) >= c.coneCutoff)
		return true;
	return false;
}

// CPU reference of GfxSkinBounds' cone rebuild (gfxskinbounds.cs.hlsl):
// the exact math the shader's three per-cluster walks run, on caller-supplied
// (deformed) triangle corners — meshopt_computeMeshletBounds' cone convention
// (meshletutils.cpp), the same one the baked static cones follow, so
// GfxConeCulled consumes either without knowing which fit made it:
//   axis  = normalize(sum of raw cross products) — area-weighted (any axis
//           + min-dot cutoff is conservative; only tightness varies)
//   mindp = min dot(axis, unit face normal) over non-degenerate triangles
//   degenerate guards: triCount == 0 / axis ~0 / mindp <= 0.1 (cone wider
//           than ~168 degrees — meshopt's own "isn't useful" cutoff)
//   maxt  = max(0, dot(center - p0, n) / dot(axis, n)) — the apex lands
//           behind every face plane by construction (denominator >= mindp
//           > 0.1 once the guards pass)
//   cutoff = sqrt(1 - mindp^2) — the 90-degrees-both-sides widening the
//           perspective camera test requires
// Exported for the unit tests (test_gfxfilter pins the fit's verdicts
// against the every-face-plane-negative ground truth).
DeformedConeFit FitDeformedCone(const glm::vec3* triCorners,
	uint32_t triCount, const glm::vec3& sphereCenter)
{
	DeformedConeFit out{}; // degenerate defaults: the keep-cone
	if (triCorners == nullptr || triCount == 0)
		return out;
	glm::vec3 nsum(0.0f);
	for (uint32_t t = 0; t < triCount; ++t) {
		const glm::vec3& p0 = triCorners[t * 3 + 0];
		const glm::vec3& p1 = triCorners[t * 3 + 1];
		const glm::vec3& p2 = triCorners[t * 3 + 2];
		nsum += glm::cross(p1 - p0, p2 - p0);
	}
	const float nlen2 = glm::dot(nsum, nsum);
	if (nlen2 < 1e-12f)
		return out;
	const glm::vec3 axis = nsum / std::sqrt(nlen2);
	float mindp = 1.0f;
	for (uint32_t t = 0; t < triCount; ++t) {
		const glm::vec3 n = glm::cross(
			triCorners[t * 3 + 1] - triCorners[t * 3 + 0],
			triCorners[t * 3 + 2] - triCorners[t * 3 + 0]);
		const float l2 = glm::dot(n, n);
		if (l2 < 1e-24f)
			continue; // zero-area sliver: shades nothing
		mindp = std::min(mindp, glm::dot(axis, n / std::sqrt(l2)));
	}
	if (mindp <= 0.1f)
		return out;
	float maxt = 0.0f;
	for (uint32_t t = 0; t < triCount; ++t) {
		const glm::vec3 nRaw = glm::cross(
			triCorners[t * 3 + 1] - triCorners[t * 3 + 0],
			triCorners[t * 3 + 2] - triCorners[t * 3 + 0]);
		const float l2 = glm::dot(nRaw, nRaw);
		if (l2 < 1e-24f)
			continue;
		const glm::vec3 n = nRaw / std::sqrt(l2);
		const float dn = glm::dot(axis, n);
		const float dc = glm::dot(sphereCenter - triCorners[t * 3 + 0], n);
		maxt = std::max(maxt, dc / dn);
	}
	const float m = std::min(mindp, 1.0f);
	out.coneApex = sphereCenter - axis * maxt;
	out.coneAxis = axis;
	out.coneCutoff = std::sqrt(1.0f - m * m);
	out.degenerate = false;
	return out;
}

// Mesh-bound fast-discard gate for shadow lists (pure CPU, headless-
// testable; the CPU twin of the main view's GPU draw cull): true = the
// whole draw leaves before the per-cluster meshlet loop. The bound is
// the per-mesh union sphere (State::meshBounds, object space — the same
// containment source the demand publisher and GfxDrawCull use; a draw
// bound that misses means every cluster bound misses too).
bool CullShadowMeshBound(const glm::vec4& meshBound, const glm::mat4& model,
	const RenderView& shadowView, const RenderView* contributionView) {
	const glm::vec3 worldCenter =
		glm::vec3(model * glm::vec4(glm::vec3(meshBound), 1.0f));
	const float worldRadius = meshBound.w * MaxBasisScale(model);
	if (!RenderViewIntersectsSphere(shadowView, worldCenter, worldRadius))
		return true; // fully outside the shadow view's frustum
	// Contribution: a caster that cannot touch the main camera's SHADE
	// volume (the caller passes the main view swept toward the sun - see
	// MakeCasterContributionView; spot callers pass null, their cone planes
	// are exact) contributes nothing to it — drop the whole draw. Null
	// view = contribution culling off.
	if (contributionView
		&& !RenderViewIntersectsSphere(*contributionView, worldCenter, worldRadius))
		return true;
	return false;
}

// ---------------------------------------------------------------------------
// CPU-built shadow draw lists (cascade + spot tiles)
// ---------------------------------------------------------------------------

// Slot buffers for the CPU-built shadow lists: draws ring slice (VS BDA
// model source) + indirect commands. Storage, NOT Indirect — same
// cross-queue sharing argument as CreateSlotBuffers (uploads run on the
// transfer queue, draws on graphics); INDIRECT usage is covered by the
// buffer type's flags regardless.
static bool EnsureShadowBuffers(ResourceManager& ctx, RenderGraph& rg, ShadowView& sv) {
	if (sv.buffersReady)
		return true;
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
		sv.drawBuffers[f] = ctx.CreateBuffer(
			kDrawBufferBytes, BufferType_Storage, "gfx shadow draws");
		sv.indirectBuffers[f] = ctx.CreateBuffer(
			kShadowIndirectBytes, BufferType_Storage, "gfx shadow indirect");
		if (sv.drawBuffers[f].empty() || sv.indirectBuffers[f].empty())
			return false;
		sv.drawLogicals[f] = rg.AddExternalBuffer(sv.drawBuffers[f], "gfx shadow draws");
		sv.indirectLogicals[f] = rg.AddExternalBuffer(sv.indirectBuffers[f], "gfx shadow indirect");
		if (sv.drawLogicals[f].empty() || sv.indirectLogicals[f].empty())
			return false;
	}
	sv.buffersReady = true;
	return true;
}

void BeginShadowFrame(State& s) {
	s.shadowViewCount = 0;
}

uint32_t BeginShadowView(State& s, ResourceManager& ctx, RenderGraph& rg,
	const RenderView& renderView, CullFlags cullFlags,
	uint32_t width, uint32_t height,
	const RenderView* contributionView) {
	const uint32_t slot = s.shadowViewCount;
	if (slot >= kMaxViews)
		return UINT32_MAX;
	if (s.shadowViews.size() <= slot) {
		ShadowView fresh{};
		if (!EnsureShadowBuffers(ctx, rg, fresh))
			return UINT32_MAX;
		s.shadowViews.push_back(std::move(fresh));
	}
	ShadowView& sv = s.shadowViews[slot];
	if (!sv.buffersReady && !EnsureShadowBuffers(ctx, rg, sv))
		return UINT32_MAX;
	sv.renderView = &renderView;
	// Contribution gate for the mesh-bound fast discard in BuildShadowList
	// (claimed on the main thread before any worker build — the same
	// threading contract as renderView).
	sv.contributionView = contributionView;
	sv.cullFlags = cullFlags;
	sv.width = width;
	sv.height = height;
	sv.draws.clear();
	sv.drawConstants.clear();
	sv.commands.clear();
	sv.ranges.clear();
	sv.opaqueCommands = 0;
	sv.stats = Stats{};
	++s.shadowViewCount;
	return slot;
}

void PushShadowDraw(State& s, uint32_t sview, uint32_t mesh,
	const glm::mat4& model, uint32_t material, DrawFlags flags,
	DrawSetMask drawSet) {
	assert(sview < s.shadowViewCount);
	assert(mesh < s.meshes.size());
	ShadowView& sv = s.shadowViews[sview];
	if (sv.draws.size() >= kMaxDraws) {
		static bool warnedShadowDrawCap = false;
		if (!warnedShadowDrawCap) {
			warnedShadowDrawCap = true;
			std::fprintf(stderr,
				"[GfxFilter] PushShadowDraw: draw cap %u hit, dropping draws "
				"(increase kMaxDraws).\n", kMaxDraws);
		}
		return;
	}
	DrawRequest d{};
	d.mesh = mesh;
	d.material = material;
	d.model = model;
	d.flags = flags;
	d.drawSet = drawSet;
	sv.draws.push_back(d);
}

void BuildShadowList(State& s, uint32_t sview) {
	assert(sview < s.shadowViewCount);
	ShadowView& sv = s.shadowViews[sview];
	assert(sv.renderView != nullptr);
	sv.stats = Stats{};
	sv.stats.draws = (uint32_t)sv.draws.size();

	SMVK_PROFILE_RENDER("GfxBuildShadowList");
	SMVK_PROFILE_TAG("draws", sv.draws.size());

	// Material sort (stable): EmitBucketMajor walks the entries in emit
	// order, so per-(bucket, material) ranges stay coalesced — same
	// contract as BuildBatches.
	std::vector<uint32_t>& order = sv.drawOrderScratch;
	order.resize(sv.draws.size());
	std::iota(order.begin(), order.end(), 0u);
	std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		return sv.draws[a].material < sv.draws[b].material;
	});

	sv.drawConstants.clear();
	sv.drawConstants.reserve(sv.draws.size());
	sv.emitScratch.clear();
	bool batchCapped = false;
	for (uint32_t sorted = 0; sorted < (uint32_t)order.size(); ++sorted) {
		const DrawRequest& d = sv.draws[order[sorted]];
		Draw dc{};
		dc.model = d.model;
		dc.mesh = d.mesh;
		dc.flags = d.flags;
		dc.drawSet = d.drawSet;
		dc.lmChunk = d.lmChunk;
		dc.lmChartOffset = d.lmChartOffset;
		dc.lmDelta = d.lmDelta;
		sv.drawConstants.push_back(dc);
		const Mesh& mesh = s.meshes[d.mesh];
		const float basisScale = MaxBasisScale(d.model);
		// Mesh-bound fast discard (Northlight-style draw gate, the CPU
		// twin of the main view's GPU GfxDrawCull): the whole draw leaves
		// before the per-cluster meshlet loop when the mesh union bound
		// misses the shadow view, or (contribution cull) when it cannot
		// touch the main camera's view at all. One sphere pair per draw
		// instead of per cluster; the per-cluster sphere test below stays
		// (meshlet granularity inside surviving draws).
		if (d.mesh < s.meshBounds.size()
			&& CullShadowMeshBound(s.meshBounds[d.mesh], d.model,
				*sv.renderView, sv.contributionView)) {
			continue;
		}
		for (uint32_t ci = 0; ci < mesh.clusterCount; ++ci) {
			const Cluster& c = s.clusters[mesh.clusterStart + ci];
			++sv.stats.clustersProcessed;
			if (sv.emitScratch.size() >= kMaxBatches) {
				static bool warnedShadowBatchCap = false;
				if (!warnedShadowBatchCap) {
					warnedShadowBatchCap = true;
					SMVK_DEBUG_LOG(
						"[GfxFilter] BuildShadowList: batch cap %u hit, dropping "
						"clusters (increase kMaxBatches).\n", kMaxBatches);
				}
				batchCapped = true;
				break;
			}
			// Sphere-only cull: the normal-cone stage needs a valid eye
			// position, which directional cascade views do not have —
			// the rasterizer's backface cull covers facings instead.
			const glm::vec3 worldCenter = glm::vec3(d.model * glm::vec4(c.boundCenter, 1.0f));
			const float worldRadius = c.boundRadius * basisScale;
			if (!RenderViewIntersectsSphere(*sv.renderView, worldCenter, worldRadius)) {
				++sv.stats.clustersCulled;
				continue;
			}
			detail::EmitEntry e{};
			e.cluster = mesh.clusterStart + ci;
			e.draw = sorted; // firstInstance indexes the sorted draw constants
			e.material = d.material;
			e.drawSet = d.drawSet;
			sv.emitScratch.push_back(e);
			sv.stats.trianglesProcessed += c.triCount;
		}
		if (batchCapped)
			break;
	}
	// Bucket-major emit: opaque commands first (one VS-only multi-draw),
	// then masked per-material ranges (alpha-test FS push per range).
	EmitBucketMajor(s, sv.emitScratch, sv.commands, sv.ranges, sv.opaqueCommands);
	sv.stats.commands = (uint32_t)sv.commands.size();
	SMVK_PROFILE_TAG("commands", sv.commands.size());
	SMVK_PROFILE_TAG("clustersProcessed", sv.stats.clustersProcessed);
	SMVK_PROFILE_TAG("clustersCulled", sv.stats.clustersCulled);
}

void UploadShadowList(State& s, ResourceManager& ctx,
	uint32_t frameIndex, uint32_t sview) {
	assert(sview < s.shadowViewCount);
	assert(frameIndex < smvk::limits::MaxFramesInFlight);
	ShadowView& sv = s.shadowViews[sview];
	sv.frameIndex = frameIndex;
	// GPU mirrors (main thread; the EndAndPresent flush orders the copies
	// ahead of every consumer pass — same mechanism as the seed/remainder
	// uploads).
	if (!sv.drawConstants.empty()) {
		SMVK_PROFILE_SCOPE("GfxUploadShadowDraws");
		ctx.UploadDataToBuffer(sv.drawBuffers[frameIndex],
			sv.drawConstants.data(), sv.drawConstants.size() * sizeof(Draw),
			PipelineStage(PipelineStageBit_Vertex), AccessFlags::ShaderRead);
	}
	if (!sv.commands.empty()) {
		SMVK_PROFILE_SCOPE("GfxUploadShadowCommands");
		ctx.UploadDataToBuffer(sv.indirectBuffers[frameIndex],
			sv.commands.data(), sv.commands.size() * sizeof(SeedCommand),
			PipelineStage(PipelineStageBit_DrawIndirect), AccessFlags::IndirectCommandRead);
	}
}

const std::vector<SeedRange>& ShadowRanges(const State& s, uint32_t sview) {
	assert(sview < s.shadowViews.size());
	return s.shadowViews[sview].ranges;
}

uint32_t ShadowOpaqueCommands(const State& s, uint32_t sview) {
	assert(sview < s.shadowViews.size());
	return s.shadowViews[sview].opaqueCommands;
}

BufferHandle ShadowDrawBuffer(const State& s, uint32_t sview) {
	assert(sview < s.shadowViews.size());
	return s.shadowViews[sview].drawBuffers[s.shadowViews[sview].frameIndex];
}

BufferHandle ShadowIndirectBuffer(const State& s, uint32_t sview) {
	assert(sview < s.shadowViews.size());
	return s.shadowViews[sview].indirectBuffers[s.shadowViews[sview].frameIndex];
}

const std::vector<Draw>& ShadowDrawConstants(const State& s, uint32_t sview) {
	assert(sview < s.shadowViews.size());
	return s.shadowViews[sview].drawConstants;
}

LogicalBufferHandle ShadowDrawLogical(const State& s, uint32_t sview) {
	assert(sview < s.shadowViews.size());
	return s.shadowViews[sview].drawLogicals[s.shadowViews[sview].frameIndex];
}

LogicalBufferHandle ShadowIndirectLogical(const State& s, uint32_t sview) {
	assert(sview < s.shadowViews.size());
	return s.shadowViews[sview].indirectLogicals[s.shadowViews[sview].frameIndex];
}

Stats FinalizeView(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view) {
	assert(view < s.viewCount);
	assert(frameIndex < smvk::limits::MaxFramesInFlight);
	View& v = s.views[view];
	v.frameIndex = frameIndex;
	v.stats.draws = (uint32_t)v.draws.size();

	SMVK_PROFILE_RENDER("GfxFinalizeView");
	SMVK_PROFILE_TAG("draws", v.draws.size());

	BuildBatches(s, view);

#if 0 // TMP-DIAG scanlands hunt (disabled): validate batch->cluster references. Remove later.	{
		static bool kOnce = false;
		if (!kOnce && std::getenv("TOURMALINE_DUMPBATCH")) {
			kOnce = true;
			size_t bad = 0;
			for (size_t bi = 0; bi < v.batches.size(); ++bi) {
				const Batch& b = v.batches[bi];
				if (b.cluster >= (uint32_t)s.clusters.size()
					|| b.draw >= (uint32_t)v.drawConstants.size()) {
					std::fprintf(stderr, "[TMP-DIAG] batch OOB bi=%zu cluster=%u/%zu draw=%u/%zu\n",
						bi, b.cluster, s.clusters.size(), b.draw, v.drawConstants.size());
					if (++bad > 20) break;
					continue;
				}
				const Cluster& c = s.clusters[b.cluster];
				auto badf = [](float x) { return !(x == x); };
				if (c.triCount == 0 || c.triCount > 128
					|| c.indexOffset + c.triCount * 3u < c.indexOffset
					|| badf(c.boundCenter.x) || badf(c.boundCenter.y) || badf(c.boundCenter.z)
					|| badf(c.boundRadius)) {
					std::fprintf(stderr, "[TMP-DIAG] cluster BAD bi=%zu cl=%u tri=%u off=%u c=(%.2f,%.2f,%.2f) r=%.2f\n",
						bi, b.cluster, c.triCount, c.indexOffset,
						c.boundCenter.x, c.boundCenter.y, c.boundCenter.z, c.boundRadius);
					if (++bad > 20) break;
				}
			}
			std::fprintf(stderr, "[TMP-DIAG] batch scan done batches=%zu clusters=%zu draws=%u\n",
				v.batches.size(), s.clusters.size(), (uint32_t)v.drawConstants.size());
		}
	}
#endif

	// The ONLY per-frame CPU upload is the draws ring slice (the candidate
	// layout is pure arithmetic; gfxbatchcull — recorded later this frame —
	// writes the batch array, the filter indirect statics and the
	// seed/remainder commands GPU-side; the filtered-offset counter is
	// zeroed by gfxbatchcull group 0).
	if (!v.drawConstants.empty()) {
		SMVK_PROFILE_SCOPE("GfxUploadDraws");
		ctx.UploadDataToBuffer(v.drawBuffers[frameIndex],
			v.drawConstants.data(), v.drawConstants.size() * sizeof(Draw),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	}
	return v.stats;
}

void BuildBatches(State& s, uint32_t view) {
	assert(view < s.views.size());
	View& v = s.views[view];
	assert(v.renderView != nullptr);

	GfxProbeScope probeScope(g_probe.batches); // TMP-PROBE
	SMVK_PROFILE_RENDER("GfxBuildBatches");
	SMVK_PROFILE_TAG("draws", v.draws.size());

	// CPU SEED step of the chain — no per-cluster tests here at all. The
	// draw list is material/bucket-sorted, each draw carries its
	// candidate-slot base, and the (bucket, material) range layout is
	// computed; gfxbatchcull (recorded later this frame) runs the sphere
	// + cone tests on the GPU and writes the batch array, the indirect
	// statics and the temporal seed/remainder commands.
	constexpr uint32_t cap = kMaxBatchSlots;

	// Sort draws by (drawSet, material, lightmap lobe class) — stable, so
	// submission order is kept within a class. Bucket-major (opaque first)
	// so the seed pass's opaque phase is one contiguous command span, and
	// commands are emitted in this order so each (material, lobeCount)
	// pair owns one contiguous indirect range (single multi-draw per
	// material per lobe class). The Main filtered leg binds the matching
	// lit program once per range: the generic lit FS, or the
	// SG_LM_LOBE_COUNT-specialized twin when every draw in the range
	// shares one cooked count (the shader then drops the runtime basis
	// dispatch and folds the evaluation loops). A material belongs to one
	// bucket, so (material, lmChunk) spans never straddle a bucket edge —
	// the legacy grouping is unchanged, only the range ORDER turns
	// bucket-major (every consumer binds per range, order-independent).
	std::vector<uint32_t>& order = v.drawOrderScratch;
	order.resize(v.draws.size());
	std::iota(order.begin(), order.end(), 0u);
	std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		if (v.draws[a].drawSet != v.draws[b].drawSet)
			return v.draws[a].drawSet < v.draws[b].drawSet;
		if (v.draws[a].material != v.draws[b].material)
			return v.draws[a].material < v.draws[b].material;
		return v.draws[a].lmChunk < v.draws[b].lmChunk;
	});

	v.drawConstants.reserve(v.draws.size());
	for (uint32_t sorted = 0; sorted < (uint32_t)order.size(); ++sorted) {
		const DrawRequest& d = v.draws[order[sorted]];
		Draw dc{};
		dc.model = d.model;
		dc.mesh = d.mesh;
		dc.flags = d.flags;
		dc.drawSet = d.drawSet;
		dc.lmChunk = d.lmChunk;
		dc.lmChartOffset = d.lmChartOffset;
		dc.lmDelta = d.lmDelta;
		dc.material = d.material;
		// Filled below with the draw's candidate-slot base
		// (0xFFFFFFFF = the cap dropped the draw).
		dc.candidateBase = 0;
		v.drawConstants.push_back(dc);
	}

	uint32_t batchSlot = 0;
	uint32_t cmdMaterial = UINT32_MAX;
	uint32_t cmdLobes = UINT32_MAX;
	uint32_t rangeStart = 0;
	// Seed/remainder range build: (bucket, material) spans over
	// the candidate slots (coalesced across lmChunk — the sort puts the
	// same-(bucket, material) draws adjacent), opaque-then-masked order.
	uint32_t cullRangeStart = 0;
	uint32_t cullRangeSet = DrawSetBit_Opaque;
	uint32_t cullRangeMaterial = UINT32_MAX;
	uint32_t cullOpaque = 0;
	auto pushCullRange = [&](uint32_t endSlot) {
		if (cullRangeMaterial == UINT32_MAX || endSlot <= cullRangeStart)
			return;
		SeedRange r{};
		r.drawSet = cullRangeSet;
		r.material = cullRangeMaterial;
		r.firstCommand = cullRangeStart;
		r.commandCount = endSlot - cullRangeStart;
		v.cullRanges.push_back(r);
	};
	for (uint32_t sorted = 0; sorted < (uint32_t)order.size(); ++sorted) {
		const DrawRequest& d = v.draws[order[sorted]];
		const Mesh& mesh = s.meshes[d.mesh];
		if (d.material != cmdMaterial || d.lmChunk != cmdLobes) {
			if (cmdMaterial != UINT32_MAX) {
				MaterialRange r{};
				r.material = cmdMaterial;
				r.firstCommand = rangeStart;
				r.commandCount = batchSlot - rangeStart;
				r.lobeCount = cmdLobes;
				if (r.commandCount > 0)
					v.materialRanges.push_back(r);
			}
			cmdMaterial = d.material;
			cmdLobes = d.lmChunk;
			rangeStart = batchSlot;
		}
		// Candidate-slot base + (bucket, material) range walk.
		if (batchSlot >= cap) {
			v.drawConstants[sorted].candidateBase = 0xFFFFFFFFu;
		}
		else {
			v.drawConstants[sorted].candidateBase = batchSlot;
			if (d.drawSet != cullRangeSet || d.material != cullRangeMaterial) {
				pushCullRange(batchSlot);
				cullRangeSet = d.drawSet;
				cullRangeMaterial = d.material;
				cullRangeStart = batchSlot;
			}
			if (cullRangeSet == DrawSetBit_Masked && cullOpaque == 0)
				cullOpaque = batchSlot; // first masked slot (opaque span end)
		}
		if (batchSlot >= cap) {
			static bool warnedBatchCap = false;
			if (!warnedBatchCap) {
				warnedBatchCap = true;
				SMVK_DEBUG_LOG(
					"[GfxFilter] BuildBatches: candidate slot cap %u hit, dropping "
					"clusters (increase kMaxBatchSlots).\n",
					cap);
			}
			continue; // mark the remaining draws' bases sentinel below
		}
		for (uint32_t ci = 0; ci < mesh.clusterCount; ++ci) {
			if (batchSlot >= cap)
				break; // partial draw (shader guards slot >= cap)
			const Cluster& c = s.clusters[mesh.clusterStart + ci];
			++v.stats.clustersProcessed;
		Batch b{};
		b.cluster = mesh.clusterStart + ci;
		b.draw = sorted;
		b.outputOffset = 0u; // atomic offsets (GPU compaction)
		b.flags = 0u; // demand-publisher flag is gfxbatchcull's (first KEPT slot)
		v.batches.push_back(b);
			v.stats.trianglesProcessed += c.triCount;
			++batchSlot;
		}
	}
	if (cmdMaterial != UINT32_MAX && batchSlot > rangeStart) {
		MaterialRange r{};
		r.material = cmdMaterial;
		r.firstCommand = rangeStart;
		r.commandCount = batchSlot - rangeStart;
		r.lobeCount = cmdLobes;
		v.materialRanges.push_back(r);
	}
	pushCullRange(batchSlot);
	// No masked draw at all: the whole span is opaque.
	if (cullOpaque == 0)
		cullOpaque = batchSlot;
	v.cullOpaqueCommands = cullOpaque;
	v.batchCount = batchSlot;
	// Fresh stamp once the lists are final: the temporal key cache treats
	// keys built from these batches as valid until the next stamp.
	v.batchesSerial = s.nextBatchesSerial++;
	v.stats.draws = (uint32_t)v.draws.size();
	v.stats.commands = batchSlot;
	// Aggregate cull telemetry on the GfxBuildBatches scope: per-cluster
	// events would flood the profiler (10k+ ns-scale events per frame).
	SMVK_PROFILE_TAG("batches", v.batchCount);
	SMVK_PROFILE_TAG("clustersProcessed", v.stats.clustersProcessed);
	SMVK_PROFILE_TAG("clustersCulled", v.stats.clustersCulled);
	SMVK_PROFILE_TAG("trianglesProcessed", v.stats.trianglesProcessed);
}

RenderPass& AddFilterPass(State& s, RenderGraph& rg, uint32_t view) {
	assert(view < s.viewCount);
	View& v = s.views[view];
	const uint32_t f = v.frameIndex;
	
	SMVK_PROFILE_RENDER("GfxAddFilterPass");
	SMVK_PROFILE_TAG("batchCount", v.batchCount);

	RenderPass& pass = rg.AddComputePass("GfxFilter");
	BufferSyncRequest computeRead{};
	computeRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
	computeRead.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(s.indexHeapLogical, computeRead);
	pass.AddBufferHint(s.clusterLogical, computeRead);
	if (!s.meshBoundsLogical.empty())
		pass.AddBufferHint(s.meshBoundsLogical, computeRead);
	pass.AddBufferHint(v.batchLogicals[f], computeRead);
	pass.AddBufferHint(v.drawLogicals[f], computeRead);
	BufferSyncRequest computeRW{};
	computeRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	computeRW.barrierMasks.stageMask = PipelineStageBit_Compute | PipelineStageBit_Vertex;
	// Filtered scratch is a region of the index monoblock: the same logical
	// covers the compute write here and the index/vertex reads downstream.
	pass.AddBufferHint(s.indexHeapLogical, computeRW);
	pass.AddBufferHint(v.indirectLogicals[f], computeRW);
	// Cull chain verdicts: the kept-bit region (gfxbatchcull, earlier
	// this frame) + the draw verdicts / filtered-offset counter
	// (gfxdrawcull earlier this frame; the counter is read-modify-write
	// through the allocation atomics, so RW — compute stage only, the
	// indirect consumption hints below stay as they are).
	pass.AddBufferHint(v.keptLogicals[f], computeRead);
	BufferSyncRequest stateRW{};
	stateRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	stateRW.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(v.cullStateLogicals[f], stateRW);
	// HZB read (T4 occlusion): built by the HZB passes earlier in the frame.
	// Current flight's slice (same rule as every other ringed buffer).
	if (view < s.temporals.size() && view < s.views.size()
		&& !s.temporals[view].hiz[s.views[view].frameIndex].logical.empty())
		pass.AddBufferHint(s.temporals[view].hiz[s.views[view].frameIndex].logical, computeRead);

	// DEBUG: Log filter pass HZB binding
	static uint64_t s_filterPassCount = 0;
	if ((s_filterPassCount++ % 60) == 0) {
		const TemporalView& t = s.temporals[view];
		const HizResources& hz = t.hiz[s.views[view].frameIndex];
		SMVK_DEBUG_LOG(
			"[GfxFilter] AddFilterPass view=%u batchCount=%u hasHiz=%d hiz=%ux%u/%u mips\n",
			view, v.batchCount, !hz.logical.empty(), hz.width, hz.height, hz.mipCount);
	}

	return pass;
}

void AddConsumeHints(RenderPass& pass,
	LogicalBufferHandle filteredLogical, LogicalBufferHandle indirectLogical) {
	BufferSyncRequest indexRead{};
	indexRead.barrierMasks.accessMask = AccessFlags::IndexRead;
	indexRead.barrierMasks.stageMask = PipelineStageBit_Vertex;
	pass.AddBufferHint(filteredLogical, indexRead);
	BufferSyncRequest indirectRead{};
	indirectRead.barrierMasks.accessMask = AccessFlags::IndirectCommandRead;
	indirectRead.barrierMasks.stageMask = PipelineStageBit_DrawIndirect;
	pass.AddBufferHint(indirectLogical, indirectRead);
}

void PushAndDispatch(State& s, CommandBuffer& cmd, ResourceManager& ctx, uint32_t view) {
	assert(view < s.views.size());
	View& v = s.views[view];
	if (v.batchCount == 0)
		return;

	SMVK_PROFILE_SCOPE("GfxPushAndDispatch");
	SMVK_PROFILE_TAG("batchCount", v.batchCount);

	const uint32_t f = v.frameIndex;
	cmd.BindComputePipeline(s.filterProgram);
	FilterPush push{};
	push.positions = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticVertexPosBuffer);
	// Index streams address regions of the static index monoblock: cluster
	// reads at clusterIndexBase, filtered writes at this flight's slice.
	const uint64_t indexHeapAddr = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticIndexBuffer);
	push.indices = indexHeapAddr + (uint64_t)s.clusterIndexBase * sizeof(uint32_t);
	push.clusters = ctx.GetBufferDeviceAddress(s.clusterBuffer);
	push.batches = ctx.GetBufferDeviceAddress(v.batchBuffers[f]);
	push.draws = ctx.GetBufferDeviceAddress(v.drawBuffers[f]);
	push.filtered = indexHeapAddr + (uint64_t)v.filteredBase[f] * sizeof(uint32_t);
	push.indirect = ctx.GetBufferDeviceAddress(v.indirectBuffers[f]);
	push.indexBase = v.filteredBase[f];
	// A null BDA dereferenced by the filter CS is an MMU fault and takes the
	// whole device down (Aftermath: compute read at 0xa000). Never dispatch
	// in that state: the indirect buffers were zero-filled by FinalizeView
	// (and the chain's statics come from gfxbatchcull), so skipping draws
	// nothing instead of faulting. Loud + throttled.
	if (push.positions == 0 || push.indices == 0 || push.clusters == 0
		|| push.batches == 0 || push.draws == 0 || push.filtered == 0
		|| push.indirect == 0) {
		static uint64_t s_nullBdaWarns = 0;
		if ((s_nullBdaWarns++ % 300) == 0) {
			std::fprintf(stderr,
				"[GfxFilter] refusing dispatch: null BDA (pos=%llx idx=%llx cl=%llx "
				"batch=%llx draw=%llx filt=%llx ind=%llx). Check buffer usage flags.\n",
				(unsigned long long)push.positions, (unsigned long long)push.indices,
				(unsigned long long)push.clusters,
				(unsigned long long)push.batches, (unsigned long long)push.draws,
				(unsigned long long)push.filtered, (unsigned long long)push.indirect);
		}
		return;
	}
	push.viewProj = v.viewProj;
	push.eye = v.eye;
	push.cullFlags = v.cullFlags;
	push.width = v.width;
	push.height = v.height;
	push.batchCount = v.batchCount;
	// TEMP-DIAG (Release-visible): dispatch sanity for the white-speck hunt.
	// (Moved AFTER the assignments above so the flags print the real value.)
	{
		static uint64_t s_dispatches = 0;
		if ((s_dispatches++ % 300) == 0)
			SMVK_TRACE_LOG("[GfxFilter] dispatch ok: view=%u batches=%u "
				"flags=0x%x slice=%u\n", view, v.batchCount, push.cullFlags, f);
	}
	// T4 occlusion/contribution inputs (0/empty disables the tests in the
	// shader; the null-BDA guard above already covers the buffer set).
	// Current flight's HZB slice (same rule as every other ringed buffer).
	if (view < s.temporals.size() && view < s.views.size()) {
		const TemporalView& t = s.temporals[view];
		const HizResources& hz = t.hiz[s.views[view].frameIndex];
		push.hzb = ctx.GetBufferDeviceAddress(hz.buffer);
		push.hizWidth = hz.width;
		push.hizHeight = hz.height;
		push.hizMips = hz.mipCount;
		push.minContribution = t.minimumContribution;
		// Belt and braces: the shader gates on hizMips, so a null address
		// can never slip through to a dereference (device-lost class).
		if (push.hzb == 0) {
			push.hizWidth = push.hizHeight = push.hizMips = 0;
			push.minContribution = 0.0f;
		}
	}
	// Deformed per-cluster bounds (GfxSkinBounds output): skinned draws
	// cull against THIS table instead of the static bind-pose clusters[]
	// (the same animated-silhouette fix the batch cull already uses).
	if (s.skinBoundsReady && !s.skinBoundsBuffers[f].empty()) {
		const uint64_t skinBoundsBda =
			ctx.GetBufferDeviceAddress(s.skinBoundsBuffers[f]);
		if (skinBoundsBda != 0) {
			push.skinBounds = skinBoundsBda;
			push.skinBoundsValid = 1;
		}
	}

	// Draw-level texture-demand inputs (designated-publisher groups;
	// PublishDrawDemand gates on textureMetaCount). Belt and braces: any
	// null BDA zero-gates the whole set — a null dereference is an MMU
	// fault and takes the whole device down (same rule as the HZB guard
	// above). The demand ring slice is THIS dispatch's flight slice (same
	// f as batches/draws above); the materials BDA is the app's per-frame
	// slice resolved in SetDemandInputs.
	push.meshBounds = ctx.GetBufferDeviceAddress(s.meshBoundsBuffer);
	push.materials = s.demandMaterialsBda;
	push.textureMeta = ctx.GetBufferDeviceAddress(s.textureMetaBuffer);
	push.textureDemand = ctx.GetBufferDeviceAddress(s.demandBuffers[f]);
	push.textureMetaCount = s.textureMetaCount;
	push.mipBias = s.demandMipBias;
	if (push.meshBounds == 0 || push.materials == 0 || push.textureMeta == 0
		|| push.textureDemand == 0) {
		push.textureMetaCount = 0;
	}

	// Cull chain inputs: the kept-bit region (parity-indexed — the
	// region gfxbatchcull wrote this frame) and the cullState buffer
	// (gfxdrawcull's draw verdicts + the atomic filtered-offset counter
	// gfxbatchcull zeroed). A null pair is a device-lost-class bug, never
	// a fallback: there is no legacy path anymore, so fail loud and skip
	// the dispatch (the indirects stay pre-zeroed — holes, not faults).
	{
		const uint64_t keptBda = ctx.GetBufferDeviceAddress(v.keptBuffers[f]);
		const uint64_t stateBda = ctx.GetBufferDeviceAddress(v.cullStateBuffers[f]);
		if (keptBda == 0 || stateBda == 0) {
			std::fprintf(stderr,
				"[GfxFilter] PushAndDispatch: null kept/cullState BDA (view %u)\n",
				view);
			assert(false && "filter chain buffers missing");
			return;
		}
		push.kept = keptBda + (uint64_t)KeptParity(s) * kMaxBatchSlots * sizeof(uint32_t);
		push.cullState = stateBda;
		push.keptParity = KeptParity(s);
	}

	// DEBUG: Log filter dispatch HZB parameters
	static uint64_t s_filterDispatchCount = 0;
	if ((s_filterDispatchCount++ % 60) == 0) {
		SMVK_DEBUG_LOG(
			"[GfxFilter] PushAndDispatch view=%u batchCount=%u cullFlags=0x%x hzb=0x%llx hiz=%ux%u/%u minContrib=%.4f\n",
			view, v.batchCount, v.cullFlags,
			(unsigned long long)push.hzb, push.hizWidth, push.hizHeight, push.hizMips, push.minContribution);
	}

	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	cmd.Dispatch(v.batchCount, 1, 1);
}

// ---------------------------------------------------------------------------
// GPU cull chain (GfxBatchCull + GfxDrawCull)
// ---------------------------------------------------------------------------

RenderPass& AddSkinBoundsPass(State& s, RenderGraph& rg, uint32_t view) {
	assert(view < s.viewCount);
	View& v = s.views[view];
	const uint32_t f = v.frameIndex;
	RenderPass& pass = rg.AddComputePass("GfxSkinBounds");
	BufferSyncRequest computeRead{};
	computeRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
	computeRead.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(v.drawLogicals[f], computeRead);
	if (!s.meshesLogical.empty())
		pass.AddBufferHint(s.meshesLogical, computeRead);
	pass.AddBufferHint(s.clusterLogical, computeRead);
	// The cluster-ordered index stream (region of the static monoblock).
	if (!s.indexHeapLogical.empty())
		pass.AddBufferHint(s.indexHeapLogical, computeRead);
	// The output table (GfxSkinBoundsTable: the per-candidate-slot cluster
	// entries + the per-draw drawBound tail — ONE buffer): written here,
	// read by GfxBatchCull + GfxDrawCull below (the hint pairs are the
	// cross-pass ordering edges). The DEFORMED heap is BDA-only (no graph
	// handle): the skinning pass's trailing ALL_COMMANDS barrier orders
	// that read, exactly like the draw passes' deformed fetches.
	BufferSyncRequest computeRW{};
	computeRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	computeRW.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(s.skinBoundsLogicals[f], computeRW);
	return pass;
}

void PushAndDispatchSkinBounds(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view) {
	if (!s.skinBoundsReady)
		return;
	assert(view < s.views.size());
	View& v = s.views[view];
	if (s.skinBoundsProgram.shaderCount == 0 || s.skinBoundsModule.empty())
		return;
	const uint32_t drawCount = (uint32_t)v.drawConstants.size();
	if (drawCount == 0 || drawCount > kMaxDraws)
		return;
	const uint32_t f = v.frameIndex;
	SkinBoundsPush push{};
	push.draws = ctx.GetBufferDeviceAddress(v.drawBuffers[f]);
	push.meshes = ctx.GetBufferDeviceAddress(s.meshesBuffer);
	push.clusters = ctx.GetBufferDeviceAddress(s.clusterBuffer);
	const uint64_t indexHeapAddr = ctx.GetBufferDeviceAddress(
		staticVertexFmtHeap.m_staticIndexBuffer);
	push.indices = indexHeapAddr + (uint64_t)s.clusterIndexBase * sizeof(uint32_t);
	push.deformed = s.skinDeformedBda;
	push.skinBounds = ctx.GetBufferDeviceAddress(s.skinBoundsBuffers[f]);
	push.deformedBase = s.skinDeformedBase;
	push.drawCount = drawCount;
	// The cone rebuild ALWAYS runs (the honest cones feed the cone-axis
	// overlay + the SkinDump cross-check + the TOURMALINE_SKIN_CONE=1
	// consumer lever in gfxbatchcull; the conservative default lives on
	// the CONSUMER side where the cull behavior actually is).
	push.coneEnabled = 1u;
	// MMU-fault rule: never dispatch on a null BDA (no fallback; the batch
	// cull reads the static table for a slot the pass never wrote ... which
	// is exactly the bind-pose hole this pass exists to fix, so skip loudly).
	if (push.draws == 0 || push.meshes == 0 || push.clusters == 0
		|| push.indices == 0 || push.deformed == 0 || push.skinBounds == 0) {
		static uint64_t s_nullBdaWarns = 0;
		if ((s_nullBdaWarns++ % 300) == 0)
			std::fprintf(stderr, "[GfxSkinBounds] refusing dispatch: null BDA\n");
		return;
	}
	cmd.BindComputePipeline(s.skinBoundsProgram);
	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	cmd.Dispatch(drawCount, 1, 1);
}

// ---------------------------------------------------------------------------
// Cone-axis debug lines (the overlay's GPU leg)
// ---------------------------------------------------------------------------

void SetConeDebug(State& s, ResourceManager& ctx, RenderGraph& rg, bool enable) {
	if (enable == s.coneDebug && !enable)
		return; // already off
	if (enable) {
		// Lazily create the module/program + the shared line buffer (the
		// candidate-space prefix, pre-sized to the cap — one allocation for
		// the State's lifetime; the per-frame pass writes exactly the used
		// slots [0, candidateTotal)).
		if (s.coneDebugProgram.shaderCount == 0 || s.coneDebugModule.empty()) {
			try {
				if (s.coneDebugModule.empty())
					s.coneDebugModule = ctx.CreateShaderModule(
						readFile(kGfxConeDebugCsPath), "gfxConeDebugCS");
			}
			catch (...) {
				return; // broken install: the overlay stays dark
			}
			if (s.coneDebugModule.empty())
				return;
			Shader cs{ .module = s.coneDebugModule, .entryPoint = "mainCS",
				.stage = ShaderStage::Compute };
			s.coneDebugProgram = { .shaders = { cs }, .shaderCount = 1 };
		}
		if (s.coneDebugLines.empty()) {
			s.coneDebugLines = ctx.CreateBuffer(
				kConeDebugLinesBytes, BufferType_Storage, "gfx coneDebug lines");
			if (s.coneDebugLines.empty())
				return;
		}
		if (s.coneDebugLinesLogical.empty()) {
			s.coneDebugLinesLogical = rg.AddExternalBuffer(
				s.coneDebugLines, "gfx coneDebug lines");
			if (s.coneDebugLinesLogical.empty()) {
				s.coneDebugLines = {};
				return;
			}
		}
	}
	s.coneDebug = enable;
}

RenderPass& AddConeDebugPass(State& s, RenderGraph& rg, uint32_t view) {
	assert(view < s.viewCount);
	View& v = s.views[view];
	const uint32_t f = v.frameIndex;
	RenderPass& pass = rg.AddComputePass("GfxConeDebug");
	BufferSyncRequest computeRead{};
	computeRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
	computeRead.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(v.drawLogicals[f], computeRead);
	if (!s.meshesLogical.empty())
		pass.AddBufferHint(s.meshesLogical, computeRead);
	pass.AddBufferHint(s.clusterLogical, computeRead);
	// The deformed cones (GfxSkinBounds' output, written earlier this frame):
	// the hint pair is the cross-pass ordering edge.
	if (s.skinBoundsReady && !s.skinBoundsLogicals[f].empty())
		pass.AddBufferHint(s.skinBoundsLogicals[f], computeRead);
	// The line output: compute-write here, vertex-attribute-read by Main's
	// overlay draw (the stage pair carries the cross-stage transition, the
	// same pattern AddFilterPass uses for the filtered index monoblock).
	BufferSyncRequest linesRW{};
	linesRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	linesRW.barrierMasks.stageMask = PipelineStageBit_Compute | PipelineStageBit_Vertex;
	pass.AddBufferHint(s.coneDebugLinesLogical, linesRW);
	return pass;
}

void PushAndDispatchConeDebug(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view) {
	if (!ConeDebugReady(s))
		return;
	assert(view < s.views.size());
	View& v = s.views[view];
	const uint32_t drawCount = (uint32_t)v.drawConstants.size();
	if (drawCount == 0 || drawCount > kMaxDraws)
		return;
	const uint32_t f = v.frameIndex;
	ConeDebugPush push{};
	push.draws = ctx.GetBufferDeviceAddress(v.drawBuffers[f]);
	push.meshes = ctx.GetBufferDeviceAddress(s.meshesBuffer);
	push.clusters = ctx.GetBufferDeviceAddress(s.clusterBuffer);
	push.lines = ctx.GetBufferDeviceAddress(s.coneDebugLines);
	// The deformed cones (GfxSkinBounds output, written just before this
	// pass) — the same gate as the batch cull's.
	if (s.skinBoundsReady && !s.skinBoundsBuffers[f].empty()) {
		const uint64_t bda = ctx.GetBufferDeviceAddress(s.skinBoundsBuffers[f]);
		if (bda != 0) {
			push.skinBounds = bda;
			push.skinBoundsValid = 1;
		}
	}
	// MMU-fault rule: never dispatch on a null BDA (skip loudly; the
	// overlay draws nothing rather than faulting the device).
	if (push.draws == 0 || push.meshes == 0 || push.clusters == 0
		|| push.lines == 0) {
		static uint64_t s_nullBdaWarns = 0;
		if ((s_nullBdaWarns++ % 300) == 0)
			std::fprintf(stderr, "[GfxConeDebug] refusing dispatch: null BDA\n");
		return;
	}
	push.drawCount = drawCount;
	cmd.BindComputePipeline(s.coneDebugProgram);
	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	// One 128-thread workgroup per draw (the GfxBatchCull dispatch shape).
	cmd.Dispatch(drawCount, 1, 1);
}

RenderPass& AddBatchCullPass(State& s, RenderGraph& rg, uint32_t view) {
	assert(view < s.viewCount);
	View& v = s.views[view];
	const uint32_t f = v.frameIndex;
	RenderPass& pass = rg.AddComputePass("GfxBatchCull");
	BufferSyncRequest computeRead{};
	computeRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
	computeRead.barrierMasks.stageMask = PipelineStageBit_Compute;
	// This frame's draws (FinalizeView upload -> compute read).
	pass.AddBufferHint(v.drawLogicals[f], computeRead);
	// Static tables (load-time uploads).
	pass.AddBufferHint(s.clusterLogical, computeRead);
	if (!s.meshesLogical.empty())
		pass.AddBufferHint(s.meshesLogical, computeRead);
	if (!s.meshBoundsLogical.empty())
		pass.AddBufferHint(s.meshBoundsLogical, computeRead);
	// Deformed cluster bounds (GfxSkinBounds output, written just before
	// this pass): skinned draws read this instead of the static table. The
	// hint pair is the cross-pass ordering edge.
	if (s.skinBoundsReady && !s.skinBoundsLogicals[f].empty())
		pass.AddBufferHint(s.skinBoundsLogicals[f], computeRead);
	BufferSyncRequest computeRW{};
	computeRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	computeRW.barrierMasks.stageMask = PipelineStageBit_Compute;
	// The chain's outputs, all consumed later this frame: the batch array
	// + the filter indirect statics (the filter pass — write here, the
	// filter's own RW hint takes over), and the seed/remainder command
	// arrays (the seed/remainder passes' IndirectCommandRead hints — the
	// compute-write -> draw-indirect-read barriers are the rendergraph's
	// standard cross-stage transitions, the same ones the CPU uploads used
	// to make).
	pass.AddBufferHint(v.batchLogicals[f], computeRW);
	pass.AddBufferHint(v.indirectLogicals[f], computeRW);
	if (view < s.temporals.size()) {
		TemporalView& t = s.temporals[view];
		pass.AddBufferHint(t.seedIndirectLogicals[f], computeRW);
		pass.AddBufferHint(t.remainderIndirectLogicals[f], computeRW);
	}
	// The kept-bit ring: current parity region write + prev parity region
	// read (ONE buffer — RW covers both; the prev region's last writer was
	// smvk::limits::MaxFramesInFlight frames ago, flight-fence complete, so the
	// rendergraph's own within-frame tracking never sees that dependency —
	// the submission ordering carries it, like every other ringed buffer).
	pass.AddBufferHint(v.keptLogicals[f], computeRW);
	// The cullState buffer: the filtered-offset counter zero (group 0).
	// gfxdrawcull writes the verdict words in a LATER pass this frame —
	// the rendergraph sequences the WAW pair by hints.
	pass.AddBufferHint(v.cullStateLogicals[f], computeRW);
	return pass;
}

void PushAndDispatchBatchCull(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view) {
	assert(view < s.views.size());
	View& v = s.views[view];
	if (s.batchCullProgram.shaderCount == 0 || s.batchCullModule.empty()) {
		std::fprintf(stderr,
			"[GfxBatchCull] missing shader module (view %u) - chain incomplete\n",
			view);
		return; // chain incomplete: skip (indirects stay pre-zeroed)
	}
	const uint32_t drawCount = (uint32_t)v.drawConstants.size();
	if (drawCount == 0 || drawCount > kMaxDraws)
		return;
	const uint32_t f = v.frameIndex;
	BatchCullPush push{};
	push.draws = ctx.GetBufferDeviceAddress(v.drawBuffers[f]);
	push.meshes = ctx.GetBufferDeviceAddress(s.meshesBuffer);
	push.clusters = ctx.GetBufferDeviceAddress(s.clusterBuffer);
	push.meshBounds = ctx.GetBufferDeviceAddress(s.meshBoundsBuffer);
	push.batches = ctx.GetBufferDeviceAddress(v.batchBuffers[f]);
	push.indirect = ctx.GetBufferDeviceAddress(v.indirectBuffers[f]);
	if (view < s.temporals.size()) {
		TemporalView& t = s.temporals[view];
		push.seedCmds = ctx.GetBufferDeviceAddress(t.seedIndirectBuffers[f]);
		push.remCmds = ctx.GetBufferDeviceAddress(t.remainderIndirectBuffers[f]);
	}
	// Full kept-ring BDA: the shader indexes both parity regions itself.
	push.kept = ctx.GetBufferDeviceAddress(v.keptBuffers[f]);
	push.cullState = ctx.GetBufferDeviceAddress(v.cullStateBuffers[f]);
	// Same MMU-fault rule as PushAndDispatch: never dispatch on a null BDA
	// (no fallback exists — skip loudly; the indirects stay pre-zeroed).
	if (push.draws == 0 || push.meshes == 0 || push.clusters == 0
		|| push.meshBounds == 0 || push.batches == 0 || push.indirect == 0
		|| push.seedCmds == 0 || push.remCmds == 0 || push.kept == 0
		|| push.cullState == 0) {
		static uint64_t s_nullBdaWarns = 0;
		if ((s_nullBdaWarns++ % 300) == 0)
			std::fprintf(stderr, "[GfxBatchCull] refusing dispatch: null BDA\n");
		return;
	}
	// LIVE view (seed/remainder semantics): the gfx view record's matrix,
	// NOT the culling-freeze override (everything that writes the live
	// depth buffer stays on the live main view — the freeze contract).
	push.viewProj = v.viewProj;
	push.eye = v.eye;
	push.drawCount = drawCount;
	push.drawSetMask = DrawSetMask_All;
	if (view < s.temporals.size())
		push.drawSetMask = s.temporals[view].drawSetMask;
	push.clusterIndexBase = s.clusterIndexBase;
	push.keptParity = KeptParity(s);
	push.width = v.width;
	push.height = v.height;
	// Deformed bounds for the skinned draws (GfxSkinBounds output). Only
	// live when this frame ran the skinning pass AND the bounds pass
	// exists - otherwise every draw stays on the static bind-pose table.
	if (s.skinBoundsReady && !s.skinBoundsBuffers[f].empty()) {
		const uint64_t bda = ctx.GetBufferDeviceAddress(s.skinBoundsBuffers[f]);
		if (bda != 0) {
			push.skinBounds = bda;
			push.skinBoundsValid = 1;
		}
	}
	// Skinned-cone A/B lever (TOURMALINE_SKIN_CONE=1): default 0 — skinned
	// clusters cull CONSERVATIVE (sphere + frustum only), Remedy's own
	// shipped call; 1 restores the cone test for skinned clusters (the
	// rebuild itself always runs — the overlay + the SkinDump cross-check
	// read the honest cones either way).
	static const bool s_skinConeOn = [] {
		const char* e = std::getenv("TOURMALINE_SKIN_CONE");
		return e && std::strcmp(e, "1") == 0;
	}();
	push.skinConeEnabled = s_skinConeOn ? 1u : 0u;
	cmd.BindComputePipeline(s.batchCullProgram);
	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	// One 128-thread workgroup per material-sorted draw.
	cmd.Dispatch(drawCount, 1, 1);
}

RenderPass& AddDrawCullPass(State& s, RenderGraph& rg, uint32_t view) {
	assert(view < s.viewCount);
	View& v = s.views[view];
	const uint32_t f = v.frameIndex;
	RenderPass& pass = rg.AddComputePass("GfxDrawCull");
	BufferSyncRequest computeRead{};
	computeRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
	computeRead.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(v.drawLogicals[f], computeRead);
	if (!s.meshBoundsLogical.empty())
		pass.AddBufferHint(s.meshBoundsLogical, computeRead);
	// HZB read: the pyramid the GfxHiz passes built earlier this frame
	// (current flight's slice, same rule as the filter's read).
	if (view < s.temporals.size()
		&& !s.temporals[view].hiz[v.frameIndex].logical.empty())
		pass.AddBufferHint(s.temporals[view].hiz[v.frameIndex].logical, computeRead);
	// Deformed per-draw union bounds (GfxSkinBounds' drawBound tail, written
	// earlier this frame): skinned draws test this instead of the static
	// mesh unions. The hint pair is the cross-pass ordering edge.
	if (s.skinBoundsReady && !s.skinBoundsLogicals[f].empty())
		pass.AddBufferHint(s.skinBoundsLogicals[f], computeRead);
	BufferSyncRequest computeRW{};
	computeRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	computeRW.barrierMasks.stageMask = PipelineStageBit_Compute;
	// Verdict words: gfxbatchcull's counter zero writes the SAME buffer
	// earlier this frame (WAW by hints; the filter reads the words after
	// this pass — also by hints).
	pass.AddBufferHint(v.cullStateLogicals[f], computeRW);
	return pass;
}

void PushAndDispatchDrawCull(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view) {
	assert(view < s.views.size());
	View& v = s.views[view];
	if (s.drawCullProgram.shaderCount == 0 || s.drawCullModule.empty()) {
		std::fprintf(stderr,
			"[GfxDrawCull] missing shader module (view %u) - chain incomplete\n",
			view);
		return; // chain incomplete: skip (verdicts stay zero)
	}
	const uint32_t drawCount = (uint32_t)v.drawConstants.size();
	if (drawCount == 0 || drawCount > kMaxDraws)
		return;
	const uint32_t f = v.frameIndex;
	DrawCullPush push{};
	push.draws = ctx.GetBufferDeviceAddress(v.drawBuffers[f]);
	push.meshBounds = ctx.GetBufferDeviceAddress(s.meshBoundsBuffer);
	push.cullState = ctx.GetBufferDeviceAddress(v.cullStateBuffers[f]);
	if (push.draws == 0 || push.meshBounds == 0 || push.cullState == 0) {
		static uint64_t s_nullBdaWarns = 0;
		if ((s_nullBdaWarns++ % 300) == 0)
			std::fprintf(stderr, "[GfxDrawCull] refusing dispatch: null BDA\n");
		return;
	}
	// FROZEN view: the gfx view record's matrix — under the debug culling
	// freeze this is the HELD view (the filter legs cull against it; the
	// batch cull above deliberately stays LIVE for the seed/remainder
	// semantics, matching the legacy split).
	push.viewProj = v.viewProj;
	push.eye = v.eye;
	push.cullFlags = v.cullFlags;
	push.drawCount = drawCount;
	push.width = v.width;
	push.height = v.height;
	if (view < s.temporals.size()) {
		const TemporalView& t = s.temporals[view];
		const HizResources& hz = t.hiz[v.frameIndex];
		push.hzb = ctx.GetBufferDeviceAddress(hz.buffer);
		push.hizMips = hz.mipCount;
		// Belt and braces: the shader gates on hizMips, so a null address
		// can never slip through to a dereference (device-lost class).
		if (push.hzb == 0)
			push.hizMips = 0;
	}
	// Deformed per-draw union bounds (GfxSkinBounds' drawBound tail, written
	// earlier this frame — the same gate as the batch cull's per-slot table).
	// Without it skinned draws keep the blanket skip (the shader gates on
	// skinBoundsValid), never the static bind-pose union.
	if (s.skinBoundsReady && !s.skinBoundsBuffers[f].empty()) {
		const uint64_t bda = ctx.GetBufferDeviceAddress(s.skinBoundsBuffers[f]);
		if (bda != 0) {
			push.skinBounds = bda;
			push.skinBoundsValid = 1;
		}
	}
	cmd.BindComputePipeline(s.drawCullProgram);
	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	cmd.Dispatch((drawCount + 63u) / 64u, 1, 1);
}

const std::vector<SeedRange>& CullRanges(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].cullRanges;
}

uint32_t CullOpaqueCommands(const State& s, uint32_t view) {
	assert(view < s.views.size());
	return s.views[view].cullOpaqueCommands;
}

bool EnsureMeshParams(State& s, ResourceManager& ctx, RenderGraph& rg) {
	// The VS legs' skinned-fetch inputs (the bind-pose heap BDAs - the
	// eternal static heaps; stashed here where the RHI globals are
	// visible so the record sites' fills never reach for them).
	s.vsPosHeapBda = ctx.GetBufferDeviceAddress(
		staticVertexFmtHeap.m_staticVertexPosBuffer);
	s.vsShadeHeapBda = ctx.GetBufferDeviceAddress(
		staticVertexFmtHeap.m_staticVertexShadingBuffer);
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
		if (s.meshParamsBuffers[f].empty()) {
			s.meshParamsBuffers[f] = ctx.CreateBuffer(
				sizeof(GfxMeshParams), BufferType_Storage, "gfx mesh params");
			if (s.meshParamsBuffers[f].empty())
				return false;
			s.meshParamsLogicals[f] = rg.AddExternalBuffer(
				s.meshParamsBuffers[f], "gfx mesh params");
			if (s.meshParamsLogicals[f].empty()) {
				s.meshParamsBuffers[f] = {};
				return false;
			}
		}
		if (s.meshBatchLists[f].empty()) {
			// Candidate-slot capacity (records every eligible candidate
			// slot — dropped slots gate out through the MS's
			// indexCount == 0 check).
			s.meshBatchLists[f] = ctx.CreateBuffer(
				kMaxBatchSlots * sizeof(uint32_t), BufferType_Storage, "gfx mesh batch list");
			if (s.meshBatchLists[f].empty())
				return false;
			s.meshBatchListLogicals[f] = rg.AddExternalBuffer(
				s.meshBatchLists[f], "gfx mesh batch list");
			if (s.meshBatchListLogicals[f].empty()) {
				s.meshBatchLists[f] = {};
				return false;
			}
		}
	}
	return true;
}

uint64_t UploadMeshParams(State& s, ResourceManager& ctx, uint32_t view,
	uint64_t listBDA, uint64_t lmXformsAddr, const glm::mat4& liveViewProj) {
	assert(view < s.views.size());
	View& v = s.views[view];
	const uint32_t f = v.frameIndex;
	if (s.meshParamsBuffers[f].empty())
		return 0; // EnsureMeshParams failed: the indexed loop covers the frame

	GfxMeshParams p{};
	// CULL matrix: the view record's (frozen while the debug freeze holds
	// the filter). OUTPUT matrix: the live main view (the indexed VS
	// resolves the same through the render-view slab).
	p.viewProj = v.viewProj;
	p.outViewProj = liveViewProj;
	p.draws = ctx.GetBufferDeviceAddress(v.drawBuffers[f]);
	p.batches = ctx.GetBufferDeviceAddress(v.batchBuffers[f]);
	p.clusters = ctx.GetBufferDeviceAddress(s.clusterBuffer);
	p.positions = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticVertexPosBuffer);
	p.shading = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticVertexShadingBuffer);
	// The OPTIONAL lightmap stream: fetched via the per-draw additive
	// delta (GfxDraw.misc2.w). 0 when the stream's buffer has no scene
	// data at all (lightmaps deprecated — the common case).
	p.lightmap = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticVertexLightmapBuffer);
	const uint64_t indexHeapAddr = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticIndexBuffer);
	p.indices = indexHeapAddr + (uint64_t)s.clusterIndexBase * sizeof(uint32_t);
	p.lmXforms = lmXformsAddr;
	p.batchList = listBDA;
	p.indirect = ctx.GetBufferDeviceAddress(v.indirectBuffers[f]);
	p.cullFlags = v.cullFlags;
	p.width = v.width;
	p.height = v.height;
	p.listCount = 0; // unused (slicing rides the tail push)
	// The DEFORMED heap (the skinning driver set s.skinDeformedBda this
	// frame; skinned draws - GFX_DRAW_SKINNED - fetch from it, indexed
	// vi - skinDeformedBase).
	p.deformed = s.skinDeformedBda;
	p.deformedBase = s.skinDeformedBase;
	// Same MMU-fault rule as PushAndDispatch: never publish a null BDA
	// (the indexed loop draws the frame instead; its commands and the
	// seed/remainder depth are intact).
	if (p.draws == 0 || p.batches == 0 || p.clusters == 0 || p.positions == 0
		|| p.shading == 0 || p.indices == 0 || p.batchList == 0
		|| p.indirect == 0) {
		static uint64_t s_meshNullWarns = 0;
		if ((s_meshNullWarns++ % 300) == 0)
			std::fprintf(stderr, "[GfxMesh] refusing dispatch: null BDA\n");
		return 0;
	}
	ctx.UploadDataToBuffer(s.meshParamsBuffers[f], &p, sizeof(p),
		PipelineStage(PipelineStageBit_Mesh), AccessFlags::ShaderRead);
	return ctx.GetBufferDeviceAddress(s.meshParamsBuffers[f]);
}

// One variant dispatch: bind mesh pipeline, push tails, DrawMeshTasks.
// Params must already be uploaded (UploadMeshParams); listBase/listCount
// slice this dispatch out of the shared compacted list.
void RecordMeshVariant(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view, ShaderProgram& meshProg, uint64_t paramsBDA,
	uint32_t listBase, uint32_t listCount,
	uint64_t renderViewsAddr, uint32_t renderViewIndex, uint64_t lmXformsAddr,
	const DepthSettings& depth, const MSAASettings& msaa) {
	assert(view < s.views.size());
	View& v = s.views[view];
	if (listCount == 0 || meshProg.shaderCount != 2 || paramsBDA == 0)
		return;
	const uint32_t f = v.frameIndex;
	uint64_t drawsBDA = ctx.GetBufferDeviceAddress(v.drawBuffers[f]);

	// Opaque state (mirrors bindForMaterial for standard
	// opaque materials: Cull Back + Opaque blend; masked draws
	// never reach this dispatch — see MeshDrawEligible).
	cmd.BindRenderPipeline(
		meshProg,
		VertexLayoutId::Empty, // no VS stage: vertex input ignored, keep empty
		depth,
		StencilSettings::DefaultDisabled(),
		BlendSettings::Opaque(),
		msaa,
		RasterSettings{ .cullMode = CullMode::Back });
	FilteredVsPush tail{};
	tail.draws = drawsBDA;
	tail.renderViews = renderViewsAddr;
	tail.renderViewIndex = renderViewIndex;
	tail.lmXforms = lmXformsAddr;
	// The VS-skin tail fields (bytes 224..240, the tail's trailing 16B): the
	// MeshTailPush below re-pushes the SAME bytes (paramsAddress +
	// listBase/listCount) - the mesh legs never execute a VS, and the mesh
	// shader reads its deformed pair from the GfxMeshParams blob, so the
	// overlay is harmless (mutually exclusive legs).
	FillVsSkinTail(tail, s);
	cmd.PushConstants(kFilteredVsPushOffset, sizeof(tail), &tail, ShaderStage::All);
	MeshTailPush ext{};
	ext.paramsAddress = paramsBDA;
	ext.listBase = listBase;
	ext.listCount = listCount;
	cmd.PushConstants(kMeshTailPushOffset, sizeof(ext), &ext, ShaderStage::All);
	// One workgroup per cluster half of this variant's list slice.
	cmd.DrawMeshTasks(listCount * 2, 1, 1);
}

// ---------------------------------------------------------------------------
// Draw-level texture demand (designated-publisher source)
// ---------------------------------------------------------------------------

void SetDemandInputs(State& s, ResourceManager& ctx,
	uint64_t materialsBda, float mipBias) {
	s.demandMaterialsBda = materialsBda;
	s.demandMipBias = mipBias;
	if (!s.demandBuffers[0].empty())
		return; // ring already created (per-State lifetime, like the pools)
	// Pre-fill the whole ring with the no-report sentinel so the first
	// consume before any GPU write observes "no demand", not garbage
	// (mirror of the retired fragment-feedback ring's creation).
	static std::vector<uint32_t> s_init;
	if (s_init.empty())
		s_init.assign(smvk::limits::MaxTextures, kDemandNoReport);
	for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
		s.demandBuffers[f] = ctx.CreateBuffer(
			static_cast<uint64_t>(smvk::limits::MaxTextures) * sizeof(uint32_t),
			BufferType_Storage, "gfx texture demand");
		if (s.demandBuffers[f].empty())
			return; // partial ring is safe: empty handles gate the dispatch
		ctx.UploadDataToBuffer(s.demandBuffers[f],
			s_init.data(), s_init.size() * sizeof(uint32_t),
			PipelineStage(PipelineStageBit_Compute),
			AccessFlags::ShaderRead | AccessFlags::ShaderWrite, 0, 0);
	}
}

bool UploadTextureMeta(State& s, ResourceManager& ctx, uint64_t epoch,
	const TextureMeta* meta, size_t count) {
	// count == 0 (no streamed textures at all): the publisher is gated
	// off, but the epoch is absorbed so this stops re-attempting.
	if (meta == nullptr || count == 0 || count > smvk::limits::MaxTextures) {
		s.textureMetaCount = 0;
		s.textureMetaEpoch = epoch;
		return true;
	}
	if (s.textureMetaEpoch == epoch && !s.textureMetaBuffer.empty())
		return true; // unchanged (register/unregister only — slots are
	// stable across retargets, so retargets never re-upload)
	s.textureMetaBuffer = ctx.CreateBuffer(
		static_cast<uint64_t>(count) * sizeof(TextureMeta),
		BufferType_Storage, "gfx texture meta");
	if (s.textureMetaBuffer.empty())
		return false; // retry next frame (epoch stays stale)
	ctx.UploadDataToBuffer(s.textureMetaBuffer,
		meta, count * sizeof(TextureMeta),
		PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	s.textureMetaEpoch = epoch;
	s.textureMetaCount = static_cast<uint32_t>(count);
	return true;
}

void ClearDemand(State& s, ResourceManager& ctx, uint32_t frameIndex) {
	if (frameIndex >= smvk::limits::MaxFramesInFlight || s.demandBuffers[frameIndex].empty())
		return;
	// No-report fill of the whole slice (buffers always hold smvk::limits::MaxTextures
	// slots; clearing the full slice keeps the upload size constant — the
	// retired fragment-feedback ring's pattern). The transfer chain orders
	// this fill after the in-flight readback copy (download-before-clear),
	// and the frame's filter writes wait the chain (the same ordering every
	// other per-frame upload relies on), so a stale min can never pin a
	// texture sharp forever after the camera moves away.
	static std::vector<uint32_t> s_clear;
	if (s_clear.empty())
		s_clear.assign(smvk::limits::MaxTextures, kDemandNoReport);
	ctx.UploadDataToBuffer(s.demandBuffers[frameIndex],
		s_clear.data(), s_clear.size() * sizeof(uint32_t),
		PipelineStage(PipelineStageBit_Compute),
		AccessFlags::ShaderRead | AccessFlags::ShaderWrite, 0, 0);
}

// DEBUG: Download and inspect HZB buffer contents (current flight's
// slice, same rule as every other ringed buffer).
void DumpHzb(State& s, ResourceManager& ctx, uint32_t view) {
	if (view >= s.temporals.size() || view >= s.views.size())
		return;
	const TemporalView& t = s.temporals[view];
	const HizResources& hz = t.hiz[s.views[view].frameIndex];
	if (hz.mipCount == 0 || hz.width == 0 || hz.height == 0 || hz.buffer.empty()) {
		SMVK_DEBUG_LOG( "[GfxHiz] DumpHzb: no HZB buffer (view %u)\n", view);
		return;
	}
	std::vector<float> hzb(HzbTotalFloats(hz.width, hz.height), 0.0f);
	if (!ctx.DownloadDataFromBuffer(hz.buffer, hzb.data(), hzb.size() * sizeof(float))) {
		SMVK_DEBUG_LOG( "[GfxHiz] DumpHzb: download failed (view %u)\n", view);
		return;
	}
	SMVK_DEBUG_LOG( "[GfxHiz] DumpHzb view=%u %ux%u mips=%u totalFloats=%zu\n",
		view, hz.width, hz.height, hz.mipCount, hzb.size());
	for (uint32_t m = 0; m < hz.mipCount; ++m) {
		uint32_t mw = 0, mh = 0;
		HzbMipDims(hz.width, hz.height, m, mw, mh);
		uint32_t off = hz.mipOffsets[m];
		float minV = std::numeric_limits<float>::infinity();
		float maxV = -std::numeric_limits<float>::infinity();
		uint32_t nanCount = 0, infCount = 0, zeroCount = 0;
		for (uint32_t y = 0; y < mh; ++y) {
			for (uint32_t x = 0; x < mw; ++x) {
				float v = hzb[off + y * mw + x];
				if (std::isnan(v)) ++nanCount;
				else if (std::isinf(v)) ++infCount;
				else {
					minV = std::min(minV, v);
					maxV = std::max(maxV, v);
					if (v == 0.0f) ++zeroCount;
				}
			}
		}
		SMVK_DEBUG_LOG( "  mip%u: %ux%u off=%u range=[%.6f, %.6f] zeros=%u nan=%u inf=%u\n",
			m, mw, mh, off, minV, maxV, zeroCount, nanCount, infCount);
		// Print first few values for mip 0
		if (m == 0 && mw * mh > 0) {
			SMVK_DEBUG_LOG( "    sample: ");
			for (uint32_t i = 0; i < std::min<uint32_t>(16, mw * mh); ++i) {
				SMVK_DEBUG_LOG( "%.4f ", hzb[off + i]);
			}
			SMVK_DEBUG_LOG( "\n");
		}
	}
}

} // namespace gfx
} // namespace smvk
