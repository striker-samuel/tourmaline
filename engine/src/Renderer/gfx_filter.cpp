#include <Engine/Renderer/gfx_filter.h>
#include <Engine/debug_log.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Profile/profile.h>
#include "../Vulkan/detail/vulkan_impl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>

namespace smvk {
namespace gfx {

static constexpr size_t kBatchBufferBytes = kMaxBatches * sizeof(Batch);
static constexpr size_t kDrawBufferBytes = kMaxDraws * sizeof(Draw);
static constexpr size_t kFilteredBytes = kMaxBatches * kMaxClusterTris * 3 * sizeof(uint32_t);
static constexpr size_t kIndirectBytes = kMaxBatches * 20; // VkDrawIndexedIndirectCommand
static constexpr size_t kVisClusterBytes = kMaxBatches * sizeof(VisibleCluster);
static constexpr size_t kVisDrawBytes = kMaxDraws * sizeof(Draw);
static constexpr size_t kSeedIndirectBytes = kMaxBatches * sizeof(SeedCommand);

static_assert(sizeof(VmaVirtualAllocation) == sizeof(uint64_t),
	"VMA virtual allocation must fit the opaque uint64_t region fields in gfx_filter.h");

// Suballocates indexCount uint32 indices from the static index monoblock
// (one index VkBuffer for mesh + cluster-ordered + filtered regions, so
// draws use absolute firstIndex and the index binding never switches).
// False when the reservation is exhausted.
static bool AllocIndexRegion(uint32_t indexCount, uint32_t& outBaseIndices, uint64_t& outAllocBits) {
	outBaseIndices = 0;
	outAllocBits = 0;
	if (indexCount == 0)
		return true;
	VmaVirtualAllocationCreateInfo info{};
	info.size = (VkDeviceSize)indexCount * sizeof(uint32_t);
	info.alignment = alignof(uint32_t);
	VmaVirtualAllocation alloc = VK_NULL_HANDLE;
	VkDeviceSize byteOffset = 0;
	if (vmaVirtualAllocate(staticVertexFmtHeap.m_staticIndexBlock, &info, &alloc, &byteOffset) != VK_SUCCESS)
		return false;
	outBaseIndices = (uint32_t)(byteOffset / sizeof(uint32_t));
	outAllocBits = (uint64_t)alloc;
	return true;
}

static void FreeIndexRegion(uint64_t& allocBits) {
	if (allocBits == 0)
		return;
	vmaVirtualFree(staticVertexFmtHeap.m_staticIndexBlock, (VmaVirtualAllocation)allocBits);
	allocBits = 0;
}

static void RollbackSlotBuffers(RenderGraph& rg, View& v, uint32_t doneFrames) {
	for (uint32_t f = 0; f < doneFrames; ++f) {
		if (!v.batchLogicals[f].empty()) rg.FreeBuffer(v.batchLogicals[f]);
		if (!v.drawLogicals[f].empty()) rg.FreeBuffer(v.drawLogicals[f]);
		if (!v.indirectLogicals[f].empty()) rg.FreeBuffer(v.indirectLogicals[f]);
		v.batchLogicals[f] = {};
		v.drawLogicals[f] = {};
		v.indirectLogicals[f] = {};
		FreeIndexRegion(v.filteredAllocBits[f]);
		v.filteredBase[f] = 0;
	}
	// Holders release their buffers automatically on scope exit.
}

static bool CreateSlotBuffers(ResourceManager& ctx, RenderGraph& rg, View& v) {
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
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
		if (v.batchBuffers[f].empty() || v.drawBuffers[f].empty()
			|| v.indirectBuffers[f].empty()) {
			RollbackSlotBuffers(rg, v, f);
			return false;
		}
		v.batchLogicals[f] = rg.AddExternalBuffer(v.batchBuffers[f], "gfx batches");
		v.drawLogicals[f] = rg.AddExternalBuffer(v.drawBuffers[f], "gfx draws");
		v.indirectLogicals[f] = rg.AddExternalBuffer(v.indirectBuffers[f], "gfx indirect");
		if (v.batchLogicals[f].empty() || v.drawLogicals[f].empty()
			|| v.indirectLogicals[f].empty()) {
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
	return EnsureHizShaders(s, ctx);
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
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
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
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			if (!v.batchLogicals[f].empty()) rg.FreeBuffer(v.batchLogicals[f]);
			if (!v.drawLogicals[f].empty()) rg.FreeBuffer(v.drawLogicals[f]);
			if (!v.indirectLogicals[f].empty()) rg.FreeBuffer(v.indirectLogicals[f]);
			FreeIndexRegion(v.filteredAllocBits[f]);
			v.filteredBase[f] = 0;
		}
	}
	for (auto& t : s.temporals) {
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			if (!t.visClusterLogicals[f].empty()) rg.FreeBuffer(t.visClusterLogicals[f]);
			if (!t.visDrawLogicals[f].empty()) rg.FreeBuffer(t.visDrawLogicals[f]);
			if (!t.seedIndirectLogicals[f].empty()) rg.FreeBuffer(t.seedIndirectLogicals[f]);
			if (!t.remainderIndirectLogicals[f].empty()) rg.FreeBuffer(t.remainderIndirectLogicals[f]);
		}
		if (!t.hiz.logical.empty()) rg.FreeBuffer(t.hiz.logical);
		t.hiz.logical = {};
		t.hiz.buffer = {};
	}
	s.temporals.clear();
	if (!s.indexHeapLogical.empty()) rg.FreeBuffer(s.indexHeapLogical);
	if (!s.clusterLogical.empty()) rg.FreeBuffer(s.clusterLogical);
	s.indexHeapLogical = {};
	s.clusterLogical = {};
	s.views.clear();
	s.viewCount = 0;
	FreeIndexRegion(s.clusterIndexAllocBits);
	s.clusterIndexBase = 0;
	s.clusterBuffer = {};
	s.filterProgram = ShaderProgram{};
	s.filterModule = {};
	s.hizProgram = ShaderProgram{};
	s.hizModule = {};
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
	const glm::vec4 clipC = viewProj * glm::vec4(worldCenter, 1.0f);
	if (clipC.w <= 0.0f)
		return f; // behind the camera plane: keep (rasterizer resolves)
	// Nearest point toward the eye (exact for an outside camera).
	const glm::vec3 nearest = worldCenter + (toEye / dist) * worldRadius;
	const glm::vec4 clipN = viewProj * glm::vec4(nearest, 1.0f);
	if (clipN.w <= 0.0f)
		return f;
	f.nearestDepth = clipN.z / clipN.w; // reversed-Z value
	// Framebuffer y-down == NDC-down post-Y-flip (cf. forwardplus.cs.hlsl).
	const float w = (float)width, h = (float)height;
	f.centerX = (clipC.x / clipC.w * 0.5f + 0.5f) * w;
	f.centerY = (clipC.y / clipC.w * 0.5f + 0.5f) * h;
	// Extent from the nearest-point depth (clipN.w <= dist): radial dist
	// underestimates off-axis footprints (smaller rect, finer mip) and
	// over-culls. Nearest depth overestimates -> conservative.
	// Focal lengths are magnitudes: viewProj[0][0] is negative for mirrored
	// views (e.g. looking down +z with y-up gives right = -x); a signed fx
	// would flip the radius sign, poison rectW <= 0 and silently disable
	// occlusion for the whole frame (fy already takes abs).
	const float fx = std::abs(viewProj[0][0]);
	const float fy = std::abs(viewProj[1][1]);
	const float depthForExtent = std::max(clipN.w, 1e-6f);
	f.radiusX = worldRadius * fx / depthForExtent * (w * 0.5f);
	f.radiusY = worldRadius * fy / depthForExtent * (h * 0.5f);
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

bool EnsureHiz(State& s, ResourceManager& ctx, RenderGraph& rg,
	uint32_t view, uint32_t w, uint32_t h) {
	TemporalView& t = Temporal(s, view);
	if (w == 0 || h == 0) {
		t.historyValid = false;
		return false;
	}
	const uint32_t wantMips = HzbMipCount(w, h);
	if (t.hiz.width == w && t.hiz.height == h && t.hiz.mipCount == wantMips
		&& !t.hiz.buffer.empty() && !t.hiz.logical.empty())
		return true; // steady state: keep history
	// Size drift: rebuild + disarm (stale seed extent).
	if (!t.hiz.logical.empty()) rg.FreeBuffer(t.hiz.logical);
	t.hiz.logical = {};
	t.hiz.buffer = {};
	t.historyValid = false;
	t.hiz.width = w;
	t.hiz.height = h;
	t.hiz.mipCount = wantMips;
	for (uint32_t m = 0; m < wantMips; ++m)
		t.hiz.mipOffsets[m] = HzbMipOffsetFloats(w, h, m);
	for (uint32_t m = wantMips; m < kHizMaxMips; ++m)
		t.hiz.mipOffsets[m] = 0;
	// Diagnose-only: shader-side array cap (hzb.hlsli HZB_MAX_FLOATS).
	// Exceeding it reads/writes out of bounds -> undefined HZB -> over-cull.
	static constexpr uint32_t kHzbShaderMaxFloats = 12582912;
	if ((uint64_t)HzbTotalFloats(w, h) > kHzbShaderMaxFloats) {
		SMVK_DEBUG_LOG(
			"[GfxHiz] target %ux%u needs %u floats, exceeds shader cap %u: "
			"expect pyramid corruption.\n",
			w, h, HzbTotalFloats(w, h), kHzbShaderMaxFloats);
	}
	t.hiz.buffer = ctx.CreateBuffer(
		(size_t)HzbTotalFloats(w, h) * sizeof(float),
		BufferType_Storage, "gfx hiz");
	if (t.hiz.buffer.empty()) {
		// Leave no half-built pyramid behind: the shader gates on mips.
		t.hiz.width = t.hiz.height = t.hiz.mipCount = 0;
		return false;
	}
	t.hiz.logical = rg.AddExternalBuffer(t.hiz.buffer, "gfx hiz");
	if (t.hiz.logical.empty()) {
		t.hiz.buffer = {};
		return false;
	}
	return true;
}

uint64_t HizAddress(State& s, ResourceManager& ctx, uint32_t view) {
	if (view >= s.temporals.size())
		return 0;
	return ctx.GetBufferDeviceAddress(s.temporals[view].hiz.buffer);
}

RenderPass& AddHizPass(State& s, RenderGraph& rg, uint32_t view,
	uint32_t dstMip, LogicalImageViewHandle depthLogical, const char* passName) {
	assert(view < s.temporals.size());
	TemporalView& t = s.temporals[view];

	SMVK_PROFILE_RENDER("GfxAddHizPass");
	SMVK_PROFILE_TAG("mip", dstMip);
	SMVK_PROFILE_TAG("mipCount", t.hiz.mipCount);

	RenderPass& pass = rg.AddComputePass(passName ? passName : "GfxHiz");
	BufferSyncRequest hizRW{};
	hizRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	hizRW.barrierMasks.stageMask = PipelineStageBit_Compute;
	pass.AddBufferHint(t.hiz.logical, hizRW);
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
			view, dstMip, t.hiz.mipCount, !depthLogical.empty(), passName ? passName : "GfxHiz");
	}

	return pass;
}

void PushAndDispatchHiz(State& s, CommandBuffer& cmd, ResourceManager& ctx,
	uint32_t view, uint32_t dstMip, uint32_t depthBindlessIndex) {
	assert(view < s.temporals.size());
	TemporalView& t = s.temporals[view];
	if (dstMip >= t.hiz.mipCount)
		return;

	SMVK_PROFILE_SCOPE("GfxPushAndDispatchHiz");
	SMVK_PROFILE_TAG("dstMip", dstMip);
	SMVK_PROFILE_TAG("mipCount", t.hiz.mipCount);

	uint32_t dw = 0, dh = 0;
	HzbMipDims(t.hiz.width, t.hiz.height, dstMip, dw, dh);
	if (dw == 0 || dh == 0)
		return;
	const uint64_t addr = ctx.GetBufferDeviceAddress(t.hiz.buffer);
	// Null-BDA guard (same device-lost lesson as PushAndDispatch): skip the
	// level instead of faulting; the HZB stays stale and T4 keeps geometry.
	if (addr == 0) {
		static uint64_t s_hizNullWarns = 0;
		if ((s_hizNullWarns++ % 300) == 0) {
			SMVK_DEBUG_LOG(
				"[GfxHiz] refusing dispatch: null HZB address (view %u mip %u).\n",
				view, dstMip);
		}
		return;
	}
	// Diagnose-only: invalid depth bindless for the mip0 copy reads garbage
	// (undefined values); garbage-near MIN-reduces upward and over-culls.
	// Throttled; no behavior change.
	if (dstMip == 0 && depthBindlessIndex == UINT32_MAX) {
		static uint64_t s_hizDepthWarns = 0;
		if ((s_hizDepthWarns++ % 300) == 0) {
			SMVK_DEBUG_LOG(
				"[GfxHiz] mip0 copy with invalid depth bindless (view %u): "
				"HZB content undefined, expect over-cull.\n",
				view);
		}
	}
	// Diagnose-only: HZB extent vs view extent mismatch misaligns footprint
	// rows/cols with depth rows (wrong-texel over-cull). Throttled.
	if (view < s.views.size()) {
		const View& v = s.views[view];
		if ((t.hiz.width != v.width || t.hiz.height != v.height)
			&& v.width != 0 && v.height != 0) {
			static uint64_t s_hizExtentWarns = 0;
			if ((s_hizExtentWarns++ % 300) == 0) {
				SMVK_DEBUG_LOG(
					"[GfxHiz] extent mismatch: hiz %ux%u vs view %ux%u (view %u).\n",
					t.hiz.width, t.hiz.height, v.width, v.height, view);
			}
		}
	}
	HzbPush push{};
	push.hzb = addr;
	push.dstMip = dstMip;
	push.dstWidth = dw;
	push.dstHeight = dh;
	push.dstOffset = t.hiz.mipOffsets[dstMip];
	push.depthTextureIndex = depthBindlessIndex;
	if (dstMip == 0) {
		push.srcMip = kHizSrcDepth;
		push.srcWidth = t.hiz.width;
		push.srcHeight = t.hiz.height;
		push.srcOffset = 0;
	}
	else {
		uint32_t sw = 0, sh = 0;
		HzbMipDims(t.hiz.width, t.hiz.height, dstMip - 1, sw, sh);
		push.srcMip = dstMip - 1;
		push.srcWidth = sw;
		push.srcHeight = sh;
		push.srcOffset = t.hiz.mipOffsets[dstMip - 1];
	}

	// DEBUG: Log HZB dispatch parameters
	static uint64_t s_hizDispatchCount = 0;
	if ((s_hizDispatchCount++ % 60) == 0) {
		SMVK_DEBUG_LOG(
			"[GfxHiz] Dispatch view=%u mip=%u/%u dst=%ux%u off=%u srcMip=%u src=%ux%u srcOff=%u depthIdx=%u BDA=0x%llx\n",
			view, dstMip, t.hiz.mipCount, dw, dh, push.dstOffset,
			push.srcMip, push.srcWidth, push.srcHeight, push.srcOffset,
			push.depthTextureIndex, (unsigned long long)addr);
	}

	cmd.BindComputePipeline(s.hizProgram);
	cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
	cmd.Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
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

void UploadStaticData(State& s, ResourceManager& ctx, RenderGraph& rg) {
	if (!s.clusterLogical.empty()) rg.FreeBuffer(s.clusterLogical);
	s.clusterLogical = {};
	s.clusterBuffer = {};
	FreeIndexRegion(s.clusterIndexAllocBits);
	s.clusterIndexBase = 0;
	// One rendergraph logical for the static index monoblock (mesh +
	// cluster-ordered + filtered regions share one VkBuffer, so one hint
	// covers every index stream). The heap outlives filter data; keep the
	// logical for the State lifetime.
	if (s.indexHeapLogical.empty())
		s.indexHeapLogical = rg.AddExternalBuffer(
			staticVertexFmtHeap.m_staticIndexBuffer, "static index monoblock");
	if (s.filterIndices.empty() || s.clusters.empty())
		return;
	if (!AllocIndexRegion((uint32_t)s.filterIndices.size(),
		s.clusterIndexBase, s.clusterIndexAllocBits)) {
		std::fprintf(stderr,
			"[GfxFilter] cluster index region exhausted the static index "
			"monoblock (%u indices): increase the reservation.\n",
			(unsigned)s.filterIndices.size());
		return;
	}
	std::vector<ClusterConstants> table(s.clusters.size());
	for (size_t i = 0; i < s.clusters.size(); ++i) {
		table[i].indexOffset = s.clusters[i].indexOffset;
		table[i].triCount = s.clusters[i].triCount;
		table[i].boundCenter = s.clusters[i].boundCenter;
		table[i].boundRadius = s.clusters[i].boundRadius;
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
}

// ---------------------------------------------------------------------------
// Per-frame views
// ---------------------------------------------------------------------------

void BeginFrame(State& s) {
	s.viewCount = 0;
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
			RollbackSlotBuffers(rg, slot, MAX_FRAMES_IN_FLIGHT);
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

void SnapshotVisible(State& s, uint32_t view) {
	assert(view < s.views.size());
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
	// historyValid flips true here; the seed pass (T3) consumes prev* next
	// frame. First snapshot after InvalidateHistory re-arms history.
	t.historyValid = true;
}

void UploadVisible(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view) {
	assert(view < s.views.size());
	assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
	if (view >= s.temporals.size())
		return;
	TemporalView& t = s.temporals[view];
	if (t.visClusterBuffers[frameIndex].empty() || t.visDrawBuffers[frameIndex].empty())
		return; // headless / unallocated: CPU snapshot still stands

	SMVK_PROFILE_SCOPE("GfxUploadVisible");
	SMVK_PROFILE_TAG("prevBatchCount", t.prevBatchCount);
	SMVK_PROFILE_TAG("prevDrawCount", t.prevDrawCount);

	if (t.prevBatchCount > 0) {
		SMVK_PROFILE_SCOPE("GfxUploadVisClusters");
		std::vector<VisibleCluster> pairs;
		pairs.reserve(t.prevBatchCount);
		for (const Batch& b : t.prevBatches)
			pairs.push_back(VisibleCluster{ b.cluster, b.draw });
		ctx.UploadDataToBuffer(t.visClusterBuffers[frameIndex],
			pairs.data(), pairs.size() * sizeof(VisibleCluster),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	}
	if (!t.prevDraws.empty()) {
		SMVK_PROFILE_SCOPE("GfxUploadVisDraws");
		ctx.UploadDataToBuffer(t.visDrawBuffers[frameIndex],
			t.prevDraws.data(), t.prevDraws.size() * sizeof(Draw),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	}
}

// One bucket-major emitter input: resolved (cluster, draw, material, set).
struct EmitEntry {
	uint32_t cluster = 0;
	uint32_t draw = 0; // firstInstance: index into the draws array the VS reads
	uint32_t material = UINT32_MAX;
	uint32_t drawSet = DrawSetBit_Opaque;
};

// Shared bucket-major emitter: opaque commands first (single VS-only
// multi-draw), then masked (per-material ranges for the alpha-test FS
// push). Runs coalesce on (bucket, material).
static void EmitBucketMajor(State& s, const std::vector<EmitEntry>& entries,
	std::vector<SeedCommand>& outCmds, std::vector<SeedRange>& outRanges,
	uint32_t& outOpaque) {
	outCmds.clear();
	outRanges.clear();
	outOpaque = 0;
	for (uint32_t pass = 0; pass < 2; ++pass) {
		const uint32_t wantSet = (pass == 0) ? DrawSetBit_Opaque : DrawSetBit_Masked;
		for (const EmitEntry& e : entries) {
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
			if (slot >= kMaxBatches)
				break;
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

void BuildSeedCommands(State& s, uint32_t view) {
	assert(view < s.views.size());
	TemporalView& t = Temporal(s, view);
	t.seedCommands.clear();
	t.seedRanges.clear();
	t.seedOpaqueCommands = 0;
	if (!t.historyValid || t.prevBatches.empty())
		return;

	SMVK_PROFILE_RENDER("GfxBuildSeedCommands");
	SMVK_PROFILE_TAG("prevBatches", t.prevBatches.size());

	std::vector<uint32_t> batchMaterial;
	ResolveBatchMaterials(t.prevRanges, t.prevBatches.size(), batchMaterial);
	std::vector<EmitEntry> entries;
	entries.reserve(t.prevBatches.size());
	for (size_t i = 0; i < t.prevBatches.size(); ++i) {
		const Batch& b = t.prevBatches[i];
		if (b.draw >= (uint32_t)t.prevDraws.size())
			continue;
		EmitEntry e{};
		e.cluster = b.cluster;
		e.draw = b.draw; // into the visDraws snapshot
		e.material = batchMaterial[i];
		e.drawSet = t.prevDraws[b.draw].drawSet;
		entries.push_back(e);
	}
	EmitBucketMajor(s, entries, t.seedCommands, t.seedRanges, t.seedOpaqueCommands);
}

// (cluster, model) identity for the remainder difference.
struct VisKey {
	uint32_t cluster = UINT32_MAX;
	glm::mat4 model{ 1.0f };
};
static bool VisKeyLess(const VisKey& a, const VisKey& b) {
	if (a.cluster != b.cluster)
		return a.cluster < b.cluster;
	return std::memcmp(&a.model, &b.model, sizeof(glm::mat4)) < 0;
}

void BuildRemainderCommands(State& s, uint32_t view) {
	assert(view < s.views.size());
	TemporalView& t = Temporal(s, view);
	t.remainderCommands.clear();
	t.remainderRanges.clear();
	t.remainderOpaqueCommands = 0;
	const View& v = s.views[view];
	if (!t.historyValid || v.batches.empty())
		return; // no baseline (full prepass covered everything) or nothing new

	SMVK_PROFILE_RENDER("GfxBuildRemainderCommands");
	SMVK_PROFILE_TAG("currBatches", v.batches.size());
	SMVK_PROFILE_TAG("prevBatches", t.prevBatches.size());
	// Sorted baseline keys from the snapshot.
	std::vector<VisKey> prev;
	prev.reserve(t.prevBatches.size());
	for (const Batch& b : t.prevBatches) {
		if (b.draw >= (uint32_t)t.prevDraws.size())
			continue;
		if (b.cluster >= (uint32_t)s.clusters.size())
			continue;
		VisKey k{};
		k.cluster = b.cluster;
		k.model = t.prevDraws[b.draw].model;
		prev.push_back(k);
	}
	std::sort(prev.begin(), prev.end(), VisKeyLess);
	// Material per current batch for range building.
	std::vector<uint32_t> batchMaterial;
	ResolveBatchMaterials(v.materialRanges, v.batches.size(), batchMaterial);
	std::vector<EmitEntry> entries;
	entries.reserve(v.batches.size());
	for (size_t i = 0; i < v.batches.size(); ++i) {
		const Batch& b = v.batches[i];
		if (b.draw >= (uint32_t)v.drawConstants.size())
			continue;
		if (b.cluster >= (uint32_t)s.clusters.size())
			continue;
		VisKey k{};
		k.cluster = b.cluster;
		k.model = v.drawConstants[b.draw].model;
		if (std::binary_search(prev.begin(), prev.end(), k, VisKeyLess))
			continue; // seeded already: same cluster, same transform
		EmitEntry e{};
		e.cluster = b.cluster;
		e.draw = b.draw; // into the CURRENT draw constants (DrawBuffer)
		e.material = batchMaterial[i];
		e.drawSet = v.drawConstants[b.draw].drawSet;
		entries.push_back(e);
	}
	EmitBucketMajor(s, entries,
		t.remainderCommands, t.remainderRanges, t.remainderOpaqueCommands);
}

void UploadSeed(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view) {
	assert(view < s.views.size());
	assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
	if (view >= s.temporals.size())
		return;
	TemporalView& t = s.temporals[view];
	if (t.seedIndirectBuffers[frameIndex].empty())
		return;
	if (t.seedCommands.empty())
		return;

	SMVK_PROFILE_SCOPE("GfxUploadSeed");
	SMVK_PROFILE_TAG("commands", t.seedCommands.size());

	ctx.UploadDataToBuffer(t.seedIndirectBuffers[frameIndex],
		t.seedCommands.data(), t.seedCommands.size() * sizeof(SeedCommand),
		PipelineStage(PipelineStageBit_DrawIndirect), AccessFlags::IndirectCommandRead);
}

void UploadRemainder(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view) {
	assert(view < s.views.size());
	assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
	if (view >= s.temporals.size())
		return;
	TemporalView& t = s.temporals[view];
	if (t.remainderIndirectBuffers[frameIndex].empty())
		return;
	if (t.remainderCommands.empty())
		return;

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
	v.batchCount = 0;
	v.stats = Stats{};
	++s.viewCount;
	return slot;
}

void PushDraw(State& s, uint32_t view, uint32_t mesh,
	const glm::mat4& model, uint32_t material, DrawFlags flags, DrawSetMask drawSet) {
	assert(view < s.viewCount);
	assert(mesh < s.meshes.size());
	View& v = s.views[view];
	if (v.draws.size() >= kMaxDraws)
		return;
	DrawRequest d{};
	d.mesh = mesh;
	d.material = material;
	d.model = model;
	d.flags = flags;
	d.drawSet = drawSet;
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
	const RenderView& renderView, bool twoSided) {
	const glm::vec3 worldCenter = glm::vec3(model * glm::vec4(c.boundCenter, 1.0f));
	const float worldRadius = c.boundRadius * MaxBasisScale(model);
	if (!RenderViewIntersectsSphere(renderView, worldCenter, worldRadius))
		return true;
	if (twoSided)
		return false; // double-sided raster keeps both facings: no cone cull
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

Stats FinalizeView(State& s, ResourceManager& ctx, uint32_t frameIndex, uint32_t view) {
	assert(view < s.viewCount);
	assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
	View& v = s.views[view];
	v.frameIndex = frameIndex;
	v.stats.draws = (uint32_t)v.draws.size();

	SMVK_PROFILE_RENDER("GfxFinalizeView");
	SMVK_PROFILE_TAG("draws", v.draws.size());

	BuildBatches(s, view);

	if (v.batchCount > 0) {
		SMVK_PROFILE_SCOPE("GfxUploadBatches");
		ctx.UploadDataToBuffer(v.batchBuffers[frameIndex],
			v.batches.data(), v.batches.size() * sizeof(Batch),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
	}
	if (!v.drawConstants.empty()) {
		SMVK_PROFILE_SCOPE("GfxUploadDraws");
		ctx.UploadDataToBuffer(v.drawBuffers[frameIndex],
			v.drawConstants.data(), v.drawConstants.size() * sizeof(Draw),
			PipelineStage(PipelineStageBit_Compute), AccessFlags::ShaderRead);
		// Zero-fill indirect (replaces ClearDrawIndirectArgsCS; each group
		// writes exactly its own command slot, so no atomics are needed).
		std::vector<uint8_t> zeros(v.batchCount * 20, 0);
		if (!zeros.empty()) {
			SMVK_PROFILE_SCOPE("GfxUploadIndirect");
			ctx.UploadDataToBuffer(v.indirectBuffers[frameIndex],
				zeros.data(), zeros.size(),
				PipelineStage(PipelineStageBit_Compute),
				AccessFlags::ShaderRead | AccessFlags::ShaderWrite);
		}
	}
	return v.stats;
}

void BuildBatches(State& s, uint32_t view) {
	assert(view < s.views.size());
	View& v = s.views[view];
	assert(v.renderView != nullptr);

	SMVK_PROFILE_RENDER("GfxBuildBatches");
	SMVK_PROFILE_TAG("draws", v.draws.size());

	// Sort draws by material (stable: submission order kept per material).
	// Commands are emitted in this order, so each material owns one
	// contiguous indirect range (single multi-draw per material).
	std::vector<uint32_t> order(v.draws.size());
	std::iota(order.begin(), order.end(), 0u);
	std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		return v.draws[a].material < v.draws[b].material;
	});

	v.drawConstants.reserve(v.draws.size());
	for (uint32_t sorted = 0; sorted < (uint32_t)order.size(); ++sorted) {
		const DrawRequest& d = v.draws[order[sorted]];
		Draw dc{};
		dc.model = d.model;
		dc.mesh = d.mesh;
		dc.flags = d.flags;
		dc.drawSet = d.drawSet;
		v.drawConstants.push_back(dc);
	}

	uint32_t batchSlot = 0;
	uint32_t cmdMaterial = UINT32_MAX;
	uint32_t rangeStart = 0;
	for (uint32_t sorted = 0; sorted < (uint32_t)order.size(); ++sorted) {
		const DrawRequest& d = v.draws[order[sorted]];
		const Mesh& mesh = s.meshes[d.mesh];
		if (d.material != cmdMaterial) {
			if (cmdMaterial != UINT32_MAX) {
				MaterialRange r{};
				r.material = cmdMaterial;
				r.firstCommand = rangeStart;
				r.commandCount = batchSlot - rangeStart;
				if (r.commandCount > 0)
					v.materialRanges.push_back(r);
			}
			cmdMaterial = d.material;
			rangeStart = batchSlot;
		}
		if (batchSlot >= kMaxBatches)
			break;
		const glm::mat4 invModel = glm::inverse(d.model);
		const bool twoSided = (d.flags & DrawFlagBit_TwoSided) != 0;
		for (uint32_t ci = 0; ci < mesh.clusterCount; ++ci) {
			if (batchSlot >= kMaxBatches)
				break;
			const Cluster& c = s.clusters[mesh.clusterStart + ci];
			++v.stats.clustersProcessed;
			if (CullCluster(c, d.model, invModel, v.eye, *v.renderView, twoSided)) {
				++v.stats.clustersCulled;
				continue;
			}
			Batch b{};
			b.cluster = mesh.clusterStart + ci;
			b.draw = sorted;
			b.outputOffset = batchSlot * kMaxClusterTris * 3;
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
		v.materialRanges.push_back(r);
	}
	v.batchCount = batchSlot;
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
	pass.AddBufferHint(v.batchLogicals[f], computeRead);
	pass.AddBufferHint(v.drawLogicals[f], computeRead);
	BufferSyncRequest computeRW{};
	computeRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
	computeRW.barrierMasks.stageMask = PipelineStageBit_Compute | PipelineStageBit_Vertex;
	// Filtered scratch is a region of the index monoblock: the same logical
	// covers the compute write here and the index/vertex reads downstream.
	pass.AddBufferHint(s.indexHeapLogical, computeRW);
	pass.AddBufferHint(v.indirectLogicals[f], computeRW);
	// HZB read (T4 occlusion): built by the HZB passes earlier in the frame.
	if (view < s.temporals.size() && !s.temporals[view].hiz.logical.empty())
		pass.AddBufferHint(s.temporals[view].hiz.logical, computeRead);

	// DEBUG: Log filter pass HZB binding
	static uint64_t s_filterPassCount = 0;
	if ((s_filterPassCount++ % 60) == 0) {
		const TemporalView& t = s.temporals[view];
		SMVK_DEBUG_LOG(
			"[GfxFilter] AddFilterPass view=%u batchCount=%u hasHiz=%d hiz=%ux%u/%u mips\n",
			view, v.batchCount, !t.hiz.logical.empty(), t.hiz.width, t.hiz.height, t.hiz.mipCount);
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
	// in that state: the indirect buffers were zero-filled by FinalizeView,
	// so skipping draws nothing instead of faulting. Loud + throttled.
	if (push.positions == 0 || push.indices == 0 || push.clusters == 0
		|| push.batches == 0 || push.draws == 0 || push.filtered == 0
		|| push.indirect == 0) {
		static uint64_t s_nullBdaWarns = 0;
		if ((s_nullBdaWarns++ % 300) == 0) {
			SMVK_DEBUG_LOG(
				"[GfxFilter] refusing dispatch: null BDA (pos=%llx idx=%llx cl=%llx "
				"batch=%llx draw=%llx filt=%llx ind=%llx). Check buffer usage flags.\n",
				(unsigned long long)push.positions, (unsigned long long)push.indices,
				(unsigned long long)push.clusters, (unsigned long long)push.batches,
				(unsigned long long)push.draws, (unsigned long long)push.filtered,
				(unsigned long long)push.indirect);
		}
		return;
	}
	push.viewProj = v.viewProj;
	push.eye = v.eye;
	push.cullFlags = v.cullFlags;
	push.width = v.width;
	push.height = v.height;
	push.batchCount = v.batchCount;
	// T4 occlusion/contribution inputs (0/empty disables the tests in the
	// shader; the null-BDA guard above already covers the buffer set).
	if (view < s.temporals.size()) {
		const TemporalView& t = s.temporals[view];
		push.hzb = ctx.GetBufferDeviceAddress(t.hiz.buffer);
		push.hizWidth = t.hiz.width;
		push.hizHeight = t.hiz.height;
		push.hizMips = t.hiz.mipCount;
		push.minContribution = t.minimumContribution;
		// Belt and braces: the shader gates on hizMips, so a null address
		// can never slip through to a dereference (device-lost class).
		if (push.hzb == 0) {
			push.hizWidth = push.hizHeight = push.hizMips = 0;
			push.minContribution = 0.0f;
		}
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

// DEBUG: Download and inspect HZB buffer contents
void DumpHzb(State& s, ResourceManager& ctx, uint32_t view) {
	if (view >= s.temporals.size())
		return;
	const TemporalView& t = s.temporals[view];
	if (t.hiz.mipCount == 0 || t.hiz.width == 0 || t.hiz.height == 0 || t.hiz.buffer.empty()) {
		SMVK_DEBUG_LOG( "[GfxHiz] DumpHzb: no HZB buffer (view %u)\n", view);
		return;
	}
	std::vector<float> hzb(HzbTotalFloats(t.hiz.width, t.hiz.height), 0.0f);
	if (!ctx.DownloadDataFromBuffer(t.hiz.buffer, hzb.data(), hzb.size() * sizeof(float))) {
		SMVK_DEBUG_LOG( "[GfxHiz] DumpHzb: download failed (view %u)\n", view);
		return;
	}
	SMVK_DEBUG_LOG( "[GfxHiz] DumpHzb view=%u %ux%u mips=%u totalFloats=%zu\n",
		view, t.hiz.width, t.hiz.height, t.hiz.mipCount, hzb.size());
	for (uint32_t m = 0; m < t.hiz.mipCount; ++m) {
		uint32_t mw = 0, mh = 0;
		HzbMipDims(t.hiz.width, t.hiz.height, m, mw, mh);
		uint32_t off = t.hiz.mipOffsets[m];
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
