#pragma once
// HDR -> LDR tonemapper (fullscreen triangle, ACES / Reinhard / Uncharted2).
//
// Ports the pattern used by BloomChain / CascadedShadowAtlas into one place:
// default shader program ownership + push layout + rendergraph pass setup.
// Sampling is bindless (textures[] indexed by push), barriers come from
// SetColor (write) + SetInputImages (read).
//
// Composite: hdr += bloom * intensity (pre-tonemap), then Tonemap(hdr * exposure)
// then gamma 1/2.2. Bloom disabled when bloomTextureIndex == UINT32_MAX.
//
// Usage (app/src/main.cpp):
//   smvk::Tonemapper tonemap;
//   tonemap.Ensure(ctx, desc);                        // default programs
//   tonemap.AddTonemapPass(rg, hdrView, hdrBindless,  // HDR + optional bloom
//                          bloomView, bloomBindless, bloomIntensity,
//                          linearSampler, swapViews, swapExtent);

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

// Must match tonemap.frag.hlsl TonemapPush (32 bytes).
struct TonemapPush {
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	float exposure = 0.02f;
	uint32_t tonemapMode = 1; // 0=Reinhard, 1=ACES, 2=Uncharted2
	uint32_t bloomTextureIndex = UINT32_MAX; // UINT32_MAX = disabled
	float bloomIntensity = 0.0f;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(TonemapPush) == 32);

inline constexpr uint32_t kTonemapReinhard = 0;
inline constexpr uint32_t kTonemapACES = 1;
inline constexpr uint32_t kTonemapUncharted2 = 2;

struct TonemapDesc {
	float exposure = 0.02f;
	uint32_t tonemapMode = kTonemapACES;
	const char* name = "Tonemap";
};

// Default SPVs (fullscreen-triangle VS + tonemap PS).
inline constexpr const char* kTonemapVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kTonemapPsPath = "content/shaders/tonemap.frag.hlsl.spv";

struct Tonemapper {
	// Default program (tonemap.vert mainVS + tonemap.frag mainPS). Created by
	// EnsureShaders/Ensure so apps don't own tonemap modules.
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};
	TonemapDesc desc{};

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}

	// Idempotent default program creation. Returns true when ready afterwards.
	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kTonemapVsPath,
		const char* psPath = kTonemapPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kTonemapVsPath;
			const std::string ps = psPath ? psPath : kTonemapPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "tonemapVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "tonemapFrag");
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

	// No viewport resources – just stores desc + internalizes EnsureShaders.
	bool Ensure(ResourceManager& ctx, const TonemapDesc& d,
		const char* vsPath = kTonemapVsPath,
		const char* psPath = kTonemapPsPath) {
		desc = d;
		return EnsureShaders(ctx, vsPath, psPath);
	}

	// Emits the HDR -> swapchain pass with recording. Captures only copyable
	// handles/indices, so the pass is safe once this call returns.
	// bloomView may be empty (or bloomBindless UINT32_MAX) to disable bloom.
	RenderPass& AddTonemapPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle bloomView,
		uint32_t bloomBindlessIndex,
		float bloomIntensity,
		uint32_t linearSamplerIndex,
		const std::vector<LogicalImageViewHandle>& swapViews,
		Extent2D swapExtent,
		const TonemapDesc& overrideDesc,
		const char* name = nullptr) const {
		assert(ShadersValid());
		assert(!hdrView.empty());
		assert(!swapViews.empty());

		const bool bloomOn = !bloomView.empty() && bloomBindlessIndex != UINT32_MAX && bloomIntensity > 0.0f;
		TonemapPush push{};
		push.hdrTextureIndex = hdrBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.exposure = overrideDesc.exposure;
		push.tonemapMode = overrideDesc.tonemapMode;
		push.bloomTextureIndex = bloomOn ? bloomBindlessIndex : UINT32_MAX;
		push.bloomIntensity = bloomOn ? bloomIntensity : 0.0f;
		push.pad0 = 0;
		push.pad1 = 0;

		std::vector<LogicalImageViewHandle> inputs;
		inputs.reserve(bloomOn ? 2 : 1);
		inputs.push_back(hdrView);
		if (bloomOn)
			inputs.push_back(bloomView);

		RenderPass& pass = rg.AddPass(name ? name : overrideDesc.name);
		pass.SetOffset(0, 0)
			.SetExtent(swapExtent)
			.SetInputImages(inputs)
			.SetColor(swapViews,
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::Store,
				ClearColor(0, 0, 0, 1));
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
			cmd.PushConstants(0, sizeof(TonemapPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
		return pass;
	}

	// Uses the stored desc from Ensure().
	RenderPass& AddTonemapPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle bloomView,
		uint32_t bloomBindlessIndex,
		float bloomIntensity,
		uint32_t linearSamplerIndex,
		const std::vector<LogicalImageViewHandle>& swapViews,
		Extent2D swapExtent,
		const char* name = nullptr) const {
		return AddTonemapPass(rg, hdrView, hdrBindlessIndex, bloomView, bloomBindlessIndex,
			bloomIntensity, linearSamplerIndex, swapViews, swapExtent, desc, name);
	}
};

} // namespace smvk
