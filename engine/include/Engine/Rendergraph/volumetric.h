#pragma once
// Raymarched volumetric fog lit by the baked SG-GI probe volume.
//
// Single-scatter participating media composited in HDR:
//   - Raymarch camera -> scene depth (reversed-Z, exact invProj unproject).
//   - Beer-Lambert extinction + isotropic GI ambient from SgGiSample +
//     analytic sun single-scatter with Henyey-Greenstein phase.
//   - The GI probes carry the COMPLETE baked field (sky + sun/spot bounce),
//     so in-scatter ambient is probe-driven where covered, sky-IBL fallback
//     where not -- same discipline as default_lit.
// Placement: after SSR, before Exposure/Bloom/Tonemap so fog meters/blooms/
// tonemaps with the scene. Input = scene HDR (+ depth), output = same-extent
// HDR with fog composited (no copy when disabled).
//
// Quality model (adjustable):
//   - VolumetricQuality preset -> march steps. Low=8, Medium=16, High=32,
//     Ultra=64 steps.
//   - The composite ALWAYS runs at full render resolution (like SSR): a
//     half-res fog target breaks downstream consumers — the exposure meter
//     .Load()s the scene HDR at full-res pixel coords (out-of-bounds reads
//     on a smaller image), DLSS takes colorIn at renderExtent, and the
//     depth subsample alternates ray lengths at silhouettes (checkered
//     halos after the bilinear upscale). resolutionDivisor is kept at 1;
//     quality scales with steps only.

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

// ---------------------------------------------------------------------------
// Quality presets. Custom = user edited steps/divisor by hand (UI sets this
// automatically when a slider diverges from the preset).
// ---------------------------------------------------------------------------
enum class VolumetricQuality : int {
	Low = 0,     // 8 steps, half-res
	Medium = 1,  // 16 steps, half-res
	High = 2,    // 32 steps, full-res
	Ultra = 3,   // 64 steps, full-res
	Custom = 4,  // hand-tuned steps/divisor
};

inline const char* VolumetricQualityName(VolumetricQuality q) {
	switch (q) {
	case VolumetricQuality::Low: return "Low (8 steps)";
	case VolumetricQuality::Medium: return "Medium (16 steps)";
	case VolumetricQuality::High: return "High (32 steps)";
	case VolumetricQuality::Ultra: return "Ultra (64 steps)";
	case VolumetricQuality::Custom: return "Custom";
	}
	return "?";
}

struct VolumetricDesc {
	Format format = Format::RGBA16_Float; // must match the HDR target
	uint32_t steps = 16;                  // march steps, 4..128
	uint32_t resolutionDivisor = 1;       // MUST stay 1 (full-res composite;
	                                      // see the quality-model note above)
	float maxDistance = 80.0f;            // ray clamp (m)
	float density = 0.015f;               // extinction coefficient (1/m)
	float intensity = 1.0f;               // scattering multiplier
	float anisotropy = 0.4f;              // HG g, -0.9..0.9 (forward+)
	float sunScale = 1.0f;                // sun single-scatter strength
	float giScale = 1.0f;                 // probe ambient strength
	uint32_t useGi = 1;                   // 1 = sample SG-GI probes
	uint32_t useSun = 1;                  // 1 = analytic sun scatter
	uint32_t useCvolGi = 1;                // 1 = convex volumes stack into the
	                                       // fog GI (0 = octree/sky only ΓÇö the
	                                       // fog-GI quality bypass; skips the
	                                       // per-refresh volume walk entirely)
	float jitterStrength = 1.0f;          // 0 = off, 1 = full IGN dither
	float heightFalloff = 0.0f;           // extra height density falloff (1/m, 0=off)
	float heightBase = 0.0f;              // reference height (m)
	const char* name = "volumetric";
	const char* viewName = "volumetricView";
	const char* logicalName = "volumetricLogical";
};

// Preset -> steps. The divisor stays 1 (full-res; see above).
inline void ApplyVolumetricQuality(VolumetricDesc& d, VolumetricQuality q) {
	switch (q) {
	case VolumetricQuality::Low:
		d.steps = 8;
		break;
	case VolumetricQuality::Medium:
		d.steps = 16;
		break;
	case VolumetricQuality::High:
		d.steps = 32;
		break;
	case VolumetricQuality::Ultra:
		d.steps = 64;
		break;
	case VolumetricQuality::Custom:
		break;
	}
	d.resolutionDivisor = 1;
}

// Must match volumetric.frag.hlsl VolumetricPush (128 bytes).
struct VolumetricPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t steps = 16;
	float texelX = 0.0f;        // output texel (1/outputExtent)
	float texelY = 0.0f;
	float depthTexelX = 0.0f;   // full-res depth texel (1/depthExtent)
	float depthTexelY = 0.0f;
	float maxDistance = 80.0f;
	float density = 0.015f;
	float intensity = 1.0f;
	float anisotropy = 0.4f;
	float sunScale = 1.0f;
	float giScale = 1.0f;
	uint32_t useGi = 1;
	uint32_t useSun = 1;
	uint32_t useCvolGi = 1; // 1 = convex volumes stack into the fog GI
	float jitterStrength = 1.0f;
	float heightFalloff = 0.0f;
	float heightBase = 0.0f;
	float time = 0.0f;
	uint32_t pad1 = 0; // 8-align filler for the BDA tail below
	// Forward+ light feed for fog in-scatter (the forward tiled lights
	// contribute to the march, not just the surfaces): tile-list BDAs
	// ride the same freshness contract as the cvol tile masks (address
	// + grid publish together under TileCullWillRun, zeros together —
	// the shader falls back to a direct [0, fpLightCount) walk when the
	// cull didn't run). fpLights/fpLightCount/fpMaxPerTile publish
	// whenever the light buffer is uploaded (independent of the cull).
	uint64_t fpTileCounts = 0;   // per-tile light index lists (0 = stale)
	uint64_t fpTileIndices = 0;  // (0 = stale)
	uint64_t fpLights = 0;       // ForwardPlusLight buffer (this flight)
	uint32_t fpLightCount = 0;   // lights-only bound (probes sort after)
	uint32_t fpMaxPerTile = 128; // list stride - probe slots (matches pc)
};
static_assert(sizeof(VolumetricPush) == 128);

// Default SPVs (fullscreen-triangle VS shared with tonemap + volumetric PS).
inline constexpr const char* kVolumetricVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kVolumetricPsPath = "content/shaders/volumetric.frag.hlsl.spv";

struct VolumetricFog {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 };       // output extent (== renderExtent, divisor is 1)
	Extent2D renderExtent{ 0, 0 }; // full-res reference (drift detection)
	Format format = Format::RGBA16_Float;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};
	VolumetricDesc desc{};

	[[nodiscard]] bool TargetValid() const { return !image.empty() && !logicalView.empty(); }
	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetValid() && ShadersValid(); }

	[[nodiscard]] LogicalImageViewHandle OutputView() const { return logicalView; }
	[[nodiscard]] uint32_t OutputBindless() const { return bindlessIndex; }

	static Extent2D OutputExtentFor(Extent2D render, uint32_t divisor) {
		divisor = std::clamp(divisor, 1u, 4u);
		return {
			std::max(1u, render.width / divisor),
			std::max(1u, render.height / divisor),
		};
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kVolumetricVsPath,
		const char* psPath = kVolumetricPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kVolumetricVsPath;
			const std::string ps = psPath ? psPath : kVolumetricPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "volumetricVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "volumetricFrag");
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
		program = ShaderProgram{};
		vsModule = {};
		fragModule = {};
	}

	bool CreateTarget(ResourceManager& ctx, RenderGraph& rg, Extent2D renderExtentIn, const VolumetricDesc& d) {
		if (renderExtentIn.width == 0 || renderExtentIn.height == 0)
			return false;
		DestroyTarget(rg);

		const Extent2D out = OutputExtentFor(renderExtentIn, d.resolutionDivisor);
		ImageCreateInfo ci{};
		ci.extent = { out.width, out.height, 1 };
		ci.format = d.format;
		// TransferSrc: debug stage dumps (VULKANTOY_SSR_DUMP) — armed dumps
		// only (DownloadDataFromImage needs it on the source image).
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
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

		image = std::move(newImage);
		view = std::move(newView);
		bindless = std::move(newBindless);
		bindlessIndex = newBindlessIndex;
		logicalView = newLogical;
		extent = out;
		renderExtent = renderExtentIn;
		format = d.format;
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
		renderExtent = { 0, 0 };
	}

	// Idempotent output + program creation. Live-updates desc (lighting
	// knobs need no realloc; steps/divisor realloc only on drift).
	// Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D renderExtentIn,
		const VolumetricDesc& d,
		const char* vsPath = kVolumetricVsPath,
		const char* psPath = kVolumetricPsPath) {
		desc = d;
		const Extent2D want = OutputExtentFor(renderExtentIn, d.resolutionDivisor);
		bool ok = true;
		if (!(TargetValid() &&
				extent.width == want.width && extent.height == want.height &&
				renderExtent.width == renderExtentIn.width &&
				renderExtent.height == renderExtentIn.height &&
				format == d.format)) {
			ok = CreateTarget(ctx, rg, renderExtentIn, d);
		}
		ok = EnsureShaders(ctx, vsPath, psPath) && ok;
		return ok;
	}

	void Destroy(RenderGraph& rg) {
		DestroyTarget(rg);
		DestroyShaders();
	}

	// Emits the HDR -> HDR volumetric composite. Captures only copyable
	// handles/indices, so the pass is safe once this call returns.
	// depthExtent = full-res depth size (for .Load mapping when divisor > 1).
	RenderPass& AddVolumetricPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		Extent2D depthExtent,
		float frameTime,
		const VolumetricDesc& overrideDesc,
		const char* name = nullptr,
		// Forward+ light feed (fog in-scatter): tile-list addresses ride
		// the cull freshness contract (0 = stale -> direct walk), the
		// light buffer + count publish whenever uploaded.
		uint64_t fpTileCountsAddr = 0,
		uint64_t fpTileIndicesAddr = 0,
		uint64_t fpLightsAddr = 0,
		uint32_t fpLightCountIn = 0,
		uint32_t fpMaxPerTileIn = 128) const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);
		assert(extent.width != 0 && extent.height != 0);

		VolumetricPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.hdrTextureIndex = hdrBindlessIndex;
		push.depthTextureIndex = depthBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.steps = std::clamp(overrideDesc.steps, 4u, 128u);
		push.texelX = 1.0f / static_cast<float>(extent.width);
		push.texelY = 1.0f / static_cast<float>(extent.height);
		push.depthTexelX = 1.0f / static_cast<float>(std::max(1u, depthExtent.width));
		push.depthTexelY = 1.0f / static_cast<float>(std::max(1u, depthExtent.height));
		push.maxDistance = (std::max)(1.0f, overrideDesc.maxDistance);
		push.density = (std::max)(0.0f, overrideDesc.density);
		push.intensity = (std::max)(0.0f, overrideDesc.intensity);
		push.anisotropy = std::clamp(overrideDesc.anisotropy, -0.9f, 0.9f);
		push.sunScale = (std::max)(0.0f, overrideDesc.sunScale);
		push.giScale = (std::max)(0.0f, overrideDesc.giScale);
		push.useGi = overrideDesc.useGi ? 1u : 0u;
		push.useSun = overrideDesc.useSun ? 1u : 0u;
		push.useCvolGi = overrideDesc.useCvolGi ? 1u : 0u;
		push.fpTileCounts = fpTileCountsAddr;
		push.fpTileIndices = fpTileIndicesAddr;
		push.fpLights = fpLightsAddr;
		push.fpLightCount = fpLightCountIn;
		push.fpMaxPerTile = fpMaxPerTileIn;
		push.jitterStrength = std::clamp(overrideDesc.jitterStrength, 0.0f, 1.0f);
		push.heightFalloff = (std::max)(0.0f, overrideDesc.heightFalloff);
		push.heightBase = overrideDesc.heightBase;
		push.time = frameTime;
		push.pad1 = 0;

		RenderPass& pass = rg.AddPass(name ? name : "Volumetric");
		pass.SetOffset(0, 0)
			.SetExtent(extent)
			.SetInputImages({ hdrView, depthView })
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
			cmd.PushConstants(0, sizeof(VolumetricPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
		return pass;
	}

	// Uses the stored desc from Ensure().
	RenderPass& AddVolumetricPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		Extent2D depthExtent,
		float frameTime,
		const char* name = nullptr) const {
		return AddVolumetricPass(rg, hdrView, hdrBindlessIndex, depthView, depthBindlessIndex,
			frameUniformsAddr, linearSamplerIndex, depthExtent, frameTime, desc, name);
	}
};

} // namespace smvk
