#pragma once
// Cascaded shadow-map atlas builder.
// Ports the working directional CSM path that previously lived inline in
// app/src/main.cpp (Option B: single D32 atlas, NxN tiles, one depth pass
// per cascade) so engine users get resource creation + cascade math +
// rendergraph pass setup from one place.
//
// Layout convention (must match shaders/shadows.hlsli + shadow.vert.hlsl):
//  - atlas is columns x rows tiles, tile i at (i % cols, i / cols)
//  - per-cascade uvScale/uvBias map tile-local [0,1] UV into atlas UV
//  - uvClampMin/Max inset by half a texel to avoid bleeding
//  - lightVP per cascade is shared verbatim between shadow pass and sampling
//    (no Y-flip on the light ortho – see note in BuildDirectionalLight)

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/light.h>
#include <Engine/platform.h>

#include <glm/gtc/type_ptr.hpp>

#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace smvk {

// Maximum cascades supported by GpuDirectionalLightDescription.
static constexpr uint32_t kMaxCascades = 4;

struct CascadedShadowMapDesc {
	uint32_t cascadeCount = 4;
	uint32_t cascadeResolution = 2048; // per-cascade tile, square
	Format format = Format::D32_Float; // depth-only atlas, stencil dropped
	float lambda = 0.92f;              // 0 = linear splits, 1 = logarithmic
	float zPadding = 1.0f;             // ortho Z padding around the slice
	const char* name = "shadowMapAtlas";
};

inline uint32_t CascadedShadowColumns(uint32_t count) {
	assert(count != 0);
	return static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(count))));
}

inline uint32_t CascadedShadowRows(uint32_t count) {
	const uint32_t cols = CascadedShadowColumns(count);
	return (count + cols - 1) / cols;
}

inline Extent2D CascadedShadowAtlasExtent(uint32_t cascadeRes, uint32_t count) {
	const uint32_t cols = CascadedShadowColumns(count);
	const uint32_t rows = CascadedShadowRows(count);
	return { cascadeRes * cols, cascadeRes * rows };
}

// Practical split scheme (lambda blend of log & linear), splits[0] = near,
// splits[count] = far.
inline std::vector<float> ComputeCascadeSplits(
	float nearClip,
	float farClip,
	float lambda,
	uint32_t cascadeCount) {
	assert(cascadeCount >= 1 && cascadeCount <= kMaxCascades);
	assert(nearClip > 0.0f && farClip > nearClip);
	std::vector<float> splits(cascadeCount + 1, nearClip);
	splits[0] = nearClip;
	splits[cascadeCount] = farClip;
	for (uint32_t i = 1; i < cascadeCount; ++i) {
		const float p = static_cast<float>(i) / static_cast<float>(cascadeCount);
		const float logSplit = nearClip * std::pow(farClip / nearClip, p);
		const float linSplit = nearClip + (farClip - nearClip) * p;
		splits[i] = lambda * logSplit + (1.0f - lambda) * linSplit;
	}
	return splits;
}

// Default SPV for the depth-only cascade pass (single VS, no FS).
inline constexpr const char* kCascadedShadowVsPath = "content/shaders/shadow.vert.hlsl.spv";

struct CascadedShadowAtlas {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> attachmentView; // depth attachment (depth aspect)
	Holder<ImageViewHandle> sampledView;    // sampled depth view
	Holder<BindlessImageHandle> bindless;
	LogicalImageViewHandle logicalView; // rendergraph alias of sampledView

	// Default depth-only program (shadow.vert, mainVS). Created by
	// EnsureShaders/Create/Ensure so apps don't own shadow modules.
	Holder<ShaderModuleHandle> vsModule;
	ShaderProgram program{};

	Extent2D atlasExtent{ 0, 0 };
	Extent2D tileExtent{ 0, 0 };
	Format format = Format::D32_Float;
	uint32_t cascadeCount = 0;

	[[nodiscard]] bool Valid() const {
		return !image.empty() && cascadeCount != 0;
	}

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty();
	}

	[[nodiscard]] uint32_t Columns() const {
		return CascadedShadowColumns(cascadeCount);
	}

	// Tile origin in texels for cascade i.
	void TileOrigin(uint32_t index, int32_t& outX, int32_t& outY) const {
		assert(index < cascadeCount);
		const uint32_t cols = Columns();
		outX = static_cast<int32_t>((index % cols) * tileExtent.width);
		outY = static_cast<int32_t>((index / cols) * tileExtent.height);
	}

	// Idempotent default program creation (shadow.vert, mainVS).
	// Returns true when program is ready afterwards.
	bool EnsureShaders(ResourceManager& ctx, const char* vsPath = kCascadedShadowVsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string path = vsPath ? vsPath : kCascadedShadowVsPath;
			vsModule = ctx.CreateShaderModule(readFile(path), "shadowVert");
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
		program = ShaderProgram{};
		vsModule = {};
	}

	// Frees the atlas images/views (shaders survive – they are size-independent).
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
		cascadeCount = 0;
	}

	// Creates the physical atlas (image + depth/sampled views + bindless)
	// and registers the sampled view with the rendergraph.
	// Also ensures the default shader program. Returns false when the
	// descriptor is invalid or shaders fail to load.
	bool Create(ResourceManager& ctx, RenderGraph& rg, const CascadedShadowMapDesc& desc,
		const char* vsPath = kCascadedShadowVsPath) {
		if (desc.cascadeCount == 0 || desc.cascadeCount > kMaxCascades)
			return false;
		if (desc.cascadeResolution == 0)
			return false;

		const Extent2D atlas = CascadedShadowAtlasExtent(desc.cascadeResolution, desc.cascadeCount);

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
		attachmentCI.name = "shadow attachment (depth only, stencil dropped)";

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
		sampledCI.name = "shadow sampled";

		Holder<ImageViewHandle> newSampled = ctx.CreateImageView(newImage, sampledCI);
		if (newSampled.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newSampled, ImageLayout::DepthReadOnlyOptimal);
		if (newBindless.empty())
			return false;

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newSampled, desc.format, "shadowMapView1");
		if (newLogical.empty())
			return false;

		image = std::move(newImage);
		attachmentView = std::move(newAttachment);
		sampledView = std::move(newSampled);
		bindless = std::move(newBindless);
		logicalView = newLogical;
		atlasExtent = atlas;
		tileExtent = { desc.cascadeResolution, desc.cascadeResolution };
		format = desc.format;
		cascadeCount = desc.cascadeCount;
		return EnsureShaders(ctx, vsPath);
	}

	// Recreate only when cascadeCount / resolution / format drift.
	// Also internalizes EnsureShaders – one call keeps resources + program valid.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, const CascadedShadowMapDesc& desc,
		const char* vsPath = kCascadedShadowVsPath) {
		if (Valid() &&
			cascadeCount == desc.cascadeCount &&
			tileExtent.width == desc.cascadeResolution &&
			tileExtent.height == desc.cascadeResolution &&
			format == desc.format) {
			return EnsureShaders(ctx, vsPath);
		}
		Destroy(rg);
		return Create(ctx, rg, desc, vsPath);
	}

	// Builds the GPU directional description for the current atlas layout.
	// invView/invProj must come from the Y-flipped camera proj (the same
	// matrices used for rendering), splits from ComputeCascadeSplits.
	// shadowMapIndex is the bindless index of this atlas.
	GpuDirectionalLightDescription BuildDirectionalLight(
		const glm::vec3& lightDir,
		const glm::vec4& color,
		float intensity,
		int32_t shadowMapIndex,
		const glm::mat4& invView,
		const glm::mat4& invProj,
		const std::vector<float>& splits,
		float zPadding = 1.0f) const {
		assert(Valid());
		assert(splits.size() == static_cast<size_t>(cascadeCount + 1));

		GpuDirectionalLightDescription dir{};
		dir.color = color;
		dir.direction[0] = lightDir.x;
		dir.direction[1] = lightDir.y;
		dir.direction[2] = lightDir.z;
		dir.flags = 0;
		dir.intensity = intensity;
		dir.shadow_map_index = shadowMapIndex;
		dir.numCascades = static_cast<int>(cascadeCount);

		const float invAtlasW = 1.0f / static_cast<float>(atlasExtent.width);
		const float invAtlasH = 1.0f / static_cast<float>(atlasExtent.height);
		const uint32_t cols = Columns();

		for (uint32_t i = 0; i < cascadeCount; ++i) {
			glm::mat4 lv, lp;
			GetDirectionalLightOrthoVP(
				lightDir,
				invView,
				invProj,
				splits[i],
				splits[i + 1],
				lv,
				lp,
				zPadding,
				true,
				static_cast<int>(tileExtent.width));

			// NOTE: do NOT Y-flip the light ortho here. Flipping only lp[1][1]
			// (scale) without the offset term lp[3][1] breaks the NDC mapping
			// whenever the light-space Y bounds are asymmetric -> receivers
			// near the slice edge land outside [0,1] UV and the shader's
			// bounds check returns "lit". The flip is also unnecessary: the
			// shadow pass and the sampling share the same lightVP, so the
			// NDC->UV convention cancels out.
		const glm::mat4 lvp = lp * lv;
		std::memcpy(dir.cascades[i].lightVP, glm::value_ptr(lvp), sizeof(dir.cascades[i].lightVP));
		dir.cascades[i].splitEnd = splits[i + 1];
		// Small depth bias per cascade – grows slightly for far cascades.
		dir.cascades[i].bias = 0.0005f * (1.0f + static_cast<float>(i) * 0.5f);
		dir.cascades[i].normalBias = 0.0f;
		dir.cascades[i].pad_ = 0.0f;
		dir.cascades[i].invShadowSize =
			glm::vec2(1.0f / static_cast<float>(tileExtent.width),
				1.0f / static_cast<float>(tileExtent.height));

		const uint32_t x = i % cols;
		const uint32_t y = i / cols;
		const float offX = static_cast<float>(x * tileExtent.width);
		const float offY = static_cast<float>(y * tileExtent.height);
		dir.cascades[i].uvScale = glm::vec2(
			static_cast<float>(tileExtent.width) * invAtlasW,
			static_cast<float>(tileExtent.height) * invAtlasH);
		dir.cascades[i].uvBias = glm::vec2(offX * invAtlasW, offY * invAtlasH);
			const float texelU = 0.5f * invAtlasW;
			const float texelV = 0.5f * invAtlasH;
			dir.cascades[i].uvClampMin = dir.cascades[i].uvBias + glm::vec2(texelU, texelV);
			dir.cascades[i].uvClampMax =
				dir.cascades[i].uvBias + dir.cascades[i].uvScale - glm::vec2(texelU, texelV);
			dir.cascades[i].pad2_[0] = 0;
			dir.cascades[i].pad2_[1] = 0;
		}
		std::memcpy(dir.lightVP, dir.cascades[0].lightVP, sizeof(dir.lightVP));
		return dir;
	}

	// Adds one depth-only pass rendering cascadeIndex into its atlas tile.
	// Every cascade clears its own tile: Execute() turns sub-rect clears into
	// a full-area render pass + explicit vkCmdClearAttachments for the tile
	// (scissor-clipped), so sibling tiles rendered by other passes survive.
	// Caller sets the recording function (bind shadow pipeline + draw
	// casters).
	RenderPass& AddCascadePass(RenderGraph& rg, uint32_t cascadeIndex, const char* name = nullptr) const {
		assert(Valid());
		assert(cascadeIndex < cascadeCount);
		int32_t offX = 0, offY = 0;
		TileOrigin(cascadeIndex, offX, offY);
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

} // namespace smvk
