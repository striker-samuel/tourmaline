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
#include <cmath>
#include <cstdint>
#include <string>

namespace smvk {

// ---------------------------------------------------------------------------
// GPU light (must match HLSL ForwardPlusLight in common.hlsli / forwardplus)
// 96 bytes, 16-byte aligned.
//
// Shadow fields (spot only, points ignore them):
//  shadowTile      – tile index into the spot-shadow atlas description
//                    buffer, or -1 when this light casts no shadow this frame.
//  shadowMapIndex  – bindless index of the spot-shadow atlas (same for all
//                    shadowed spots in the frame), -1 when unshadowed.
//  shadowBias      – depth bias applied to the receiver depth (perspective).
// `flags` bit kForwardPlusFlag_CastShadow is the *request* flag: the app sets
// it on lights allowed to cast shadows; per-frame selection then assigns
// shadowTile/MapIndex only to the top-N casters (see spotshadow.h).
//
// Light cookie (spot only): cookieIndex is the bindless texture index of a
// gobo projected through the spot cone (cone-UV convention matches the
// spot-shadow projection, see light.hlsli SpotConeUV). -1 = no cookie.
//
// Light function: per-light procedural intensity modulation evaluated per
// pixel per light (Unreal-style light functions, but analytic instead of a
// material). functionType selects the curve, functionSpeed/functionScale
// parameterize it (see kForwardPlusFunction_*).
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
	int32_t flags = 0;                      // 60 bit0 = shadow request
	int32_t shadowTile = -1;                // 64 tile in spot atlas, -1 = none
	int32_t shadowMapIndex = -1;            // 68 bindless atlas index, -1 = none
	float shadowBias = 0.0f;                // 72 receiver-depth bias
	float pad2_ = 0.0f;                     // 76
	int32_t cookieIndex = -1;               // 80 bindless gobo texture, -1 = none
	int32_t functionType = 0;               // 84 kForwardPlusFunction_*
	float functionSpeed = 1.0f;             // 88 animation speed multiplier
	float functionScale = 1.0f;              // 92 modulation depth / pattern scale
};
static_assert(sizeof(ForwardPlusLight) == 96);
static_assert(sizeof(ForwardPlusLight) % 16 == 0);

static constexpr uint32_t kForwardPlusPoint = 0;
static constexpr uint32_t kForwardPlusSpot = 1;
// Per-light controllable flag: light *may* cast a spot shadow. The frame
// selection (upper limit + screenspace/distance heuristic) decides which
// requesting spots actually get a tile (shadowTile >= 0).
static constexpr int32_t kForwardPlusFlag_CastShadow = 1 << 0;

// Light function types (must match light.hlsli EvaluateLightFunction).
// Flicker = stepped candle-like noise over time; Pulse = sine breathing;
// Bands = moving diagonal bands across the spot cone (spatial; spots only,
// points fall back to Pulse). Speed scales the time axis, Scale how deep the
// modulation dips (1 = down to black, 0 = disabled).
static constexpr int32_t kForwardPlusFunction_None = 0;
static constexpr int32_t kForwardPlusFunction_Flicker = 1;
static constexpr int32_t kForwardPlusFunction_Pulse = 2;
static constexpr int32_t kForwardPlusFunction_Bands = 3;

// ---------------------------------------------------------------------------
// GPU decal (must match HLSL ForwardPlusDecal in common.hlsli).
// 128 bytes, 16-byte aligned.
//
// A decal is an oriented box projected along its local +Z: the projection
// axis (xform2's xyz direction) points from the decal center TOWARD the
// receiving surface (like a camera looking down -Z). A surface receives the
// decal when the pixel's world position lands inside the unit cube
// (|local| <= 1, local = xformRows . worldPos) AND the geometric surface
// normal faces back toward the projector (dot(N, axisZ) < 0).
// UV = local.xy * 0.5 + 0.5 (V along the decal's local +Y).
//
// Tile culling tests the bounding sphere (bounds.xyz / bounds.w) against
// the tile frustum + depth range, exactly like point lights.
//
// material (int4): x = bindless albedo texture (RGBA: rgb color, a =
//   coverage mask), y = bindless normal map (tangent-space, xy in [0,1],
//   Z reconstructed on the unit sphere like engine normal maps), z = flags
//   (kDecalFlag_*), w = reserved (-1).
// color: rgb = linear tint (multiplies the albedo texture), a = opacity.
// params: x = normal strength, y = roughness target, z = metallic target,
//   w = depth fade start (opacity fades to 0 as |local.z| approaches 1;
//   keeps offset/curved surfaces from popping at the box faces).
// emissive: rgb = emissive add color, a = strength (0 = off; additive after
//   the material emissive so decals can double as glow projections).
// ---------------------------------------------------------------------------
struct alignas(16) ForwardPlusDecal {
	glm::vec4 bounds = glm::vec4(0.0f);   // 0   xyz world center, w bounding radius
	glm::vec4 xform0 = glm::vec4(0.0f);   // 16  world->unit-cube row 0 (axisX/halfX, -c*axisX/halfX)
	glm::vec4 xform1 = glm::vec4(0.0f);   // 32  row 1
	glm::vec4 xform2 = glm::vec4(0.0f);   // 48  row 2 (projection axis in xyz)
	glm::ivec4 material = glm::ivec4(-1); // 64  albedoTex, normalTex, flags, -1
	glm::vec4 color = glm::vec4(1.0f);    // 80  rgb tint, a opacity
	glm::vec4 params = glm::vec4(1.0f);   // 96  normalStrength, roughTarget, metalTarget, depthFadeStart
	glm::vec4 emissive = glm::vec4(0.0f); // 112 rgb emissive add, a strength
};
static_assert(sizeof(ForwardPlusDecal) == 128);
static_assert(sizeof(ForwardPlusDecal) % 16 == 0);

// Channel flags (ForwardPlusDecal::material.z; must match common.hlsli).
static constexpr int32_t kDecalFlag_Albedo = 1 << 0;
static constexpr int32_t kDecalFlag_Normal = 1 << 1;
static constexpr int32_t kDecalFlag_Material = 1 << 2; // rough/metal targets

// Builds a decal from a placement frame: center + projection axis (points
// from the decal toward the surface) + up hint (texture V direction),
// half extents (x, y, depth) and the material knobs (see ForwardPlusDecal).
// yawRadians spins the decal in its own plane (splat rotation). The up hint
// is orthogonalized against axisZ with a safe fallback for near-parallel
// inputs (floor splats pass world up with axisZ = -Y).
inline ForwardPlusDecal MakeDecal(const glm::vec3& center, glm::vec3 axisZ,
	const glm::vec3& upHint, const glm::vec3& halfExtents,
	int32_t albedoTex, int32_t normalTex, int32_t flags, float opacity,
	const glm::vec3& tint, float emissiveStrength, const glm::vec3& emissiveColor,
	float normalStrength, float roughnessTarget, float metalTarget,
	float depthFadeStart, float yawRadians)
{
	axisZ = glm::normalize(axisZ);
	glm::vec3 up = upHint - axisZ * glm::dot(upHint, axisZ);
	if (glm::length(up) < 1e-4f) {
		up = glm::vec3(0.0f, 1.0f, 0.0f);
		up -= axisZ * glm::dot(up, axisZ);
		if (glm::length(up) < 1e-4f) {
			up = glm::vec3(1.0f, 0.0f, 0.0f);
			up -= axisZ * glm::dot(up, axisZ);
		}
	}
	glm::vec3 axisX = glm::normalize(glm::cross(up, axisZ));
	glm::vec3 axisY = glm::normalize(glm::cross(axisZ, axisX));
	// In-plane yaw (rotate X/Y around the projection axis).
	const float s = std::sin(yawRadians);
	const float c = std::cos(yawRadians);
	const glm::vec3 rx = axisX * c + axisY * s;
	const glm::vec3 ry = axisY * c - axisX * s;
	const float hx = std::max(halfExtents.x, 1e-4f);
	const float hy = std::max(halfExtents.y, 1e-4f);
	const float hz = std::max(halfExtents.z, 1e-4f);

	ForwardPlusDecal d{};
	d.bounds = glm::vec4(center, glm::length(halfExtents));
	const glm::vec3 ex = rx / hx;
	const glm::vec3 ey = ry / hy;
	const glm::vec3 ez = axisZ / hz;
	d.xform0 = glm::vec4(ex, -glm::dot(center, ex));
	d.xform1 = glm::vec4(ey, -glm::dot(center, ey));
	d.xform2 = glm::vec4(ez, -glm::dot(center, ez));
	d.material = glm::ivec4(albedoTex, normalTex, flags, -1);
	d.color = glm::vec4(tint, opacity);
	d.params = glm::vec4(normalStrength, roughnessTarget, metalTarget, depthFadeStart);
	d.emissive = glm::vec4(emissiveColor, emissiveStrength);
	return d;
}

struct ForwardPlusDesc {
	uint32_t tileSize = 16;
	uint32_t maxLights = 1024;
	uint32_t maxLightsPerTile = 128;
	// Decals share the light tiling (same tile size / tile counts): the
	// cull pass fills parallel per-tile decal index lists in the same
	// dispatch. Must stay in sync with the shader MAX_FP_DECALS /
	// MAX_FP_DECALS_PER_TILE defines.
	uint32_t maxDecals = 256;
	uint32_t maxDecalsPerTile = 16;
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
	// Decal tile lists (same tiling as the light lists; the cull pass
	// fills both in one dispatch). Stride is desc.maxDecalsPerTile.
	Holder<BufferHandle> decalTileCountsBuffer;
	Holder<BufferHandle> decalTileIndicesBuffer;
	LogicalBufferHandle decalTileCountsLogical{};
	LogicalBufferHandle decalTileIndicesLogical{};

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
		Holder<BufferHandle> decalCounts = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles, BufferType_Storage, "forwardplus decalTileCounts");
		Holder<BufferHandle> decalIndices = ctx.CreateBuffer(
			sizeof(uint32_t) * tiles * d.maxDecalsPerTile, BufferType_Storage,
			"forwardplus decalTileIndices");
		if (decalCounts.empty() || decalIndices.empty())
			return false;

		LogicalBufferHandle countsLogical = rg.AddExternalBuffer(counts, "forwardplus tileCounts");
		if (countsLogical.empty())
			return false;
		LogicalBufferHandle indicesLogical = rg.AddExternalBuffer(indices, "forwardplus tileIndices");
		if (indicesLogical.empty()) {
			rg.FreeBuffer(countsLogical);
			return false;
		}
		LogicalBufferHandle decalCountsLogical = rg.AddExternalBuffer(decalCounts, "forwardplus decalTileCounts");
		LogicalBufferHandle decalIndicesLogical = rg.AddExternalBuffer(decalIndices, "forwardplus decalTileIndices");
		if (decalCountsLogical.empty() || decalIndicesLogical.empty()) {
			rg.FreeBuffer(countsLogical);
			rg.FreeBuffer(indicesLogical);
			if (!decalCountsLogical.empty())
				rg.FreeBuffer(decalCountsLogical);
			if (!decalIndicesLogical.empty())
				rg.FreeBuffer(decalIndicesLogical);
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
		decalTileCountsBuffer = std::move(decalCounts);
		decalTileIndicesBuffer = std::move(decalIndices);
		decalTileCountsLogical = decalCountsLogical;
		decalTileIndicesLogical = decalIndicesLogical;

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
		if (!decalTileCountsLogical.empty())
			rg.FreeBuffer(decalTileCountsLogical);
		if (!decalTileIndicesLogical.empty())
			rg.FreeBuffer(decalTileIndicesLogical);
		tileCountsLogical = {};
		tileIndicesLogical = {};
		decalTileCountsLogical = {};
		decalTileIndicesLogical = {};
		depthBindlessIndex = UINT32_MAX;
		depthBindless = {};
		depthViewHandle = {};
		tileCountsBuffer = {};
		tileIndicesBuffer = {};
		decalTileCountsBuffer = {};
		decalTileIndicesBuffer = {};
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
			desc.maxDecals == d.maxDecals && desc.maxDecalsPerTile == d.maxDecalsPerTile &&
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
	uint64_t DecalTileCountsAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(decalTileCountsBuffer);
	}
	uint64_t DecalTileIndicesAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(decalTileIndicesBuffer);
	}

	// ---- Pass shells (caller sets the recording func) ----
	// Depth prepass: no color, depth (Clear|Load)->Store over the full
	// extent. Load when the GfxRenderer seeded depth first (T3 temporal
	// seed); Clear otherwise (unchanged legacy behavior).
	RenderPass& AddDepthPrepass(RenderGraph& rg,
		LogicalImageViewHandle depthLogical, Extent2D swapExtent,
		const char* name = "ForwardPlusDepth",
		AttachmentLoadOp depthLoad = AttachmentLoadOp::Clear) const {
		assert(Valid());
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(0, 0)
			.SetExtent(swapExtent)
			.SetDepth(depthLogical,
				depthLoad,
				AttachmentStoreOp::Store,
				ClearDepthStencil(0.0f, 0));
		return pass;
	}

	// Cull: compute, reads depth + light buffer, writes tile buffers.
	// Caller binds cullProgram, pushes ForwardPlusCullPush, dispatches
	// (tileCountX, tileCountY, 1). decalsLogical (optional): the per-frame
	// decal upload buffer read by the decal leg of the cull (pass an empty
	// handle when the frame has no decal data buffer).
	RenderPass& AddCullPass(RenderGraph& rg,
		LogicalImageViewHandle depthLogical,
		LogicalBufferHandle lightsLogical,
		const char* name = "ForwardPlusCull",
		LogicalBufferHandle decalsLogical = LogicalBufferHandle{}) const {
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
		// Decal data (uploaded this frame) read in compute.
		if (!decalsLogical.empty()) {
			BufferSyncRequest decalsReq{};
			decalsReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
			decalsReq.barrierMasks.stageMask = PipelineStageBit_Compute;
			pass.AddBufferHint(decalsLogical, decalsReq);
		}
		// Tile buffers write in compute, read in fragment.
		BufferSyncRequest tileWrite{};
		tileWrite.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		tileWrite.barrierMasks.stageMask = PipelineStageBit_Compute | PipelineStageBit_Fragment;
		pass.AddBufferHint(tileCountsLogical, tileWrite);
		pass.AddBufferHint(tileIndicesLogical, tileWrite);
		pass.AddBufferHint(decalTileCountsLogical, tileWrite);
		pass.AddBufferHint(decalTileIndicesLogical, tileWrite);
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

	// Decal-list sibling of AddTileReadHints (Main samples the decal tile
	// counts + indices even when the light lists come from the
	// deferred-shadow pipeline).
	static void AddDecalTileReadHints(RenderPass& mainPass,
		LogicalBufferHandle countsLogical, LogicalBufferHandle indicesLogical) {
		AddTileReadHints(mainPass, countsLogical, indicesLogical);
	}
};

} // namespace smvk
