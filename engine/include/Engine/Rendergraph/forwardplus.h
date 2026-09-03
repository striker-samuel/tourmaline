#pragma once
// Forward+ tiled light culling builder.
//
// Ports the classic Forward+ stages into the engine builder pattern used by
// BloomChain / CascadedShadowAtlas / HdrTarget:
//   depth prepass (geometry VS only, writes scene depth) ->
//   compute tile cull (depth min/max + frustum vs point/spot volumes) ->
//   forward opaque samples per-tile lists.
//
// Directional lights + CSM stay on their existing path; only point/spot
// lights are tiled. Buffers are addressed via buffer-device-address push
// constants (no new descriptor types); depth is sampled bindless.
//
// Usage (app/src/main.cpp):
//   smvk::ForwardPlus fp;
//   fp.Ensure(ctx, rg, swapExtent, desc, depthViewHandle);
//   // per frame:
//   fp.Ensure(ctx, rg, swapExtent, desc, depthViewHandle); // resize-safe
//   auto& pre = fp.AddDepthPrepass(rg, depthLogical, swapExtent);
//   pre.SetRecordingFunc(... draw opaque casters with depthProgram ...);
//   auto& cull = fp.AddCullPass(rg, depthLogical, lightsLogical,
//       tileCountsLogical, tileIndicesLogical);
//   cull.SetRecordingFunc(... bind cullProgram, push ForwardPlusCullPush,
//       Dispatch(tileCountX, tileCountY, 1) ...);
//   // main opaque: Load depth, sample tile lists via extended PushConstants.

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

// ---------------------------------------------------------------------------
// GPU light (must match HLSL ForwardPlusLight in common.hlsli / forwardplus)
// 64 bytes, 16-byte aligned.
// ---------------------------------------------------------------------------
struct alignas(16) ForwardPlusLight {
	glm::vec4 color = glm::vec4(1.0f);      // 0  linear RGB + pad
	glm::vec3 position = { 0, 0, 0 };       // 16 world pos
	float range = 1.0f;                     // 28 cutoff distance (m)
	glm::vec3 direction = { 0, -1, 0 };     // 32 normalized, spot axis
	float intensity = 0.0f;                 // 44 candela (lm/sr)
	float innerAngle = 0.0f;                // 48 radians (spot)
	float outerAngle = 0.0f;                // 52 radians (spot)
	int32_t type = 0;                       // 56 0=point, 1=spot
	int32_t flags = 0;                      // 60
};
static_assert(sizeof(ForwardPlusLight) == 64);
static_assert(sizeof(ForwardPlusLight) % 16 == 0);

static constexpr uint32_t kForwardPlusPoint = 0;
static constexpr uint32_t kForwardPlusSpot = 1;

struct ForwardPlusDesc {
	uint32_t tileSize = 16;
	uint32_t maxLights = 1024;
	uint32_t maxLightsPerTile = 128;
	const char* name = "forwardplus";
};

// Must match forwardplus.cs.hlsl ForwardPlusCullPush (64 bytes).
struct ForwardPlusCullPush {
	uint64_t frameUniforms = 0;
	uint64_t lights = 0;
	uint64_t tileCounts = 0;
	uint64_t tileIndices = 0;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
	uint32_t lightCount = 0;
	uint32_t tileSize = 16;
	uint32_t maxLightsPerTile = 128;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(ForwardPlusCullPush) == 64);

inline constexpr const char* kForwardPlusCsPath = "content/shaders/forwardplus.cs.hlsl.spv";
inline constexpr const char* kForwardPlusDepthVsPath = "content/shaders/default.vert.hlsl.spv";

struct ForwardPlus {
	ForwardPlusDesc desc{};
	Extent2D extent{ 0, 0 };
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
	uint32_t tileCount = 0;

	// Tile buffers (physical, owned). Single copy reused across frames;
	// GPU serializes via rendergraph barriers (compute write -> frag read).
	Holder<BufferHandle> tileCountsBuffer;
	Holder<BufferHandle> tileIndicesBuffer;
	LogicalBufferHandle tileCountsLogical{};
	LogicalBufferHandle tileIndicesLogical{};

	// Depth bindless (sampled D32 as float in compute). Rebuilt when the
	// depth view handle changes (swapchain recreate).
	Holder<BindlessImageHandle> depthBindless;
	uint32_t depthBindlessIndex = UINT32_MAX;
	ImageViewHandle depthViewHandle{};

	// Programs (owned). Cull CS + depth-prepass VS-only.
	Holder<ShaderModuleHandle> cullModule;
	ShaderProgram cullProgram{};
	Holder<ShaderModuleHandle> depthVsModule;
	ShaderProgram depthProgram{};

	[[nodiscard]] bool Valid() const {
		return tileCount != 0 && !tileCountsBuffer.empty() && !tileIndicesBuffer.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return cullProgram.shaderCount != 0 && depthProgram.shaderCount != 0 &&
			!cullModule.empty() && !depthVsModule.empty();
	}

	static uint32_t TileCountFor(uint32_t dim, uint32_t tileSize) {
		tileSize = tileSize == 0 ? 16 : tileSize;
		return (dim + tileSize - 1) / tileSize;
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* csPath = kForwardPlusCsPath,
		const char* depthVsPath = kForwardPlusDepthVsPath) {
		if (ShadersValid())
			return true;
		try {
			if (cullModule.empty())
				cullModule = ctx.CreateShaderModule(
					readFile(csPath ? csPath : kForwardPlusCsPath), "forwardplusCullCS");
			if (depthVsModule.empty())
				depthVsModule = ctx.CreateShaderModule(
					readFile(depthVsPath ? depthVsPath : kForwardPlusDepthVsPath), "forwardplusDepthVS");
		} catch (...) {
			return false;
		}
		if (cullModule.empty() || depthVsModule.empty())
			return false;
		Shader cs{ .module = cullModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		cullProgram = { .shaders = { cs }, .shaderCount = 1 };
		Shader vs{ .module = depthVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		depthProgram = { .shaders = { vs }, .shaderCount = 1 };
		return true;
	}

	void DestroyShaders() {
		cullProgram = ShaderProgram{};
		depthProgram = ShaderProgram{};
		cullModule = {};
		depthVsModule = {};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent,
		const ForwardPlusDesc& d, ImageViewHandle depthView,
		const char* csPath = kForwardPlusCsPath,
		const char* depthVsPath = kForwardPlusDepthVsPath) {
		if (swapExtent.width == 0 || swapExtent.height == 0)
			return false;
		if (d.tileSize == 0 || d.maxLights == 0 || d.maxLightsPerTile == 0)
			return false;
		Destroy(rg);

		const uint32_t tcx = TileCountFor(swapExtent.width, d.tileSize);
		const uint32_t tcy = TileCountFor(swapExtent.height, d.tileSize);
		const uint32_t tiles = tcx * tcy;
		if (tiles == 0 || tiles > 32768)
			return false;

		Holder<BufferHandle> counts = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles, BufferType_Storage, "forwardplus tileCounts");
		if (counts.empty())
			return false;
		Holder<BufferHandle> indices = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles * d.maxLightsPerTile, BufferType_Storage,
			"forwardplus tileIndices");
		if (indices.empty())
			return false;

		LogicalBufferHandle countsLogical = rg.AddExternalBuffer(counts, "forwardplus tileCounts");
		if (countsLogical.empty())
			return false;
		LogicalBufferHandle indicesLogical = rg.AddExternalBuffer(indices, "forwardplus tileIndices");
		if (indicesLogical.empty()) {
			rg.FreeBuffer(countsLogical);
			return false;
		}

		desc = d;
		extent = swapExtent;
		tileCountX = tcx;
		tileCountY = tcy;
		tileCount = tiles;
		tileCountsBuffer = std::move(counts);
		tileIndicesBuffer = std::move(indices);
		tileCountsLogical = countsLogical;
		tileIndicesLogical = indicesLogical;

		if (!depthView.empty()) {
			depthViewHandle = depthView;
			// Must match the post-barrier layout: the cull hint requests
			// ShaderReadOnlyOptimal and the graph promotes depth formats to
			// DepthStencilReadOnlyOptimal (see CollectPassUsages fixup).
			depthBindless = ctx.CreateBindlessImage(depthView, ImageLayout::DepthStencilReadOnlyOptimal);
			depthBindlessIndex = depthBindless.empty()
				? UINT32_MAX : ctx.GetBindlessIndex(depthBindless);
		}
		return EnsureShaders(ctx, csPath, depthVsPath);
	}

	void Destroy(RenderGraph& rg) {
		if (!tileCountsLogical.empty())
			rg.FreeBuffer(tileCountsLogical);
		if (!tileIndicesLogical.empty())
			rg.FreeBuffer(tileIndicesLogical);
		tileCountsLogical = {};
		tileIndicesLogical = {};
		depthBindlessIndex = UINT32_MAX;
		depthBindless = {};
		depthViewHandle = {};
		tileCountsBuffer = {};
		tileIndicesBuffer = {};
		extent = { 0, 0 };
		tileCountX = tileCountY = tileCount = 0;
	}

	// Resize-safe: recreates tile buffers only when extent/tile/limits drift.
	// Refreshes the depth bindless when the depth view handle changes.
	// Also ensures compute + depth programs. Returns true when valid after.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent,
		const ForwardPlusDesc& d, ImageViewHandle depthView,
		const char* csPath = kForwardPlusCsPath,
		const char* depthVsPath = kForwardPlusDepthVsPath) {
		const uint32_t wantX = TileCountFor(swapExtent.width, d.tileSize);
		const uint32_t wantY = TileCountFor(swapExtent.height, d.tileSize);
		const bool shapeOk = Valid() &&
			extent.width == swapExtent.width && extent.height == swapExtent.height &&
			desc.tileSize == d.tileSize && desc.maxLights == d.maxLights &&
			desc.maxLightsPerTile == d.maxLightsPerTile &&
			tileCountX == wantX && tileCountY == wantY;
		if (shapeOk) {
			desc = d;
			if (depthView != depthViewHandle && !depthView.empty()) {
				depthViewHandle = depthView;
				depthBindless = ctx.CreateBindlessImage(depthView, ImageLayout::DepthStencilReadOnlyOptimal);
				depthBindlessIndex = depthBindless.empty()
					? UINT32_MAX : ctx.GetBindlessIndex(depthBindless);
			}
			return EnsureShaders(ctx, csPath, depthVsPath);
		}
		return Create(ctx, rg, swapExtent, d, depthView, csPath, depthVsPath);
	}

	uint64_t TileCountsAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(tileCountsBuffer);
	}
	uint64_t TileIndicesAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(tileIndicesBuffer);
	}

	// ---- Pass shells (caller sets the recording func) ----
	// Depth prepass: no color, depth Clear->Store over the full extent.
	RenderPass& AddDepthPrepass(RenderGraph& rg,
		LogicalImageViewHandle depthLogical, Extent2D swapExtent,
		const char* name = "ForwardPlusDepth") const {
		assert(Valid());
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(0, 0)
			.SetExtent(swapExtent)
			.SetDepth(depthLogical,
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::Store,
				ClearDepthStencil(0.0f, 0));
		return pass;
	}

	// Cull: compute, reads depth + light buffer, writes tile buffers.
	// Caller binds cullProgram, pushes ForwardPlusCullPush, dispatches
	// (tileCountX, tileCountY, 1).
	RenderPass& AddCullPass(RenderGraph& rg,
		LogicalImageViewHandle depthLogical,
		LogicalBufferHandle lightsLogical,
		const char* name = "ForwardPlusCull") const {
		assert(Valid());
		RenderPass& pass = rg.AddComputePass(name);
		// Depth sampled in compute.
		ImageSyncRequest depthReq{};
		depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
		depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
		depthReq.layout = ImageLayout::ShaderReadOnlyOptimal;
		pass.AddImageHint(depthLogical, depthReq);
		// Lights read in compute.
		BufferSyncRequest lightsReq{};
		lightsReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
		lightsReq.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(lightsLogical, lightsReq);
		// Tile buffers write in compute, read in fragment.
		BufferSyncRequest tileWrite{};
		tileWrite.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		tileWrite.barrierMasks.stageMask = PipelineStageBit_Compute | PipelineStageBit_Fragment;
		pass.AddBufferHint(tileCountsLogical, tileWrite);
		pass.AddBufferHint(tileIndicesLogical, tileWrite);
		return pass;
	}

	// Read-side barrier helper for the opaque forward pass: tile buffers
	// sampled in fragment. Call before/while building the main pass.
	static void AddTileReadHints(RenderPass& mainPass,
		LogicalBufferHandle countsLogical, LogicalBufferHandle indicesLogical) {
		BufferSyncRequest tileRead{};
		tileRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		tileRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
		mainPass.AddBufferHint(countsLogical, tileRead);
		mainPass.AddBufferHint(indicesLogical, tileRead);
	}
};

} // namespace smvk
