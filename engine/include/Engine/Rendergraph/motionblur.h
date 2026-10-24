// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// HDR motion blur (velocity gather along motion vectors, pre-tonemap).
//
// Ports the pattern used by BloomChain / Tonemapper into one place:
// resource creation (output image + view + bindless + rendergraph alias) +
// default program ownership + rendergraph pass setup. Sampling is bindless
// (textures[] indexed by push), barriers come from SetColor (write) +
// SetInputImages (read).
//
// Placement: after Main, before Bloom/Tonemap. Input is the HDR scene
// target, output is a same-extent/format blurred HDR image. Bloom and the
// tonemapper sample the blurred output when the pass ran, else the sharp
// HDR directly (no copy when disabled).
//
// Blur: per-pixel gather along the motion vector (prevUV - currUV from the
// MotionVectors pass, RG16F), pre-dilated through a tile map (CoD:AW
// style): a compute phase reduces each 16x16 tile to its max-length
// velocity (motionblur_tiles.cs.hlsl), a second dilates the max over the
// 5x5 tile neighborhood (motionblur_tiledilate.cs.hlsl), and the gather
// adopts the tile velocity when it beats the pixel's own — revealed
// background streaks with the foreground that vacated it instead of
// staying sharp around silhouettes. Taps are phase-offset by Call of
// Duty interleaved gradient noise (Jimenez 2014) so low tap counts read
// as grain, not banding. The vector is scaled (shutter strength),
// clamped to maxRadiusPx, then sampleCount box taps centered on the
// pixel (t in [-0.5, +0.5)). Sub-pixel motion (< 0.5px) early-outs to
// the center tap. Depth-weighted foreground tap rejection (background
// bleeding ONTO movers) is the remaining upgrade path.
//
// Usage (app/src/main.cpp):
//   smvk::MotionBlur motionBlur;
//   motionBlur.Ensure(ctx, rg, renderExtent, desc);   // output + tile maps + programs
//   if (enabled)
//     motionBlur.AddMotionBlurPass(rg, hdrView, hdrBindless,
//                                  motionView, motionBindless,
//                                  linearSampler, renderExtent, desc);
//     // emits MotionBlurTiles + MotionBlurTileDilate (when desc.tileDilation)
//     // ahead of the gather, all in one call
//   ... motionBlur.OutputView() / motionBlur.OutputBindless() ...

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

struct MotionBlurDesc {
	Format format = Format::RGBA16_Float; // must match the HDR target
	uint32_t sampleCount = 12;            // gather taps, 1..32 (1 = copy)
	float maxRadiusPx = 24.0f;            // per-pixel blur clamp (px)
	float velocityScale = 1.0f;           // shutter strength on motion vectors
	bool tileDilation = true;             // emit the tile max/dilate prepasses
	const char* name = "motionBlur";
	const char* viewName = "motionBlurView";
	const char* logicalName = "motionBlurLogical";
};

// Tile side in pixels. Must match [numthreads(16,16,1)] in
// motionblur_tiles.cs.hlsl and the tile math in both compute shaders.
inline constexpr uint32_t kMotionBlurTilePixels = 16;

// Must match motionblur.frag.hlsl MotionBlurPush (52 bytes).
struct MotionBlurPush {
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t motionTextureIndex = UINT32_MAX;
	// Dilated tile-map bindless (motionblur_tiledilate output);
	// UINT32_MAX = no tile prepasses this frame (per-pixel velocities only).
	uint32_t tileVelTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t sampleCount = 12;
	float maxRadiusPx = 24.0f;
	float velocityScale = 1.0f;
	float texelX = 0.0f;
	float texelY = 0.0f;
	// Jitter component of the motion vectors ((prevJitter - currJitter)
	// in UV) - subtracted so the blur follows TRUE camera/object motion;
	// with temporal jitter active the raw vectors are non-zero on a
	// perfectly still camera (they must carry the offset for SMAA T2x /
	// DLSS reprojection). Zero when no jitter is applied.
	float jitterDeltaX = 0.0f;
	float jitterDeltaY = 0.0f;
	// Tile-map texel count (xy as floats): pixel -> tile point lookup
	// (floor(uv * tileScale), clamped). Zero when tiles are off.
	float tileScaleX = 0.0f;
	float tileScaleY = 0.0f;
};
static_assert(sizeof(MotionBlurPush) == 52);

// Must match motionblur_tiles.cs.hlsl MotionBlurTilesPush (32 bytes).
struct MotionBlurTilesPush {
	uint32_t motionTextureIndex = UINT32_MAX;  // bindless RG16F motion vectors
	uint32_t tileMaxTextureIndex = UINT32_MAX; // storage slot (binding 2)
	uint32_t srcWidth = 0;                     // source extent (edge guard)
	uint32_t srcHeight = 0;
	float jitterDeltaX = 0.0f;                 // same transform as the gather
	float jitterDeltaY = 0.0f;
	float velocityScale = 1.0f;
	float pad0 = 0.0f;
};
static_assert(sizeof(MotionBlurTilesPush) == 32);

// Must match motionblur_tiledilate.cs.hlsl MotionBlurTileDilatePush
// (16 bytes).
struct MotionBlurTileDilatePush {
	uint32_t tileMaxTextureIndex = UINT32_MAX; // read via textures[] (binding 0)
	uint32_t tileVelTextureIndex = UINT32_MAX; // write via storage[] (binding 2)
	uint32_t tileCountX = 0;
	uint32_t tileCountY = 0;
};
static_assert(sizeof(MotionBlurTileDilatePush) == 16);

// Default SPVs (fullscreen-triangle VS shared with tonemap + blur PS;
// tile prepasses are standalone compute).
inline constexpr const char* kMotionBlurVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kMotionBlurPsPath = "content/shaders/motionblur.frag.hlsl.spv";
inline constexpr const char* kMotionBlurTilesCsPath =
	"content/shaders/motionblur_tiles.cs.hlsl.spv";
inline constexpr const char* kMotionBlurTileDilateCsPath =
	"content/shaders/motionblur_tiledilate.cs.hlsl.spv";

struct MotionBlur {
	// Blurred HDR output (same extent/format as the HDR target).
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 };
	Format format = Format::RGBA16_Float;

	// Default program (tonemap.vert mainVS + motionblur.frag mainPS).
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};
	MotionBlurDesc desc{};

	// ---- Tile dilation (CoD:AW) ----
	// Raw + dilated per-tile max-velocity maps (RGBA32F storage, one
	// texel per 16x16-px tile, xy = jitter-stripped/scaled velocity UV).
	// tileMax: phase-A reduce output; tileVel: phase-B 5x5 dilate output
	// (the gather's lookup). Both Storage (binding-2 mirror) + Sampled
	// (binding 0) - same lifecycle as the EVSM blur ping-pong.
	Holder<ImageHandle> tileMaxImage;
	Holder<ImageViewHandle> tileMaxView;
	Holder<BindlessImageHandle> tileMaxBindless;
	uint32_t tileMaxBindlessIndex = UINT32_MAX;
	LogicalImageViewHandle tileMaxLogical;
	Holder<ImageHandle> tileVelImage;
	Holder<ImageViewHandle> tileVelView;
	Holder<BindlessImageHandle> tileVelBindless;
	uint32_t tileVelBindlessIndex = UINT32_MAX;
	LogicalImageViewHandle tileVelLogical;
	Extent2D tileCount{ 0, 0 }; // tile-map extent (ceil(target / 16))

	// Tile prepass programs (tiles CS + dilate CS).
	Holder<ShaderModuleHandle> tilesCsModule;
	Holder<ShaderModuleHandle> dilateCsModule;
	ShaderProgram tilesProgram{};
	ShaderProgram dilateProgram{};

	[[nodiscard]] bool TargetValid() const { return !image.empty() && !logicalView.empty(); }
	[[nodiscard]] bool TileTargetsValid() const {
		return !tileMaxImage.empty() && !tileMaxLogical.empty() &&
			!tileVelImage.empty() && !tileVelLogical.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}
	[[nodiscard]] bool ComputeShadersValid() const {
		return tilesProgram.shaderCount != 0 && !tilesCsModule.empty() &&
			dilateProgram.shaderCount != 0 && !dilateCsModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetValid() && ShadersValid(); }
	[[nodiscard]] bool TilesValid() const {
		return TileTargetsValid() && ComputeShadersValid();
	}

	[[nodiscard]] LogicalImageViewHandle OutputView() const { return logicalView; }
	[[nodiscard]] uint32_t OutputBindless() const { return bindlessIndex; }

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kMotionBlurVsPath,
		const char* psPath = kMotionBlurPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kMotionBlurVsPath;
			const std::string ps = psPath ? psPath : kMotionBlurPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "motionBlurVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "motionBlurFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || fragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader ps{ .module = fragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		program = { .shaders = { vs, ps }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		program = ShaderProgram();
		vsModule = {};
		fragModule = {};
	}

	bool EnsureComputeShaders(ResourceManager& ctx,
		const char* tilesCsPath = kMotionBlurTilesCsPath,
		const char* dilateCsPath = kMotionBlurTileDilateCsPath) {
		if (ComputeShadersValid())
			return true;
		try {
			const std::string tiles = tilesCsPath ? tilesCsPath : kMotionBlurTilesCsPath;
			const std::string dilate = dilateCsPath ? dilateCsPath : kMotionBlurTileDilateCsPath;
			if (tilesCsModule.empty())
				tilesCsModule = ctx.CreateShaderModule(readFile(tiles), "motionBlurTilesCS");
			if (dilateCsModule.empty())
				dilateCsModule = ctx.CreateShaderModule(readFile(dilate), "motionBlurTileDilateCS");
		} catch (...) {
			return false;
		}
		if (tilesCsModule.empty() || dilateCsModule.empty())
			return false;
		Shader tilesCS{
			.module = tilesCsModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		tilesProgram = { .shaders = { tilesCS }, .shaderCount = 1 };
		Shader dilateCS{
			.module = dilateCsModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		dilateProgram = { .shaders = { dilateCS }, .shaderCount = 1 };
		return true;
	}

	void DestroyComputeShaders() {
		tilesProgram = ShaderProgram();
		dilateProgram = ShaderProgram();
		tilesCsModule = {};
		dilateCsModule = {};
	}

	// One RGBA32F tile-map slot (Storage for the compute write via the
	// binding-2 mirror, Sampled for the next phase's textures[] read).
	struct TileTarget {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical;
	};

	static bool CreateTileTarget(ResourceManager& ctx, RenderGraph& rg,
		Extent2D tiles, Format format, const char* name, const char* viewName,
		const char* logicalName, TileTarget& out) {
		ImageCreateInfo ci{};
		ci.extent = { tiles.width, tiles.height, 1 };
		ci.format = format;
		ci.usage = ImageUsage::Sampled | ImageUsage::Storage;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = name;

		out = TileTarget{};
		out.image = ctx.CreateImage(ci, nullptr);
		if (out.image.empty())
			return false;

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = viewName;
		out.view = ctx.CreateImageView(out.image, vci);
		if (out.view.empty()) {
			out = TileTarget{};
			return false;
		}

		out.bindless = ctx.CreateBindlessImage(out.view, ImageLayout::ShaderReadOnlyOptimal);
		if (out.bindless.empty()) {
			out = TileTarget{};
			return false;
		}
		out.bindlessIndex = ctx.GetBindlessIndex(out.bindless);

		out.logical = rg.AddExternalImageView(out.view, format, logicalName);
		if (out.logical.empty()) {
			out = TileTarget{};
			return false;
		}
		return true;
	}

	static void DestroyTileTarget(RenderGraph& rg, TileTarget& t) {
		if (!t.logical.empty())
			rg.FreeImageView(t.logical);
		t = TileTarget{};
	}

	bool CreateTarget(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const MotionBlurDesc& d) {
		if (targetExtent.width == 0 || targetExtent.height == 0)
			return false;
		DestroyTarget(rg);

		ImageCreateInfo ci{};
		ci.extent = { targetExtent.width, targetExtent.height, 1 };
		ci.format = d.format;
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = d.name;

		Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
		if (newImage.empty())
			return false;

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = d.viewName;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, d.format, d.logicalName);
		if (newLogical.empty())
			return false;

		// Tile-dilation maps (RGBA32F storage, one texel per 16px tile;
		// guaranteed storage support, same choice as the EVSM atlas).
		Extent2D newTileCount{
			(targetExtent.width + kMotionBlurTilePixels - 1) / kMotionBlurTilePixels,
			(targetExtent.height + kMotionBlurTilePixels - 1) / kMotionBlurTilePixels };
		TileTarget newTileMax, newTileVel;
		if (!CreateTileTarget(ctx, rg, newTileCount, Format::RGBA32_Float,
				"motionBlurTileMax", "motionBlurTileMaxView", "motionBlurTileMaxLogical",
				newTileMax) ||
			!CreateTileTarget(ctx, rg, newTileCount, Format::RGBA32_Float,
				"motionBlurTileVel", "motionBlurTileVelView", "motionBlurTileVelLogical",
				newTileVel)) {
			DestroyTileTarget(rg, newTileMax);
			DestroyTileTarget(rg, newTileVel);
			return false;
		}

		image = std::move(newImage);
		view = std::move(newView);
		bindless = std::move(newBindless);
		bindlessIndex = newBindlessIndex;
		logicalView = newLogical;
		extent = targetExtent;
		format = d.format;
		tileMaxImage = std::move(newTileMax.image);
		tileMaxView = std::move(newTileMax.view);
		tileMaxBindless = std::move(newTileMax.bindless);
		tileMaxBindlessIndex = newTileMax.bindlessIndex;
		tileMaxLogical = newTileMax.logical;
		tileVelImage = std::move(newTileVel.image);
		tileVelView = std::move(newTileVel.view);
		tileVelBindless = std::move(newTileVel.bindless);
		tileVelBindlessIndex = newTileVel.bindlessIndex;
		tileVelLogical = newTileVel.logical;
		tileCount = newTileCount;
		ctx.GetOrCreateSampler(SamplerCreateInfo{});
		return true;
	}

	void DestroyTarget(RenderGraph& rg) {
		if (!logicalView.empty())
			rg.FreeImageView(logicalView);
		logicalView = {};
		bindlessIndex = UINT32_MAX;
		bindless = {};
		view = {};
		image = {};
		extent = { 0, 0 };
		if (!tileMaxLogical.empty())
			rg.FreeImageView(tileMaxLogical);
		tileMaxLogical = {};
		tileMaxBindlessIndex = UINT32_MAX;
		tileMaxBindless = {};
		tileMaxView = {};
		tileMaxImage = {};
		if (!tileVelLogical.empty())
			rg.FreeImageView(tileVelLogical);
		tileVelLogical = {};
		tileVelBindlessIndex = UINT32_MAX;
		tileVelBindless = {};
		tileVelView = {};
		tileVelImage = {};
		tileCount = { 0, 0 };
	}

	// Idempotent output + program creation. Live-updates desc (sample
	// count / radius / strength / tile toggle need no realloc - the tile
	// maps exist whenever the output target does, the desc flag only
	// gates pass emission). Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent,
		const MotionBlurDesc& d,
		const char* vsPath = kMotionBlurVsPath,
		const char* psPath = kMotionBlurPsPath,
		const char* tilesCsPath = kMotionBlurTilesCsPath,
		const char* dilateCsPath = kMotionBlurTileDilateCsPath) {
		desc = d;
		bool ok = true;
		if (!(TargetValid() && TileTargetsValid() &&
				extent.width == targetExtent.width && extent.height == targetExtent.height &&
				format == d.format)) {
			ok = CreateTarget(ctx, rg, targetExtent, d);
		}
		ok = EnsureShaders(ctx, vsPath, psPath) && ok;
		ok = EnsureComputeShaders(ctx, tilesCsPath, dilateCsPath) && ok;
		return ok;
	}

	void Destroy(RenderGraph& rg) {
		DestroyTarget(rg);
		DestroyShaders();
		DestroyComputeShaders();
	}

	// Emits the two tile prepasses (CoD:AW tile dilation):
	//   MotionBlurTiles      - 16x16 group per tile, argmax-reduce the
	//                          jitter-stripped/scaled motion vectors,
	//                          store the max vector in the raw tile map.
	//   MotionBlurTileDilate - one thread per tile, max over the 5x5 tile
	//                          neighborhood into the dilated tile map.
	// The gather (AddMotionBlurPass) point-loads the dilated map and
	// adopts its velocity where it beats the pixel's own. Barriers come
	// from the image hints (read: compute/ShaderReadOnly, write:
	// compute/General - same lifecycle as the EVSM blur ping-pong).
	// Returns the LAST pass (the gather depends on its output).
	RenderPass& AddTilePasses(RenderGraph& rg,
		LogicalImageViewHandle motionView,
		uint32_t motionBindlessIndex,
		Extent2D srcExtent,
		const MotionBlurDesc& overrideDesc,
		float jitterDeltaX = 0.0f,
		float jitterDeltaY = 0.0f,
		const char* tilesName = nullptr,
		const char* dilateName = nullptr) const {
		assert(TilesValid());
		assert(!motionView.empty() && motionBindlessIndex != UINT32_MAX);
		assert(srcExtent.width != 0 && srcExtent.height != 0);
		const uint32_t tilesX = tileCount.width;
		const uint32_t tilesY = tileCount.height;

		MotionBlurTilesPush tPush{};
		tPush.motionTextureIndex = motionBindlessIndex;
		tPush.tileMaxTextureIndex = tileMaxBindlessIndex;
		tPush.srcWidth = srcExtent.width;
		tPush.srcHeight = srcExtent.height;
		tPush.jitterDeltaX = jitterDeltaX;
		tPush.jitterDeltaY = jitterDeltaY;
		tPush.velocityScale = overrideDesc.velocityScale;

		ImageSyncRequest motionRead{};
		motionRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		motionRead.barrierMasks.stageMask = PipelineStageBit_Compute;
		motionRead.layout = ImageLayout::ShaderReadOnlyOptimal;
		ImageSyncRequest tileMaxWrite{};
		tileMaxWrite.barrierMasks.accessMask = AccessFlags::ShaderWrite;
		tileMaxWrite.barrierMasks.stageMask = PipelineStageBit_Compute;
		tileMaxWrite.layout = ImageLayout::General;

		RenderPass& tilesPass = rg.AddComputePass(tilesName ? tilesName : "MotionBlurTiles");
		tilesPass.AddImageHint(motionView, motionRead)
			.AddImageHint(tileMaxLogical, tileMaxWrite);
		ShaderProgram tilesProgCopy = tilesProgram; // handles only, copyable
		tilesPass.SetRecordingFunc([tilesProgCopy, tPush, tilesX, tilesY](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(tilesProgCopy);
			cmd.PushConstants(0, sizeof(tPush), &tPush, ShaderStage::Compute);
			cmd.Dispatch(tilesX, tilesY, 1);
		});

		MotionBlurTileDilatePush dPush{};
		dPush.tileMaxTextureIndex = tileMaxBindlessIndex;
		dPush.tileVelTextureIndex = tileVelBindlessIndex;
		dPush.tileCountX = tilesX;
		dPush.tileCountY = tilesY;

		ImageSyncRequest tileMaxRead{};
		tileMaxRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		tileMaxRead.barrierMasks.stageMask = PipelineStageBit_Compute;
		tileMaxRead.layout = ImageLayout::ShaderReadOnlyOptimal;
		ImageSyncRequest tileVelWrite{};
		tileVelWrite.barrierMasks.accessMask = AccessFlags::ShaderWrite;
		tileVelWrite.barrierMasks.stageMask = PipelineStageBit_Compute;
		tileVelWrite.layout = ImageLayout::General;

		RenderPass& dilatePass = rg.AddComputePass(dilateName ? dilateName : "MotionBlurTileDilate");
		dilatePass.AddImageHint(tileMaxLogical, tileMaxRead)
			.AddImageHint(tileVelLogical, tileVelWrite);
		ShaderProgram dilateProgCopy = dilateProgram;
		dilatePass.SetRecordingFunc([dilateProgCopy, dPush, tilesX, tilesY](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(dilateProgCopy);
			cmd.PushConstants(0, sizeof(dPush), &dPush, ShaderStage::Compute);
			cmd.Dispatch((tilesX + 7) / 8, (tilesY + 7) / 8, 1);
		});
		return dilatePass;
	}

	// Emits the motion blur stack: the two tile prepasses (when
	// overrideDesc.tileDilation and the tile resources are ready - the
	// flag is a live A/B knob, resources exist whenever the target does)
	// followed by the HDR -> blurred-HDR fullscreen gather. Captures only
	// copyable handles/indices, so the passes are safe once this call
	// returns. jitterDeltaX/Y: the jitter component of the motion vectors
	// in UV (see MotionBlurPush) - zero when no temporal jitter is active.
	RenderPass& AddMotionBlurPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle motionView,
		uint32_t motionBindlessIndex,
		uint32_t linearSamplerIndex,
		Extent2D passExtent,
		const MotionBlurDesc& overrideDesc,
		float jitterDeltaX = 0.0f,
		float jitterDeltaY = 0.0f,
		const char* name = nullptr) const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!motionView.empty() && motionBindlessIndex != UINT32_MAX);
		assert(passExtent.width != 0 && passExtent.height != 0);

		// Tile dilation prepasses first (CoD:AW): the gather consumes the
		// dilated map, so ordering is MotionBlurTiles -> dilate -> gather.
		const bool tilesOn = overrideDesc.tileDilation && TilesValid();
		if (tilesOn)
			AddTilePasses(rg, motionView, motionBindlessIndex, passExtent,
				overrideDesc, jitterDeltaX, jitterDeltaY);

		MotionBlurPush push{};
		push.hdrTextureIndex = hdrBindlessIndex;
		push.motionTextureIndex = motionBindlessIndex;
		push.tileVelTextureIndex = tilesOn ? tileVelBindlessIndex : UINT32_MAX;
		push.samplerIndex = linearSamplerIndex;
		push.sampleCount = std::clamp(overrideDesc.sampleCount, 1u, 32u);
		push.maxRadiusPx = (std::max)(0.0f, overrideDesc.maxRadiusPx);
		push.velocityScale = overrideDesc.velocityScale;
		push.texelX = 1.0f / static_cast<float>(passExtent.width);
		push.texelY = 1.0f / static_cast<float>(passExtent.height);
		push.jitterDeltaX = jitterDeltaX;
		push.jitterDeltaY = jitterDeltaY;
		push.tileScaleX = tilesOn ? static_cast<float>(tileCount.width) : 0.0f;
		push.tileScaleY = tilesOn ? static_cast<float>(tileCount.height) : 0.0f;

		RenderPass& pass = rg.AddPass(name ? name : "MotionBlur");
		pass.SetOffset(0, 0)
			.SetExtent(passExtent)
			.SetInputImages(tilesOn
				? std::vector<LogicalImageViewHandle>{ hdrView, motionView, tileVelLogical }
				: std::vector<LogicalImageViewHandle>{ hdrView, motionView })
			.SetColor({ logicalView },
				AttachmentLoadOp::DontCare,
				AttachmentStoreOp::Store);
		ShaderProgram progCopy = program; // ShaderProgram is copyable (handles only)
		pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindRenderPipeline(
				progCopy,
				VertexFormat::Empty,
				DepthSettings::Disabled(),
				StencilSettings::DefaultDisabled(),
				BlendSettings::Opaque(),
				MSAASettings::Count1(),
				RasterSettings{ .cullMode = CullMode::None });
			cmd.PushConstants(0, sizeof(MotionBlurPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
		return pass;
	}

	// Uses the stored desc from Ensure().
	RenderPass& AddMotionBlurPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle motionView,
		uint32_t motionBindlessIndex,
		uint32_t linearSamplerIndex,
		Extent2D passExtent,
		float jitterDeltaX = 0.0f,
		float jitterDeltaY = 0.0f,
		const char* name = nullptr) const {
		return AddMotionBlurPass(rg, hdrView, hdrBindlessIndex,
			motionView, motionBindlessIndex, linearSamplerIndex,
			passExtent, desc, jitterDeltaX, jitterDeltaY, name);
	}
};

} // namespace smvk
