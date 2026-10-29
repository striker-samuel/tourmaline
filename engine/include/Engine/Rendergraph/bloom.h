// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Composable bloom chain (Karis-style downsample / tent upsample).
//
// Ports the pattern used by CascadedShadowMap / MipChain into one place:
// resource creation (separated per-mip images via MipBuilder) + mip math +
// rendergraph pass setup. Sampling is bindless (textures[] indexed by push),
// barriers come from SetColor (write) + SetInputImages (read).
//
// Chain layout:
//   base = hdrExtent / baseDivisor (default half-res), mip[i+1] = mip[i] / 2.
//   down (SPD, default): one compute dispatch cascades all 5 mips through
//          groupshared (2x2 Karis per level, prefilter on mip 0) — see
//          spd_downsample.cs.hlsl. Falls back to the raster path below
//          when useSpd is off, mipCount != 5, or the SPD program failed.
//   down (raster fallback):
//          mip[0] = prefilter(HDR, threshold+knee) + 13-tap downsample
//          mip[i] = 13-tap downsample(mip[i-1])            (DontCare -> Store)
//   up:    mip[i] += tentUpsample(mip[i+1])  (Load -> Store, Blend::Add)
// After the up chain, mip[0] holds the combined bloom (HDR, pre-tonemap).
// Composite in the tonemapper: hdr += bloom * intensity, then ACES.
//
// Why separated images (not LogicalMipChain single image)?
// RenderGraph tracks barriers per PHYSICAL image with one whole-image
// layout, so sampling mip N-1 while rendering mip N of the same VkImage
// aliases layouts. MipBuilder::BuildSeparated gives each mip its own
// image -> independent barriers, no aliasing.
//
// Usage (app/src/main.cpp):
//   smvk::BloomChain bloom;
//   bloom.Ensure(ctx, rg, hdrExtent, desc);   // resources + default programs
//   bloom.AddBloomPasses(rg, hdrLogical, hdrBindless, linearSampler);
//   tonemap samples bloom.MipBindless(0) as bloomTexture.

#include <Engine/Rendergraph/mipchain.h>
#include <Engine/platform.h>

#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

struct BloomDesc {
	uint32_t mipCount = 5;               // number of chain levels
	Format format = Format::RGBA16_Float; // HDR, must be color-renderable
	uint32_t baseDivisor = 2;            // base = hdr / divisor (half-res)
	float threshold = 1.0f;              // prefilter knee threshold (mip 0)
	float knee = 0.25f;                  // soft-knee width
	float intensity = 0.6f;              // composite strength (tonemap side)
	float upsampleRadius = 1.0f;         // tent radius in higher-mip texels
	bool useSpd = true;                  // single-pass compute downsample (needs mipCount == 5)
	const char* name = "bloom";
};

// Must match bloom_down.frag.hlsl BloomDownPush (32 bytes).
struct BloomDownPush {
	uint32_t srcTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	float threshold = 1.0f;
	float knee = 0.25f;
	float srcTexelX = 0.0f;
	float srcTexelY = 0.0f;
	uint32_t applyThreshold = 0;
	uint32_t pad = 0;
};
static_assert(sizeof(BloomDownPush) == 32);

// Must match bloom_up.frag.hlsl BloomUpPush (32 bytes).
struct BloomUpPush {
	uint32_t srcTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	float radius = 1.0f;
	float pad0 = 0.0f;
	float srcTexelX = 0.0f;
	float srcTexelY = 0.0f;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(BloomUpPush) == 32);

// Must match spd_downsample.cs.hlsl SpdPush (48 bytes).
// The cascade handles exactly 5 levels (16x16 workgroup tile), so the
// SPD path requires mipCount == 5 — anything else uses raster downsample.
struct SpdPush {
	uint32_t srcIndex = UINT32_MAX;   // bindless textures[] index of HDR
	uint32_t dstIndex0 = UINT32_MAX;   // bindless storage indices of the 5 mips
	uint32_t dstIndex1 = UINT32_MAX;
	uint32_t dstIndex2 = UINT32_MAX;
	uint32_t dstIndex3 = UINT32_MAX;
	uint32_t dstIndex4 = UINT32_MAX;
	uint32_t baseWidth = 0;            // bloom mip 0 extent (tiling + bounds)
	uint32_t baseHeight = 0;
	uint32_t hdrWidth = 0;             // HDR source extent (tap clamp)
	uint32_t hdrHeight = 0;
	float threshold = 1.0f;            // soft-knee prefilter (mip 0 only)
	float knee = 0.25f;
};
static_assert(sizeof(SpdPush) == 48);
static constexpr uint32_t kSpdMipLevels = 5; // cascade depth the shader implements
static constexpr uint32_t kSpdBlock = 16;    // workgroup tile of mip 0 pixels

// Default SPVs (fullscreen-triangle VS shared with tonemap + down/up PS).
inline constexpr const char* kBloomFullscreenVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kBloomDownPsPath = "content/shaders/bloom_down.frag.hlsl.spv";
inline constexpr const char* kBloomUpPsPath = "content/shaders/bloom_up.frag.hlsl.spv";
inline constexpr const char* kBloomSpdCsPath = "content/shaders/spd_downsample.cs.hlsl.spv";

struct BloomChain {
	MipBuilder::BuiltChain chain{};
	BloomDesc desc{};
	Extent2D hdrExtent{ 0, 0 };

	// Default programs (fullscreen VS + down/up PS, SPD compute CS).
	// Created by EnsureShaders/Create/Ensure so apps don't own bloom modules.
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> downFragModule;
	Holder<ShaderModuleHandle> upFragModule;
	Holder<ShaderModuleHandle> spdModule;
	ShaderProgram downProgram{};
	ShaderProgram upProgram{};
	ShaderProgram spdProgram{};

	[[nodiscard]] bool Valid() const { return chain.Valid(); }
	[[nodiscard]] bool ShadersValid() const {
		return downProgram.shaderCount != 0 && upProgram.shaderCount != 0 &&
			!vsModule.empty() && !downFragModule.empty() && !upFragModule.empty();
	}
	[[nodiscard]] bool SpdShadersValid() const {
		return spdProgram.shaderCount != 0 && !spdModule.empty();
	}
	// SPD replaces the raster down chain only for the integer it was
	// written for (5 mips); anything else keeps the 13-tap path.
	[[nodiscard]] bool SpdPathAvailable() const {
		return desc.useSpd && chain.MipCount() == kSpdMipLevels && SpdShadersValid();
	}
	[[nodiscard]] uint32_t MipCount() const { return chain.MipCount(); }

	[[nodiscard]] Extent2D MipExtent(uint32_t i) const {
		assert(i < chain.mips.size());
		return chain.mips[i].extent;
	}
	[[nodiscard]] LogicalImageViewHandle MipView(uint32_t i) const {
		assert(i < chain.mips.size());
		return chain.mips[i].logical;
	}
	[[nodiscard]] uint32_t MipBindless(uint32_t i) const {
		assert(i < chain.mips.size());
		return chain.mips[i].bindlessIndex;
	}

	static Extent2D BaseExtentFor(Extent2D hdr, uint32_t divisor) {
		divisor = divisor == 0 ? 1 : divisor;
		return {
			std::max(1u, hdr.width / divisor),
			std::max(1u, hdr.height / divisor),
		};
	}

	// Best-effort SPD program creation (compute CS). Failure only falls
	// back to the raster downsample — never fails EnsureShaders.
	bool EnsureSpdShaders(ResourceManager& ctx,
		const char* csPath = kBloomSpdCsPath) {
		if (SpdShadersValid())
			return true;
		try {
			const std::string cs = csPath ? csPath : kBloomSpdCsPath;
			if (spdModule.empty())
				spdModule = ctx.CreateShaderModule(readFile(cs), "bloomSpdCS");
		} catch (...) {
			return false;
		}
		if (spdModule.empty())
			return false;
		Shader cs{ .module = spdModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		spdProgram = { .shaders = { cs }, .shaderCount = 1 };
		return true;
	}

	// Idempotent default program creation (fullscreen VS + down/up PS).
	// Returns true when both programs are ready afterwards. SPD is
	// best-effort on top (a missing SPD SPV keeps the raster path alive).
	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kBloomFullscreenVsPath,
		const char* downPath = kBloomDownPsPath,
		const char* upPath = kBloomUpPsPath,
		const char* spdPath = kBloomSpdCsPath) {
		if (!ShadersValid()) {
			try {
				const std::string vs = vsPath ? vsPath : kBloomFullscreenVsPath;
				const std::string down = downPath ? downPath : kBloomDownPsPath;
				const std::string up = upPath ? upPath : kBloomUpPsPath;
				if (vsModule.empty())
					vsModule = ctx.CreateShaderModule(readFile(vs), "bloomFullscreenVS");
				if (downFragModule.empty())
					downFragModule = ctx.CreateShaderModule(readFile(down), "bloomDownFrag");
				if (upFragModule.empty())
					upFragModule = ctx.CreateShaderModule(readFile(up), "bloomUpFrag");
			} catch (...) {
				return false;
			}
			if (vsModule.empty() || downFragModule.empty() || upFragModule.empty())
				return false;
			Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
			Shader downPS{ .module = downFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
			Shader upPS{ .module = upFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
			downProgram = { .shaders = { vs, downPS }, .shaderCount = 2 };
			upProgram = { .shaders = { vs, upPS }, .shaderCount = 2 };
		}
		EnsureSpdShaders(ctx, spdPath);
		return true;
	}

	void DestroyShaders() {
		downProgram = ShaderProgram();
		upProgram = ShaderProgram();
		spdProgram = ShaderProgram();
		vsModule = {};
		downFragModule = {};
		upFragModule = {};
		spdModule = {};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D hdr, const BloomDesc& d,
		const char* vsPath = kBloomFullscreenVsPath,
		const char* downPath = kBloomDownPsPath,
		const char* upPath = kBloomUpPsPath,
		const char* spdPath = kBloomSpdCsPath) {
		if (hdr.width == 0 || hdr.height == 0 || d.mipCount == 0)
			return false;
		Destroy(rg);
		const Extent2D base = BaseExtentFor(hdr, d.baseDivisor);
		// Storage usage + General bindless registration: the SPD compute
		// pass stores into these mips (the raster upsample still samples
		// them — sampling a General-registered image is the EVSM blurTemp
		// precedent).
		MipBuilder::BuiltChain built = MipBuilder::BuildSeparated(
			ctx, rg, base, d.mipCount, d.format,
			ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::Storage,
			d.name,
			ImageLayout::General);
		if (!built.Valid())
			return false;
		chain = std::move(built);
		desc = d;
		hdrExtent = hdr;
		return EnsureShaders(ctx, vsPath, downPath, upPath, spdPath);
	}

	void Destroy(RenderGraph& rg) {
		MipBuilder::DestroySeparated(rg, chain);
		hdrExtent = { 0, 0 };
	}

	// Recreate only when hdr extent / mipCount / format / divisor drift.
	// Also internalizes EnsureShaders – one call keeps resources + programs valid.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D hdr, const BloomDesc& d,
		const char* vsPath = kBloomFullscreenVsPath,
		const char* downPath = kBloomDownPsPath,
		const char* upPath = kBloomUpPsPath,
		const char* spdPath = kBloomSpdCsPath) {
		const Extent2D wantBase = BaseExtentFor(hdr, d.baseDivisor);
		if (Valid() &&
			hdrExtent.width == hdr.width && hdrExtent.height == hdr.height &&
			chain.MipCount() == d.mipCount && chain.format == d.format &&
			chain.baseExtent.width == wantBase.width &&
			chain.baseExtent.height == wantBase.height) {
			desc = d; // live-update threshold/intensity without realloc
			return EnsureShaders(ctx, vsPath, downPath, upPath, spdPath);
		}
		return Create(ctx, rg, hdr, d, vsPath, downPath, upPath, spdPath);
	}

	// ---- Low-level pass shells (caller sets the recording func) ----
	// Down: dst=mip[i] (DontCare), src=caller-provided view (HDR for 0).
	RenderPass& AddDownsamplePass(RenderGraph& rg, uint32_t mipIndex,
		LogicalImageViewHandle srcView, const char* name = nullptr) const {
		assert(Valid());
		assert(mipIndex < chain.MipCount());
		assert(!srcView.empty());
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(0, 0)
			.SetExtent(chain.mips[mipIndex].extent)
			.SetInputImages({ srcView })
			.SetColor({ chain.mips[mipIndex].logical },
				AttachmentLoadOp::DontCare,
				AttachmentStoreOp::Store);
		return pass;
	}

	// SPD down: ONE compute dispatch writes all 5 mips from HDR (each
	// mip its own physical image, so no layout aliasing — barriers come
	// from the image hints, same as the EVSM blur legs). Replaces the n
	// raster AddDownsamplePass passes when SpdPathAvailable().
	RenderPass& AddSpdDownsamplePass(RenderGraph& rg,
		ShaderProgram& spdProg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		const char* name = "BloomSPD") const {
		assert(Valid());
		assert(chain.MipCount() == kSpdMipLevels);
		assert(!hdrView.empty());

		RenderPass& pass = rg.AddComputePass(name);

		ImageSyncRequest srcR{};
		srcR.barrierMasks.accessMask = AccessFlags::ShaderRead;
		srcR.barrierMasks.stageMask = PipelineStageBit_Compute;
		srcR.layout = ImageLayout::ShaderReadOnlyOptimal;
		ImageSyncRequest dstW{};
		dstW.barrierMasks.accessMask = AccessFlags::ShaderWrite;
		dstW.barrierMasks.stageMask = PipelineStageBit_Compute;
		dstW.layout = ImageLayout::General;
		pass.AddImageHint(hdrView, srcR);
		for (uint32_t i = 0; i < kSpdMipLevels; ++i)
			pass.AddImageHint(chain.mips[i].logical, dstW);

		SpdPush push{};
		push.srcIndex = hdrBindlessIndex;
		push.dstIndex0 = chain.mips[0].bindlessIndex;
		push.dstIndex1 = chain.mips[1].bindlessIndex;
		push.dstIndex2 = chain.mips[2].bindlessIndex;
		push.dstIndex3 = chain.mips[3].bindlessIndex;
		push.dstIndex4 = chain.mips[4].bindlessIndex;
		push.baseWidth = chain.baseExtent.width;
		push.baseHeight = chain.baseExtent.height;
		push.hdrWidth = hdrExtent.width;
		push.hdrHeight = hdrExtent.height;
		push.threshold = desc.threshold;
		push.knee = desc.knee;

		const uint32_t groupsX = (chain.baseExtent.width + kSpdBlock - 1) / kSpdBlock;
		const uint32_t groupsY = (chain.baseExtent.height + kSpdBlock - 1) / kSpdBlock;
		pass.SetRecordingFunc([spdProg, push, groupsX, groupsY](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(spdProg);
			cmd.PushConstants(0, sizeof(SpdPush), &push, ShaderStage::Compute);
			cmd.Dispatch(groupsX, groupsY, 1);
		});
		return pass;
	}

	// Up: dst=mip[i] (Load, caller blends Add), src=mip[i+1].
	RenderPass& AddUpsamplePass(RenderGraph& rg, uint32_t mipIndex,
		const char* name = nullptr) const {
		assert(Valid());
		assert(mipIndex + 1 < chain.MipCount());
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(0, 0)
			.SetExtent(chain.mips[mipIndex].extent)
			.SetInputImages({ chain.mips[mipIndex + 1].logical })
			.SetColor({ chain.mips[mipIndex].logical },
				AttachmentLoadOp::Load,
				AttachmentStoreOp::Store);
		return pass;
	}

	// ---- One-call helper: emits down chain + up chain with recordings ----
	// Explicit-program overload (custom VS/PS). Captures only copyable
	// handles/indices, so the returned passes are safe once this returns.
	void AddBloomPasses(RenderGraph& rg,
		ShaderProgram& downProg,
		ShaderProgram& upProg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		uint32_t linearSamplerIndex) const {
		assert(Valid());
		assert(!hdrView.empty());

		const uint32_t n = chain.MipCount();

		// Down chain: mip 0 prefilters HDR, rest downsample previous mip.
		for (uint32_t i = 0; i < n; ++i) {
			const bool isFirst = (i == 0);
			LogicalImageViewHandle srcView = isFirst ? hdrView : chain.mips[i - 1].logical;
			uint32_t srcBindless = isFirst ? hdrBindlessIndex : chain.mips[i - 1].bindlessIndex;
			Extent2D srcExtent = isFirst ? hdrExtent : chain.mips[i - 1].extent;

			BloomDownPush push{};
			push.srcTextureIndex = srcBindless;
			push.samplerIndex = linearSamplerIndex;
			push.threshold = desc.threshold;
			push.knee = desc.knee;
			push.srcTexelX = 1.0f / static_cast<float>(std::max(1u, srcExtent.width));
			push.srcTexelY = 1.0f / static_cast<float>(std::max(1u, srcExtent.height));
			push.applyThreshold = isFirst ? 1u : 0u;
			push.pad = 0;

			const char* name = isFirst ? "BloomDown0" : nullptr;
			RenderPass& pass = AddDownsamplePass(rg, i, srcView, name);
			pass.SetRecordingFunc([downProg, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					downProg,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(BloomDownPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0);
			});
		}

		// Up chain coarse->fine, additive onto the downsampled content.
		AddUpsampleChain(rg, upProg, linearSamplerIndex);
	}

	// Up chain coarse->fine, additive onto the downsampled content.
	// Shared by the raster and SPD down paths (the upsample never changed).
	void AddUpsampleChain(RenderGraph& rg,
		ShaderProgram& upProg,
		uint32_t linearSamplerIndex) const {
		assert(Valid());
		const uint32_t n = chain.MipCount();
		for (int32_t i = static_cast<int32_t>(n) - 2; i >= 0; --i) {
			const uint32_t dst = static_cast<uint32_t>(i);
			const uint32_t src = static_cast<uint32_t>(i + 1);
			Extent2D srcExtent = chain.mips[src].extent;

			BloomUpPush push{};
			push.srcTextureIndex = chain.mips[src].bindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.radius = desc.upsampleRadius;
			push.pad0 = 0.0f;
			push.srcTexelX = 1.0f / static_cast<float>(std::max(1u, srcExtent.width));
			push.srcTexelY = 1.0f / static_cast<float>(std::max(1u, srcExtent.height));
			push.pad1 = 0;
			push.pad2 = 0;

			RenderPass& pass = AddUpsamplePass(rg, dst, nullptr);
			pass.SetRecordingFunc([upProg, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					upProg,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Add(), // src ONE + dst ONE
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(BloomUpPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0);
			});
		}
	}

	// Default overload: uses the internal downProgram/upProgram created by
	// EnsureShaders/Create/Ensure. Asserts when shaders aren't ready.
	// Prefers the SPD single-dispatch downsample; falls back to raster.
	void AddBloomPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		uint32_t linearSamplerIndex) const {
		assert(ShadersValid());
		// const_cast is safe: the callees only copy the programs.
		if (SpdPathAvailable()) {
			AddSpdDownsamplePass(rg,
				const_cast<ShaderProgram&>(spdProgram),
				hdrView, hdrBindlessIndex);
			AddUpsampleChain(rg,
				const_cast<ShaderProgram&>(upProgram),
				linearSamplerIndex);
			return;
		}
		AddBloomPasses(rg,
			const_cast<ShaderProgram&>(downProgram),
			const_cast<ShaderProgram&>(upProgram),
			hdrView, hdrBindlessIndex, linearSamplerIndex);
	}
};

} // namespace smvk
