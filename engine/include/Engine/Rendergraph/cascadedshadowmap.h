// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace smvk {

// Maximum cascades supported by GpuDirectionalLightDescription.
static constexpr uint32_t kMaxCascades = 4;

struct CascadedShadowMapDesc {
	uint32_t cascadeCount = 4;
	uint32_t cascadeResolution = 2048; // per-cascade tile, square
	// The coarse VOLUMETRIC shadow atlas (the fog god-ray pass, see
	// WorldRenderer's fog sun-shadow block): 0 = none; >0 creates a
	// SECOND, DEDICATED square D32 atlas of this resolution, fully
	// separate from the cascade grid atlas. The split is deliberate:
	// the main atlas keeps its exact grid extent (every existing
	// consumer - surface PCF, EVSM moments, the fog CSM tap - sees
	// ZERO layout change), and the coarse tile gets its own image the
	// volumetric pass owns end-to-end (its own bindless index + graph
	// view). The dedicated atlas is HALF the cascade resolution at the
	// WorldRenderer call site (a coarse occluder map - 4 MB at
	// 2048-class cascades).
	uint32_t fogShadowResolution = 0;
	Format format = Format::D32_Float; // depth-only atlas, stencil dropped
	float lambda = 0.8f;               // 0 = linear splits, 1 = pure logarithmic (starves far cascade)
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

	// The DEDICATED coarse volumetric atlas (fogShadowResolution > 0):
	// a second square D32 image + its own bindless index and graph
	// view. Empty handles = no volumetric tile (fogTileExtent 0x0).
	Holder<ImageHandle> fogImage;
	Holder<ImageViewHandle> fogAttachmentView;
	Holder<ImageViewHandle> fogSampledView;
	Holder<BindlessImageHandle> fogBindless;
	LogicalImageViewHandle fogLogicalView;

	// Default depth-only program (shadow.vert, mainVS). Created by
	// EnsureShaders/Create/Ensure so apps don't own shadow modules.
	Holder<ShaderModuleHandle> vsModule;
	ShaderProgram program{};

	Extent2D gridExtent{ 0, 0 };   // the cascade grid == the main atlas extent
	Extent2D tileExtent{ 0, 0 };
	Extent2D fogTileExtent{ 0, 0 }; // the DEDICATED atlas's extent (0x0 = none)
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

	// The volumetric atlas's origin in texels (its own image - the
	// origin is trivially (0,0); kept for the pass/rect helpers' single
	// source).
	void FogTileOrigin(int32_t& outX, int32_t& outY) const {
		assert(fogTileExtent.width != 0);
		outX = 0;
		outY = 0;
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
		program = ShaderProgram();
		vsModule = {};
	}

	// Frees both atlas images/views (shaders survive – they are
	// size-independent).
	void Destroy(RenderGraph& rg) {
		if (!logicalView.empty())
			rg.FreeImageView(logicalView);
		logicalView = {};
		bindless = {};
		sampledView = {};
		attachmentView = {};
		image = {};
		if (!fogLogicalView.empty())
			rg.FreeImageView(fogLogicalView);
		fogLogicalView = {};
		fogBindless = {};
		fogSampledView = {};
		fogAttachmentView = {};
		fogImage = {};
		gridExtent = { 0, 0 };
		tileExtent = { 0, 0 };
		fogTileExtent = { 0, 0 };
		cascadeCount = 0;
	}

	// Creates the physical atlases (image + depth/sampled views +
	// bindless) and registers the sampled views with the rendergraph.
	// Also ensures the default shader program. Returns false when the
	// descriptor is invalid or shaders fail to load.
	bool Create(ResourceManager& ctx, RenderGraph& rg, const CascadedShadowMapDesc& desc,
		const char* vsPath = kCascadedShadowVsPath) {
		if (desc.cascadeCount == 0 || desc.cascadeCount > kMaxCascades)
			return false;
		if (desc.cascadeResolution == 0)
			return false;

		// The main atlas is EXACTLY the cascade grid (the pre-volumetric
		// layout - every existing consumer sees zero change; the coarse
		// tile lives in its own dedicated atlas below).
		const Extent2D grid = CascadedShadowAtlasExtent(desc.cascadeResolution, desc.cascadeCount);

		ImageCreateInfo imageCI{};
		imageCI.format = desc.format;
		imageCI.extent = { grid.width, grid.height, 1u };
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

		// ---- The DEDICATED coarse volumetric atlas (fogShadowResolution
		// > 0): a second square D32 image with its own bindless index +
		// graph view. All-or-nothing like the main views above (a failed
		// fog atlas fails Create - the caller logs and disables the
		// shadow passes; no half-created state survives).
		Holder<ImageHandle> newFogImage;
		Holder<ImageViewHandle> newFogAttachment;
		Holder<ImageViewHandle> newFogSampled;
		Holder<BindlessImageHandle> newFogBindless;
		LogicalImageViewHandle newFogLogical;
		const Extent2D fogTile = (desc.fogShadowResolution > 0)
			? Extent2D{ std::max(1u, desc.fogShadowResolution),
				std::max(1u, desc.fogShadowResolution) }
			: Extent2D{ 0, 0 };
		if (fogTile.width != 0) {
			ImageCreateInfo fogCI{};
			fogCI.format = desc.format;
			fogCI.extent = { fogTile.width, fogTile.height, 1u };
			fogCI.mipLevels = 1u;
			fogCI.arrayLayers = 1u;
			fogCI.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled;
			// TransferSrc: debug stage dumps (TOURMALINE_FOG_TILE_DUMP) —
			// armed dumps only (DownloadDataFromImage needs it on the
			// source image).
			if (DebugImageDumpArmed())
				fogCI.usage |= ImageUsage::TransferSrc;
			fogCI.name = "volumetricShadowAtlas";

			newFogImage = ctx.CreateImage(fogCI, nullptr);
			if (newFogImage.empty())
				return false;

			ImageViewCreateInfo fogAttachmentCI{};
			fogAttachmentCI.viewType = ImageViewType::View2D;
			fogAttachmentCI.format = Format::Undefined; // inherit
			fogAttachmentCI.subresourceRange = {
				.aspect = ImageAspect::Depth,
				.baseMipLevel = 0,
				.mipLevelCount = 1,
				.baseArrayLayer = 0,
				.arrayLayerCount = 1,
			};
			fogAttachmentCI.name = "volumetric shadow attachment";

			newFogAttachment = ctx.CreateImageView(newFogImage, fogAttachmentCI);
			if (newFogAttachment.empty())
				return false;

			ImageViewCreateInfo fogSampledCI{};
			fogSampledCI.viewType = ImageViewType::View2D;
			fogSampledCI.format = Format::Undefined; // inherit
			fogSampledCI.subresourceRange = {
				.aspect = ImageAspect::Depth,
				.baseMipLevel = 0,
				.mipLevelCount = 1,
				.baseArrayLayer = 0,
				.arrayLayerCount = 1,
			};
			fogSampledCI.name = "volumetric shadow sampled";

			newFogSampled = ctx.CreateImageView(newFogImage, fogSampledCI);
			if (newFogSampled.empty())
				return false;

			newFogBindless = ctx.CreateBindlessImage(newFogSampled,
				ImageLayout::DepthReadOnlyOptimal);
			if (newFogBindless.empty())
				return false;

			newFogLogical = rg.AddExternalImageView(newFogSampled, desc.format,
				"volumetricShadowMapView");
			if (newFogLogical.empty())
				return false;
		}

		image = std::move(newImage);
		attachmentView = std::move(newAttachment);
		sampledView = std::move(newSampled);
		bindless = std::move(newBindless);
		logicalView = newLogical;
		fogImage = std::move(newFogImage);
		fogAttachmentView = std::move(newFogAttachment);
		fogSampledView = std::move(newFogSampled);
		fogBindless = std::move(newFogBindless);
		fogLogicalView = newFogLogical;
		gridExtent = grid;
		tileExtent = { desc.cascadeResolution, desc.cascadeResolution };
		fogTileExtent = fogTile;
		format = desc.format;
		cascadeCount = desc.cascadeCount;
		return EnsureShaders(ctx, vsPath);
	}

	// Recreate only when cascadeCount / resolution / format / the fog
	// atlas drift.
	// Also internalizes EnsureShaders – one call keeps resources + program valid.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, const CascadedShadowMapDesc& desc,
		const char* vsPath = kCascadedShadowVsPath) {
		const Extent2D wantFog = (desc.fogShadowResolution > 0)
			? Extent2D{ std::max(1u, desc.fogShadowResolution),
				std::max(1u, desc.fogShadowResolution) }
			: Extent2D{ 0, 0 };
		// The fog atlas's handles must agree with the wanted extent (the
		// belt-and-braces twin of the extent compare - a half-created
		// state can never masquerade as valid).
		const bool fogHandlesOk = (wantFog.width == 0)
			? (fogImage.empty() && fogLogicalView.empty())
			: (!fogImage.empty() && !fogBindless.empty()
				&& !fogLogicalView.empty());
		if (Valid()
			&& fogHandlesOk
			&& cascadeCount == desc.cascadeCount
			&& tileExtent.width == desc.cascadeResolution
			&& tileExtent.height == desc.cascadeResolution
			&& fogTileExtent.width == wantFog.width
			&& fogTileExtent.height == wantFog.height
			&& format == desc.format) {
			return EnsureShaders(ctx, vsPath);
		}
		Destroy(rg);
		return Create(ctx, rg, desc, vsPath);
	}

	// Builds the GPU directional description for the current atlas layout.
	// invView/invProj must come from the Y-flipped camera proj (the same
	// matrices used for rendering), splits from ComputeCascadeSplits.
	// shadowMapIndex is the bindless index of this atlas.
	// tanSunAngle: tan(sun disc angular radius) — with softness it drives
	// the per-cascade solar-penumbra scale baked into pad2_[0]
	// (depthRange/orthoW * tanSunAngle): receiver NDC delta * pad2_[0] is
	// the physical penumbra in tile-UV (see SampleEvsmTileSoft).
	GpuDirectionalLightDescription BuildDirectionalLight(
		const glm::vec3& lightDir,
		const glm::vec4& color,
		float intensity,
		int32_t shadowMapIndex,
		const glm::mat4& invView,
		const glm::mat4& invProj,
		const std::vector<float>& splits,
		float zPadding = 1.0f,
		float tanSunAngle = 0.0f,
		float softness = 1.0f,
		// Caster-aware depth coverage (see GetDirectionalLightOrthoVP): the
		// merged AABB of the caster-candidate geometry + the explicit max
		// shadow distance clamp. Null = the slice-only fit.
		const ShadowCasterFitBounds* casterBounds = nullptr,
		float maxShadowDistance = kDirectionalShadowMaxDistance) const {
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

		const float invAtlasW = 1.0f / static_cast<float>(gridExtent.width);
		const float invAtlasH = 1.0f / static_cast<float>(gridExtent.height);
		// LAYOUT NOTE: the main atlas IS the grid (the coarse volumetric
		// tile lives in its own dedicated image - see the desc's split
		// note), so grid normalization is atlas normalization: every
		// consumer (surface PCF, EVSM moments, the fog CSM tap) reads
		// the exact pre-volumetric-tile texels. The coarse tile's own
		// rect is carried separately (FogTileUvRect - identity over its
		// dedicated atlas).
		const uint32_t cols = Columns();

		for (uint32_t i = 0; i < cascadeCount; ++i) {
			glm::mat4 lv, lp;
			float depthRange = 0.0f;
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
				static_cast<int>(tileExtent.width),
				&depthRange,
				casterBounds,
				maxShadowDistance);

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
		// Per-cascade bake (Sec. 4.2): shadowmaps render with no HW bias.
		//  - normalBias (world units): ~2 texels of the cascade ortho, so
		//    far cascades with fat texels push more than near ones. Seals
		//    thin/colliding trim without detaching contact shadows.
		//  - bias (NDC): worldBias/depthRange with the actual ortho depth
		//    range, so the shader's slope-scaled offset has a uniform
		//    world-space effect per cascade (far acne fix).
		// normalBias clamps keep near cascades from going to zero and far
		// ones from washing out tile detail (too much normal offset erases
		// sub-texel self-shadowing, which reads as "barely shadowed").
		const float orthoW = (lp[0][0] != 0.0f) ? 2.0f / lp[0][0] : 1.0f;
		const float orthoH = (lp[1][1] != 0.0f) ? 2.0f / std::abs(lp[1][1]) : 1.0f;
		const float texelWorld = (std::max)(orthoW, orthoH)
			/ static_cast<float>(tileExtent.width);
		const float normalBiasW = (std::min)((std::max)(texelWorld * 1.25f, 0.005f), 0.03f);
		const float depthWorld = (std::min)((std::max)(texelWorld * 0.5f, 0.001f), 0.015f);
		dir.cascades[i].bias = (depthRange > 1e-6f) ? depthWorld / depthRange : 0.0f;
		dir.cascades[i].normalBias = normalBiasW;
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
			// Soft shadows (SampleEvsmTileSoft): pad2_[0] converts the
			// blocker->receiver NDC depth delta into a solar penumbra in
			// tile-UV (worldDist = deltaNDC * depthRange; uv = worldDist *
			// tanSun / orthoW), pad2_[1] is the artistic softness scalar.
			dir.cascades[i].pad2_[0] =
				(depthRange > 1e-6f && orthoW > 1e-6f)
				? (depthRange / orthoW) * std::max(tanSunAngle, 0.0f)
				: 0.0f;
			dir.cascades[i].pad2_[1] = std::max(softness, 0.0f);
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

	// The volumetric atlas's normalized rect: xy = the origin, zw = the
	// size (UV units against its OWN dedicated image - the identity
	// (0,0,1,1); kept as a rect so the shader-side mapping stays
	// source-agnostic). Returns zw = 0 when no atlas exists (the
	// caller disables the lookup).
	glm::vec4 FogTileUvRect() const {
		if (fogTileExtent.width == 0 || fogTileExtent.height == 0)
			return glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
		return glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
	}

	// The dedicated coarse VOLUMETRIC shadow pass (the fog god-ray
	// atlas): one depth-only pass into the dedicated image, same
	// clear/area contract as the cascade passes. The caller's recording
	// function draws ONLY the CastVolumetricShadow-marked casters (the
	// scene flag's walk - never the full surface caster set).
	RenderPass& AddVolumetricShadowPass(RenderGraph& rg,
		const char* name = "VolumetricShadow") const {
		assert(Valid());
		assert(fogTileExtent.width != 0);
		assert(!fogLogicalView.empty());
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(0, 0)
			.SetExtent(fogTileExtent)
			.SetDepth(
				fogLogicalView,
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::Store,
				ClearDepthStencil(1.0f, 0));
		return pass;
	}
};

} // namespace smvk
