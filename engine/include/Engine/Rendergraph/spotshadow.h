// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Spot-shadow atlas builder + caster selection.
//
// Complements the directional CascadedShadowAtlas: a single D32 atlas holds up
// to kMaxSpotShadows square tiles (one perspective render per shadowed spot).
// Only spots that opt in via kForwardPlusFlag_CastShadow are eligible, and per
// frame SelectSpotShadowCasters() picks at most `maxCount` of them using a
// screenspace/distance heuristic (see below), so most spots stay cheap.
//
// Layout convention (matches shaders/shadows.hlsli spot path + shadow.vert):
//  - atlas is columns x rows tiles, tile i at (i % cols, i / cols)
//  - per-tile uvScale/uvBias map tile-local [0,1] UV into atlas UV
//  - uvClampMin/Max inset by half a texel to avoid bleeding
//  - lightVP per tile is shared verbatim between the shadow pass and sampling
//    (no Y-flip on the light perspective – same convention as cascades).
//  - tile descriptors reuse GpuCascadeDescription (splitEnd stores range).

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/light.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/platform.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace smvk {

// Hard cap: tile-descriptor buffer + HLSL SpotShadowBuffer are sized for this.
static constexpr uint32_t kMaxSpotShadows = 8;

struct SpotShadowMapDesc {
	uint32_t slotCount = 4;                 // active tiles this frame (<= kMax)
	uint32_t resolution = 1024;             // per-tile res, square
	Format format = Format::D32_Float;      // depth-only atlas
	float nearPlane = 0.5f;                 // perspective near for all spots
	float bias = 0.0f;                      // receiver-depth bias (perspective) – 0: oriented depth bias in shader
	const char* name = "spotShadowAtlas";
};

inline constexpr const char* kSpotShadowVsPath = "content/shaders/shadow.vert.hlsl.spv";

struct SpotShadowAtlas {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> attachmentView; // depth attachment (depth aspect)
	Holder<ImageViewHandle> sampledView;    // sampled depth view
	Holder<BindlessImageHandle> bindless;
	LogicalImageViewHandle logicalView; // rendergraph alias of sampledView

	// Depth program (shadow.vert, mainVS). The app's masked program handles
	// cutout casters; this one handles solid casters.
	Holder<ShaderModuleHandle> vsModule;
	ShaderProgram program{};

	Extent2D atlasExtent{ 0, 0 };
	Extent2D tileExtent{ 0, 0 };
	Format format = Format::D32_Float;
	uint32_t slotCount = 0;

	[[nodiscard]] bool Valid() const {
		return !image.empty() && slotCount != 0;
	}

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty();
	}

	[[nodiscard]] uint32_t Columns() const {
		assert(slotCount != 0);
		return static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(slotCount))));
	}

	void TileOrigin(uint32_t index, int32_t& outX, int32_t& outY) const {
		assert(index < slotCount);
		const uint32_t cols = Columns();
		outX = static_cast<int32_t>((index % cols) * tileExtent.width);
		outY = static_cast<int32_t>((index / cols) * tileExtent.height);
	}

	int32_t BindlessIndex(ResourceManager& ctx) const {
		return bindless.empty() ? -1 : static_cast<int32_t>(ctx.GetBindlessIndex(bindless));
	}

	bool EnsureShaders(ResourceManager& ctx, const char* vsPath = kSpotShadowVsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string path = vsPath ? vsPath : kSpotShadowVsPath;
			vsModule = ctx.CreateShaderModule(readFile(path), "spotShadowVert");
		} catch (...) {
			return false;
		}
		if (vsModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		program = { .shaders = { vs }, .shaderCount = 1 };
		return true;
	}

	void DestroyShaders() {
		program = ShaderProgram();
		vsModule = {};
	}

	void Destroy(RenderGraph& rg) {
		if (!logicalView.empty())
			rg.FreeImageView(logicalView);
		logicalView = {};
		bindless = {};
		sampledView = {};
		attachmentView = {};
		image = {};
		atlasExtent = { 0, 0 };
		tileExtent = { 0, 0 };
		slotCount = 0;
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, const SpotShadowMapDesc& desc,
		const char* vsPath = kSpotShadowVsPath) {
		if (desc.slotCount == 0 || desc.slotCount > kMaxSpotShadows)
			return false;
		if (desc.resolution == 0)
			return false;

		const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(desc.slotCount))));
		const uint32_t rows = (desc.slotCount + cols - 1) / cols;
		const Extent2D atlas{ desc.resolution * cols, desc.resolution * rows };

		ImageCreateInfo imageCI{};
		imageCI.format = desc.format;
		imageCI.extent = { atlas.width, atlas.height, 1u };
		imageCI.mipLevels = 1u;
		imageCI.arrayLayers = 1u;
		imageCI.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled;
		imageCI.name = desc.name;

		Holder<ImageHandle> newImage = ctx.CreateImage(imageCI, nullptr);
		if (newImage.empty())
			return false;

		ImageViewCreateInfo attachmentCI{};
		attachmentCI.viewType = ImageViewType::View2D;
		attachmentCI.format = Format::Undefined; // inherit
		attachmentCI.subresourceRange = {
			.aspect = ImageAspect::Depth,
			.baseMipLevel = 0,
			.mipLevelCount = 1,
			.baseArrayLayer = 0,
			.arrayLayerCount = 1,
		};
		attachmentCI.name = "spot shadow attachment (depth only)";

		Holder<ImageViewHandle> newAttachment = ctx.CreateImageView(newImage, attachmentCI);
		if (newAttachment.empty())
			return false;

		ImageViewCreateInfo sampledCI{};
		sampledCI.viewType = ImageViewType::View2D;
		sampledCI.format = Format::Undefined; // inherit
		sampledCI.subresourceRange = {
			.aspect = ImageAspect::Depth,
			.baseMipLevel = 0,
			.mipLevelCount = 1,
			.baseArrayLayer = 0,
			.arrayLayerCount = 1,
		};
		sampledCI.name = "spot shadow sampled";

		Holder<ImageViewHandle> newSampled = ctx.CreateImageView(newImage, sampledCI);
		if (newSampled.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newSampled, ImageLayout::DepthReadOnlyOptimal);
		if (newBindless.empty())
			return false;

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newSampled, desc.format, "spotShadowMapView");
		if (newLogical.empty())
			return false;

		image = std::move(newImage);
		attachmentView = std::move(newAttachment);
		sampledView = std::move(newSampled);
		bindless = std::move(newBindless);
		logicalView = newLogical;
		atlasExtent = atlas;
		tileExtent = { desc.resolution, desc.resolution };
		format = desc.format;
		slotCount = desc.slotCount;
		return EnsureShaders(ctx, vsPath);
	}

	// Recreate only when slotCount / resolution / format drift.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, const SpotShadowMapDesc& desc,
		const char* vsPath = kSpotShadowVsPath) {
		if (Valid() &&
			slotCount == desc.slotCount &&
			tileExtent.width == desc.resolution &&
			tileExtent.height == desc.resolution &&
			format == desc.format) {
			return EnsureShaders(ctx, vsPath);
		}
		Destroy(rg);
		return Create(ctx, rg, desc, vsPath);
	}

	// Builds one tile descriptor for a spot light. shadowMapIndex is the
	// bindless index of this atlas (stored by the caller into the light).
	// lightSize/softness drive the EVSM soft-shadow penumbra (packed into
	// the tile pads, see SampleEvsmSpotTileSoft): lightSize = world-space
	// emitting-sphere diameter, softness = artistic scalar (0 = hard).
	// nearPlane rides pad_ so the shader can invert the RH_ZO perspective.
	GpuCascadeDescription BuildSpotTile(
		const glm::vec3& position,
		const glm::vec3& direction,
		float outerAngle,
		float range,
		int32_t shadowMapIndex,
		uint32_t tileIndex,
		float nearPlane = 0.5f,
		float bias = 0.0f,
		float lightSize = 0.35f,
		float softness = 1.0f) const {
		assert(Valid());
		assert(tileIndex < slotCount);

		GpuCascadeDescription tile{};
		const glm::mat4 vp = GetSpotLightVP(position, direction, outerAngle, range, nearPlane, 1.0f);
		std::memcpy(tile.lightVP, glm::value_ptr(vp), sizeof(tile.lightVP));
		tile.splitEnd = range;
		tile.bias = bias;
		tile.normalBias = 0.02f;
		tile.pad_ = 0.0f;
		tile.invShadowSize = glm::vec2(
			1.0f / static_cast<float>(tileExtent.width),
			1.0f / static_cast<float>(tileExtent.height));

		const float invAtlasW = 1.0f / static_cast<float>(atlasExtent.width);
		const float invAtlasH = 1.0f / static_cast<float>(atlasExtent.height);
		const uint32_t cols = Columns();
		const float offX = static_cast<float>((tileIndex % cols) * tileExtent.width);
		const float offY = static_cast<float>((tileIndex / cols) * tileExtent.height);
		tile.uvScale = glm::vec2(
			static_cast<float>(tileExtent.width) * invAtlasW,
			static_cast<float>(tileExtent.height) * invAtlasH);
		tile.uvBias = glm::vec2(offX * invAtlasW, offY * invAtlasH);
		const float texelU = 0.5f * invAtlasW;
		const float texelV = 0.5f * invAtlasH;
		tile.uvClampMin = tile.uvBias + glm::vec2(texelU, texelV);
		tile.uvClampMax = tile.uvBias + tile.uvScale - glm::vec2(texelU, texelV);
		// Soft shadows (SampleEvsmSpotTileSoft): near plane for the
		// eye-depth inversion, emitting-sphere diameter + artistic scalar
		// for the PCSS-style penumbra.
		tile.pad_ = nearPlane;
		tile.pad2_[0] = std::max(lightSize, 0.0f);
		tile.pad2_[1] = std::max(softness, 0.0f);
		(void)shadowMapIndex; // atlas index lives in the light / push constants
		return tile;
	}

	// LiSPSM (Light Space Perspective Shadow Maps) variant for spotlights.
	// Warps the spot projection to match the camera's perspective frustum,
	// providing better texel density in camera/view space.
	GpuCascadeDescription BuildSpotTileLiSPSM(
		const glm::vec3& position,
		const glm::vec3& direction,
		float outerAngle,
		float range,
		int32_t shadowMapIndex,
		uint32_t tileIndex,
		const glm::mat4& cameraView,
		const glm::mat4& cameraProj,
		float cameraNear,
		float cameraFar,
		float nearPlane = 0.5f,
		float bias = 0.0f,
		float lightSize = 0.35f,
		float softness = 1.0f) const;

	// Adds one depth pass rendering slotIndex into its atlas tile. Every
	// spot tile is cleared: each pass owns a disjoint pass area (offset +
	// tileExtent) and Execute() clears exactly that tile (full-area render
	// pass + explicit scissor-clipped vkCmdClearAttachments), so per-tile
	// Clear both initializes the tile and preserves the others.
	RenderPass& AddSpotPass(RenderGraph& rg, uint32_t slotIndex, const char* name = nullptr) const {
		assert(Valid());
		assert(slotIndex < slotCount);
		int32_t offX = 0, offY = 0;
		TileOrigin(slotIndex, offX, offY);
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(offX, offY)
			.SetExtent(tileExtent)
			.SetDepth(
				logicalView,
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::Store,
				ClearDepthStencil(1.0f, 0));
		return pass;
	}
};

// ---------------------------------------------------------------------------
// Caster selection: which requesting spots get one of `maxCount` tiles.
// ---------------------------------------------------------------------------
struct SpotShadowCandidate {
	uint32_t lightIndex = 0; // index into the ForwardPlusLight array
	float score = 0.0f;      // higher = more important (see heuristic)
	float dist = 0.0f;       // camera-to-light distance (m)
	float screenRadius = 0.0f; // approx. light-sphere radius in NDC height
};

// Heuristic (cheap, frame-coherent enough for v1):
//  1. Hard culls: must be a spot, must request shadows (flags bit), must be
//     enabled (intensity > 0, range > 0), light origin within maxDistance of
//     the camera, and its bounding sphere (pos, range) must intersect the
//     NDC box (with margin) – i.e. it can affect a visible pixel.
//  2. Score: intensity-weighted screenspace coverage with distance falloff:
//       coverage ~ clamp(sphereRadiusNdc, 0, 2)
//       cone     ~ (1 - cosOuter) + 0.05  (wider cones matter more)
//       score    = intensity * cone * (0.25 + coverage) / max(dist^2, 1)
//     Brighter, wider, closer, on-screen lights win. Ties break by light
//     index so the selection is deterministic frame to frame.
// Callers assign tiles 0..selected-1 in the returned order.
inline std::vector<SpotShadowCandidate> SelectSpotShadowCasters(
	const std::vector<ForwardPlusLight>& lights,
	const glm::mat4& viewProj,
	const glm::vec3& cameraPos,
	float maxDistance,
	uint32_t maxCount,
	float proj11Abs)
{
	std::vector<SpotShadowCandidate> out;
	if (maxCount == 0 || maxDistance <= 0.0f)
		return out;
	maxCount = std::min(maxCount, kMaxSpotShadows);

	const float projFactor = std::max(proj11Abs, 1e-4f);
	for (uint32_t i = 0; i < lights.size(); ++i) {
		const ForwardPlusLight& l = lights[i];
		if (l.type != static_cast<int32_t>(kForwardPlusSpot))
			continue;
		if ((l.flags & kForwardPlusFlag_CastShadow) == 0)
			continue;
		if (l.intensity <= 0.0f || l.range <= 0.0f)
			continue;

		const glm::vec3 lightPosition(l.position[0], l.position[1], l.position[2]);
		const glm::vec3 toLight = lightPosition - cameraPos;
		const float dist = glm::length(toLight);
		if (dist > maxDistance)
			continue;

		// NDC-box test of the bounding sphere (conservative margin).
		const glm::vec4 clip = viewProj * glm::vec4(lightPosition, 1.0f);
		if (clip.w <= 1e-4f) {
			// Behind (or on) the camera plane: keep only lights whose range
			// reaches the camera – they can still light visible pixels.
			if (dist > l.range)
				continue;
		} else {
			const float radiusNdc = (l.range * projFactor) / clip.w;
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			const float m = radiusNdc;
			if (ndc.x < -1.0f - m || ndc.x > 1.0f + m ||
				ndc.y < -1.0f - m || ndc.y > 1.0f + m ||
				ndc.z < -m || ndc.z > 1.0f + m)
				continue;
		}

		const float radiusNdc = (l.range * projFactor) / std::max(clip.w <= 1e-4f ? dist : clip.w, 1e-4f);
		const float coverage = glm::clamp(radiusNdc, 0.0f, 2.0f);
		const float cosOuter = std::cos(glm::clamp(l.outerAngle, 0.0f, glm::radians(89.0f)));
		const float cone = (1.0f - cosOuter) + 0.05f;
		const float score = l.intensity * cone * (0.25f + coverage) / std::max(dist * dist, 1.0f);

		SpotShadowCandidate c{};
		c.lightIndex = i;
		c.score = score;
		c.dist = dist;
		c.screenRadius = coverage;
		out.push_back(c);
	}

	std::sort(out.begin(), out.end(), [](const SpotShadowCandidate& a, const SpotShadowCandidate& b) {
		if (a.score != b.score)
			return a.score > b.score;
		return a.lightIndex < b.lightIndex;
	});
	if (out.size() > maxCount)
		out.resize(maxCount);
	return out;
}

} // namespace smvk
