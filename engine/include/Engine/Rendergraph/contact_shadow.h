// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Sun contact shadows (the pre-Main depth raymarch): a compute pass
// between the final opaque depth and Main that marches a SHORT ray
// (~0.35 m) from every receiver toward the sun through the SCREEN-SPACE
// depth and writes a per-pixel visibility texture; the lit body
// (default_lit_body.hlsli's sun block) min()s it with the CSM/EVSM
// factor - the contact-hardening detail the cascade texel size cannot
// resolve (the leaf lattice over a wall, the ball in the corner).
//
// PLACEMENT: emitted AFTER the opaque depth is final (prepass + HZB +
// filter + motion-vector legs are done - in 1x mode Main only READS
// renderDepth) and BEFORE AddPass("Main"), whose lit body samples the
// visibility through frame.contactShadowIndex. The 1x prepass depth is
// read in BOTH MSAA modes - the motion-vector argument: MSAA Main
// writes its own transient 2x target, renderDepth is the same opaque
// scene.
//
// The write + read are a matched pair (the probe-premix pattern):
//   - this pass: an ImageSyncRequest WRITE hint (General) on the
//     visibility view;
//   - Main: an AddImageHint READ (Fragment, ShaderReadOnlyOptimal) -
//     CollectPassUsages only sees attachments and explicit hints, a
//     bindless fetch is invisible to it; without the hint pair Main
//     could sort and execute BEFORE this pass and sample last frame's
//     (or garbage) visibility. The same hint pair also drives the
//     General -> ShaderReadOnlyOptimal transition.
//
// The bindless index is dual-visible (one descriptor, both arrays - the
// evsm_resolve/evsm_blur proof): this pass writes it through the
// storage array (binding 2), Main's lit body samples it through
// textures[]. Because DXC infers the storage element format from the
// HLSL type (RWTexture2D<float> -> R32f), the target is R32_Float -
// an R8_UNORM view would mismatch the inferred format and the write is
// undefined for the whole image (the evsm float4 <-> RGBA32F rule).
// 8.3 MiB at 1080p full-res, 2.1 MiB half - the format note is the
// only deviation from the plan's R8_UNORM ask, and it is forced by the
// storage-image format-identity contract.
//
// Toggles: WorldRendererConfig::contactShadows (static) +
// m_contactShadowsEnabled (runtime: the debug-panel checkbox +
// TOURMALINE_CONTACT_SHADOWS env pin) + the tunables
// (TOURMALINE_CONTACT_SHADOWS_STEPS / _RES; distance/thickness/bias on
// the panel sliders). No temporal filtering in v1 (the initial tuning
// table) - the half/full resolution choice is exposed precisely to be
// benchmarked, not assumed (half saves the tracing but can lose thin
// occluders, which are the effect's point).

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace smvk {

// Must match contact_shadow.cs.hlsl ContactShadowPush (56 bytes).
// uint64 BDA leads; all-scalar after (the spirv-val straddling-vector
// hazard discipline - same shape as every SSR/godrays push).
struct ContactShadowPush {
	uint64_t frameUniforms = 0;       // BDA of the FrameUniforms slice
	uint32_t depthTextureIndex = UINT32_MAX; // full-res opaque depth
	uint32_t outImageIndex = UINT32_MAX;     // bindless storage index (write)
	uint32_t outWidth = 0;            // visibility extent (dispatch domain)
	uint32_t outHeight = 0;
	float invDepthW = 0.0f;           // 1 / FULL-res depth extent
	float invDepthH = 0.0f;
	float maxDistance = 0.35f;        // march length (m)
	float thickness = 0.03f;          // thin-occluder acceptance band (m)
	float startBias = 0.01f;          // ray start + self-hit delta floor (m)
	uint32_t steps = 12;              // march samples, 4..32
	uint32_t samplerIndex = 0;        // point/clamp (hard depth taps)
	uint32_t pad0 = 0;
};
static_assert(sizeof(ContactShadowPush) == 56);

// ---- desc ----

struct ContactShadowDesc {
	// The initial tuning table (meter-scale worlds): 0.35 m distance,
	// 12 samples, 0.03 m thickness, 0.01 m start bias, HALF resolution.
	// Every value is a live knob (the panel sliders + the env pins) -
	// these are starting points to tune against real scenes, not
	// constants with authority.
	float maxDistance = 0.35f;
	float thickness = 0.03f;
	float startBias = 0.01f;
	uint32_t steps = 12;         // 4..32
	uint32_t resolutionDivisor = 2; // 1 = full, 2 = half (benchmark both)
};

inline constexpr const char* kContactShadowCsPath =
	"content/shaders/contact_shadow.cs.hlsl.spv";

struct SunContactShadows {
	// The house Target: image + view + bindless + logical, lifecycle
	// managed together (the godrays/ssr shape).
	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	// R32_Float (the storage format-identity note in the file header):
	// Sampled for Main's bindless fetch, Storage for the compute write.
	Target visibility;

	Extent2D extent{ 0, 0 };       // full-res extent (= depth/HDR extent)
	Extent2D visExtent{ 0, 0 };    // extent / resolutionDivisor
	uint32_t divisor = 2;          // captured so a live flip recreates

	Holder<ShaderModuleHandle> csModule;
	ShaderProgram program{};

	[[nodiscard]] bool TargetsValid() const { return visibility.Valid(); }
	[[nodiscard]] bool ShadersValid() const { return program.shaderCount != 0 && !csModule.empty(); }
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid(); }

	// Consumer accessors: the lit body's bindless fetch index and the
	// Main read-hint's logical view.
	[[nodiscard]] uint32_t OutputIndex() const { return visibility.bindlessIndex; }
	[[nodiscard]] LogicalImageViewHandle OutputLogical() const { return visibility.logical; }

	static Extent2D VisExtentFor(Extent2D full, uint32_t div) {
		div = (div > 0u) ? div : 1u;
		return { std::max(1u, full.width / div), std::max(1u, full.height / div) };
	}

	bool EnsureShaders(ResourceManager& ctx) {
		if (ShadersValid())
			return true;
		try {
			if (csModule.empty())
				csModule = ctx.CreateShaderModule(readFile(kContactShadowCsPath),
					"contactShadowCS");
		} catch (...) {
			// LOUD on purpose (the hzb_max precedent): a silent shader-load
			// failure turns the whole effect into a no-op through the
			// Valid() gate - it must never hide.
			std::fprintf(stderr,
				"[ContactShadows] shader unavailable (%s) - the effect stays off\n",
				kContactShadowCsPath);
			return false;
		}
		if (csModule.empty()) {
			std::fprintf(stderr,
				"[ContactShadows] shader module creation failed (%s) - the effect stays off\n",
				kContactShadowCsPath);
			return false;
		}
		Shader cs{ .module = csModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		program = { .shaders = { cs }, .shaderCount = 1 };
		return true;
	}

	void DestroyShaders() {
		program = ShaderProgram();
		csModule = {};
	}

	static bool CreateTarget(ResourceManager& ctx, RenderGraph& rg,
		Extent2D ext, const char* name, Target& out) {
		if (ext.width == 0 || ext.height == 0)
			return false;
		ImageCreateInfo ci{};
		ci.extent = { ext.width, ext.height, 1 };
		ci.format = Format::R32_Float;
		// Sampled: Main's lit body fetch. Storage: this pass's compute
		// write. The bindless descriptor is dual-visible by construction
		// (the evsm pattern).
		ci.usage = ImageUsage::Sampled | ImageUsage::Storage;
		// TransferSrc: the TOURMALINE_CONTACT_SHADOWS_DUMP one-shot
		// downloads the visibility through DownloadDataFromImage -
		// armed dumps only (the SSR target pattern).
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = name;

		Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
		if (newImage.empty())
			return false;

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = name;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		// General: the storage write is the first use (the blurTempBindless
		// precedent); the Main read hint transitions to
		// ShaderReadOnlyOptimal from there.
		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::General);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, Format::R32_Float, name);
		if (newLogical.empty())
			return false;

		Target t;
		t.image = std::move(newImage);
		t.view = std::move(newView);
		t.bindless = std::move(newBindless);
		t.bindlessIndex = newBindlessIndex;
		t.logical = newLogical;
		out = std::move(t);
		return true;
	}

	static void DestroyTarget(RenderGraph& rg, Target& t) {
		if (!t.logical.empty())
			rg.FreeImageView(t.logical);
		t = Target{};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full,
		const ContactShadowDesc& d) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		const Extent2D vis = VisExtentFor(full, d.resolutionDivisor);

		Target newVis;
		if (!CreateTarget(ctx, rg, vis, "sunContactVisibility", newVis))
			return false;
		visibility = std::move(newVis);
		extent = full;
		visExtent = vis;
		divisor = d.resolutionDivisor;
		std::fprintf(stderr,
			"[ContactShadows] target: full %ux%u -> vis %ux%u (divisor %u)\n",
			full.width, full.height, vis.width, vis.height, d.resolutionDivisor);
		return EnsureShaders(ctx);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, visibility);
		extent = { 0, 0 };
		visExtent = { 0, 0 };
	}

	// Recreate only on extent/divisor drift (live-updates desc through
	// the emission site otherwise - the godrays Ensure shape).
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D full,
		const ContactShadowDesc& d) {
		if (TargetsValid()
			&& extent.width == full.width && extent.height == full.height
			&& divisor == d.resolutionDivisor) {
			return EnsureShaders(ctx);
		}
		return Create(ctx, rg, full, d);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyShaders();
	}

	// ---- pass emission ----
	//
	// ONE compute pass: read the full-res opaque depth (sampled, bindless
	// index), write the visibility through the storage array (bindless
	// index). Captures only copyable handles/indices (the recording
	// lambda runs at RenderGraph::Execute - never capture `this` of a
	// target that may be recreated). The Main-side read hint (the paired
	// edge) is attached by the caller - see the file header.
	void AddPass(RenderGraph& rg,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t pointSamplerIndex,
		const ContactShadowDesc& d,
		const char* name = "SunContactShadows") const {
		assert(Valid());
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);
		assert(visExtent.width != 0 && visExtent.height != 0);

		ContactShadowPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.depthTextureIndex = depthBindlessIndex;
		push.outImageIndex = visibility.bindlessIndex;
		push.outWidth = visExtent.width;
		push.outHeight = visExtent.height;
		push.invDepthW = 1.0f / static_cast<float>(std::max(1u, extent.width));
		push.invDepthH = 1.0f / static_cast<float>(std::max(1u, extent.height));
		push.maxDistance = (std::max)(d.maxDistance, 0.01f);
		push.thickness = (std::max)(d.thickness, 0.001f);
		push.startBias = (std::max)(d.startBias, 0.0f);
		push.steps = std::clamp(d.steps, 4u, 32u);
		push.samplerIndex = pointSamplerIndex;
		push.pad0 = 0;

		RenderPass& pass = rg.AddComputePass(name ? name : "SunContactShadows");
		{
			// The opaque depth, read as a sampled texture from compute
			// (the SSRClassify/HZB shape): DepthStencilReadOnlyOptimal.
			ImageSyncRequest depthReq{};
			depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
			depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
			depthReq.layout = ImageLayout::DepthStencilReadOnlyOptimal;
			pass.AddImageHint(depthView, depthReq);
		}
		{
			// The visibility write: General (storage), the paired READ
			// hint on Main transitions it to ShaderReadOnlyOptimal.
			ImageSyncRequest visReq{};
			visReq.barrierMasks.accessMask = AccessFlags::ShaderWrite;
			visReq.barrierMasks.stageMask = PipelineStageBit_Compute;
			visReq.layout = ImageLayout::General;
			pass.AddImageHint(visibility.logical, visReq);
		}

		ShaderProgram progCopy = program; // copyable (handles only)
		const uint32_t w = visExtent.width;
		const uint32_t h = visExtent.height;
		pass.SetRecordingFunc([progCopy, push, w, h](CommandBuffer& cmd) mutable {
			cmd.BindComputePipeline(progCopy);
			cmd.PushConstants(0, sizeof(ContactShadowPush), &push,
				ShaderStage::Compute);
			cmd.Dispatch((w + 7) / 8, (h + 7) / 8, 1);
		});
	}
};

} // namespace smvk
