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
#include <utility>

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
			if (!t.hiz[f].logical.empty()) rg.FreeBuffer(t.hiz[f].logical);
			t.hiz[f].logical = {};
			t.hiz[f].buffer = {};
		}
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

bool EnsureHiz(State& s, ResourceManager& ctx, RenderGraph& rg,
	uint32_t view, uint32_t w, uint32_t h) {
	TemporalView& t = Temporal(s, view);
	assert(view < s.views.size());
	HizResources& hz = t.hiz[s.views[view].frameIndex];
	if (w == 0 || h == 0) {
		t.historyValid = false;
		return false;
	}
	const uint32_t wantMips = HzbMipCount(w, h);
	if (hz.width == w && hz.height == h && hz.mipCount == wantMips
		&& !hz.buffer.empty() && !hz.logical.empty())
		return true; // steady state: keep history
	// Size drift: rebuild + disarm (stale seed extent).
	if (!hz.logical.empty()) rg.FreeBuffer(hz.logical);
	hz.logical = {};
	hz.buffer = {};
	t.historyValid = false;
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
		BufferType_Storage, "gfx hiz");
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
	hz.logical = rg.AddExternalBuffer(hz.buffer, "gfx hiz");
	if (hz.logical.empty()) {
		hz.buffer = {};
		return false;
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
	// Fresh stamp: cached curr keys from a previous frame of this slot
	// are now invalid (the lists were just cleared).
	v.batchesSerial = s.nextBatchesSerial++;
	++s.viewCount;
	return slot;
}

void PushDraw(State& s, uint32_t view, uint32_t mesh,
	const glm::mat4& model, uint32_t material, DrawFlags flags, DrawSetMask drawSet) {
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
		std::vector<uint8_t>& zeros = v.indirectZeroScratch;
		zeros.assign(v.batchCount * 20, 0);
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
	std::vector<uint32_t>& order = v.drawOrderScratch;
	order.resize(v.draws.size());
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
		if (batchSlot >= kMaxBatches) {
			static bool warnedBatchCap = false;
			if (!warnedBatchCap) {
				warnedBatchCap = true;
				SMVK_DEBUG_LOG(
					"[GfxFilter] BuildBatches: batch cap %u hit, dropping "
					"clusters (increase kMaxBatches).\n", kMaxBatches);
			}
			break;
		}
		const glm::mat4 invModel = glm::inverse(d.model);
		const bool twoSided = (d.flags & DrawFlagBit_TwoSided) != 0;
		for (uint32_t ci = 0; ci < mesh.clusterCount; ++ci) {
			if (batchSlot >= kMaxBatches) {
				static bool warnedInnerCap = false;
				if (!warnedInnerCap) {
					warnedInnerCap = true;
					SMVK_DEBUG_LOG(
						"[GfxFilter] BuildBatches: batch cap %u hit, dropping "
						"clusters (increase kMaxBatches).\n", kMaxBatches);
				}
				break;
			}
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
