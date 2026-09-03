#pragma once

#include <iostream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <cstdint>

#include <Engine/core.h>
#include <Engine/Renderer/dxc_integration.h>
#include <imgui.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// -----------------------------------------------------------------------------
// 02_blur – fullscreen image blur with ImGui-controllable blur steps + downres.
//
// Pipeline (all fullscreen-triangle, bindless sampled):
//   source (shoes.png, RGBA8) --downsample--> blurA (reduced res)
//                             --H/V separable gaussian ping-pong--> blurA/blurB
//                             --upscale copy--> swapchain
//
// ImGui controls:
//   * Enable Blur (bypass switch)
//   * Downsample  (1x / 2x / 4x / 8x divisor for the blur targets)
//   * Blur Steps  (0..12 directional H/V passes, ping-pong A<->B)
//   * Blur Radius (texel spread of the 9-tap gaussian)
//   * Show Original (A/B compare against the unblurred source)
// -----------------------------------------------------------------------------

static constexpr const char* kBlurVertShader = R"(
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut mainVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = uv;
    return o;
}
)";

// Self-contained: declares its own bindless tables (no #include – runtime DXC
// compiles from string, so no include paths are available).
static constexpr const char* kBlurFragShader = R"(
[[vk::binding(0, 0)]]
Texture2D textures[];
[[vk::binding(1, 0)]]
SamplerState samplers[];

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match C++ BlurPush (32 bytes).
struct BlurPush
{
    uint srcTextureIndex;
    uint samplerIndex;
    float2 texelSize;
    float radius;
    uint direction; // 0 = horizontal, 1 = vertical
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<BlurPush> bp;

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    Texture2D src = textures[NonUniformResourceIndex(bp.srcTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];

    // 9-tap separable gaussian (LearnOpenGL weights, sum ~= 1).
    // radius scales the tap offsets; radius 0 collapses to a plain copy
    // (all taps hit the center texel), which is how downsample/composite
    // reuse this same shader.
    float2 dir = (bp.direction == 1) ? float2(0.0f, 1.0f) : float2(1.0f, 0.0f);
    float2 off = dir * bp.texelSize * bp.radius;

    float3 c0 = src.Sample(smp, IN.uv).rgb;
    float3 c1a = src.Sample(smp, IN.uv + off * 1.0f).rgb;
    float3 c1b = src.Sample(smp, IN.uv - off * 1.0f).rgb;
    float3 c2a = src.Sample(smp, IN.uv + off * 2.0f).rgb;
    float3 c2b = src.Sample(smp, IN.uv - off * 2.0f).rgb;
    float3 c3a = src.Sample(smp, IN.uv + off * 3.0f).rgb;
    float3 c3b = src.Sample(smp, IN.uv - off * 3.0f).rgb;
    float3 c4a = src.Sample(smp, IN.uv + off * 4.0f).rgb;
    float3 c4b = src.Sample(smp, IN.uv - off * 4.0f).rgb;

    float3 c = c0 * 0.2270270270f
             + (c1a + c1b) * 0.1945945946f
             + (c2a + c2b) * 0.1216216216f
             + (c3a + c3b) * 0.0540540541f
             + (c4a + c4b) * 0.0162162162f;

    OUT.outColor = float4(c, 1.0f);
    return OUT;
}
)";

// Must match kBlurFragShader BlurPush (32 bytes).
struct BlurPush {
	uint32_t srcTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	float texelX = 0.0f;
	float texelY = 0.0f;
	float radius = 0.0f;
	uint32_t direction = 0; // 0 = H, 1 = V
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(BlurPush) == 32);

struct SourceImage {
	smvk::Holder<smvk::ImageHandle> image;
	smvk::Holder<smvk::ImageViewHandle> view;
	smvk::Holder<smvk::BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	smvk::LogicalImageViewHandle logical{};
	int width = 0;
	int height = 0;
	bool valid = false;
};

struct BlurTarget {
	smvk::Holder<smvk::ImageHandle> image;
	smvk::Holder<smvk::ImageViewHandle> view;
	smvk::Holder<smvk::BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	smvk::LogicalImageViewHandle logical{};
	smvk::Extent2D extent{ 0, 0 };
	bool valid = false;
};

static std::filesystem::path FindShoesPng() {
	// POST_BUILD copies shoes.png next to the exe, but also support running
	// from the repo root / sample dir for robustness.
	static const char* kCandidates[] = {
		"shoes.png",
		"samples/02_blur/shoes.png",
		"../samples/02_blur/shoes.png",
		"../../samples/02_blur/shoes.png",
	};
	for (auto c : kCandidates) {
		std::filesystem::path p(c);
		std::error_code ec;
		if (std::filesystem::exists(p, ec))
			return p;
	}
	return {};
}

static bool LoadRgba8(const std::filesystem::path& path, std::vector<uint8_t>& out, int& w, int& h) {
	int comp = 0;
	stbi_uc* px = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
	if (!px) {
		std::cerr << "[02_blur] stbi_load failed for " << path << ": " << stbi_failure_reason() << "\n";
		return false;
	}
	out.assign(px, px + size_t(w) * size_t(h) * 4);
	stbi_image_free(px);
	return true;
}

static void MakeProceduralChecker(std::vector<uint8_t>& out, int w, int h) {
	out.resize(size_t(w) * size_t(h) * 4);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const int cx = x / 32, cy = y / 32;
			const bool check = ((cx + cy) & 1) == 0;
			// Sharp edges (checker) + smooth gradient -> blur is easy to see.
			uint8_t r = check ? 230 : 20;
			uint8_t g = uint8_t((float(x) / float(w)) * 255.0f);
			uint8_t b = uint8_t((float(y) / float(h)) * 255.0f);
			size_t i = (size_t(y) * size_t(w) + size_t(x)) * 4;
			out[i + 0] = r;
			out[i + 1] = g;
			out[i + 2] = b;
			out[i + 3] = 255;
		}
	}
}

class BlurApp : public smvk::AppImpl {
	using Super = smvk::AppImpl;

public:
	BlurApp() = default;

private:
	bool Initialize() override {
		if (!Super::Initialize())
			return false;

		// Fullscreen blur pipeline (one VS + one separable-blur/copy PS).
		auto vertCode = CompileHLSLToSPIRV(kBlurVertShader, DxcShaderStage::Vertex, L"mainVS");
		auto fragCode = CompileHLSLToSPIRV(kBlurFragShader, DxcShaderStage::Fragment, L"mainPS");
		m_vert = m_context->CreateShaderModule(vertCode, "blurVert");
		m_frag = m_context->CreateShaderModule(fragCode, "blurFrag");
		smvk::Shader vs{ .module = m_vert, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
		smvk::Shader ps{ .module = m_frag, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
		m_program = { .shaders = { vs, ps }, .shaderCount = 2 };

		// Decode on CPU first so the window can be sized to the image.
		// Nothing has been submitted yet, so the GPU is idle here.
		std::vector<uint8_t> rgba;
		int w = 0, h = 0;
		if (!DecodeSource(rgba, w, h)) {
			std::cerr << "[02_blur] Failed to decode source image\n";
			return false;
		}
		SizeWindowToImage(w, h);
		m_swapchain->Recreate(); // picks up the new window size (idles internally)
		m_swapchain->ClearFramebufferResized();

		for (uint32_t i = 0; i < m_swapchain->GetImageCount(); ++i) {
			m_swapViews.push_back(m_rendergraph->AddExternalImageView(
				m_swapchain->GetImageViewHandle(i),
				m_swapchain->GetSurfaceFormat(),
				"swap"));
		}

		if (!UploadSource(rgba, w, h)) {
			std::cerr << "[02_blur] Failed to create source image\n";
			return false;
		}

		EnsureBlurTargets();

		m_imgui = std::make_unique<smvk::ImGuiOverlay>();
		if (!m_imgui->Init(*m_context, *m_swapchain, window)) {
			std::cerr << "[02_blur] ImGui init failed – continuing without UI\n";
			m_imgui.reset();
		}
		// Image viewer: always interact with the UI.
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

		return true;
	}

	void Shutdown() override {
		WaitForDeviceIdle();
		if (m_imgui) {
			m_imgui->Shutdown();
			m_imgui.reset();
		}
		FreeBlurTarget(m_blurA);
		FreeBlurTarget(m_blurB);
		if (!m_source.logical.empty()) {
			m_rendergraph->FreeImageView(m_source.logical);
			m_source.logical = {};
		}
		for (auto h : m_swapViews)
			m_rendergraph->FreeImageView(h);
		m_swapViews.clear();
		m_source = {};
		Super::Shutdown();
	}

	bool DecodeSource(std::vector<uint8_t>& rgba, int& w, int& h) {
		auto png = FindShoesPng();
		if (!png.empty() && LoadRgba8(png, rgba, w, h)) {
			std::cout << "[02_blur] Loaded " << png << " (" << w << "x" << h << ")\n";
			return true;
		}
		std::cout << "[02_blur] shoes.png not found – using procedural checker\n";
		w = 512; h = 512;
		MakeProceduralChecker(rgba, w, h);
		return true;
	}

	// Resize the OS window so the framebuffer matches the image 1:1
	// (clamped to the monitor work area so huge images stay on screen).
	void SizeWindowToImage(int w, int h) {
		int maxW = w, maxH = h;
		if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
			int x = 0, y = 0, mw = 0, mh = 0;
			glfwGetMonitorWorkarea(mon, &x, &y, &mw, &mh);
			if (mw > 0 && mh > 0) {
				maxW = std::min(w, std::max(64, mw - 80));
				maxH = std::min(h, std::max(64, mh - 120));
			}
		}
		maxW = std::max(64, maxW);
		maxH = std::max(64, maxH);
		glfwSetWindowSize(window, maxW, maxH);
		// Keep aspect when clamped: letterbox via composite stretch is
		// acceptable for this sample; 1:1 when it fits (the common case).
	}

	bool UploadSource(const std::vector<uint8_t>& rgba, int w, int h) {
		smvk::ImageCreateInfo ci{};
		ci.type = smvk::ImageType::Image2D;
		ci.extent = { uint32_t(w), uint32_t(h), 1 };
		ci.format = smvk::Format::RGBA8_UNorm;
		ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "blurSource";

		m_source.image = m_context->CreateImage(ci, rgba.data());
		if (m_source.image.empty())
			return false;

		smvk::ImageViewCreateInfo vci{};
		vci.viewType = smvk::ImageViewType::View2D;
		vci.format = smvk::Format::Undefined; // inherit
		vci.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = "blurSourceView";
		m_source.view = m_context->CreateImageView(m_source.image, vci);
		m_source.bindless = m_context->CreateBindlessImage(m_source.view, smvk::ImageLayout::ShaderReadOnlyOptimal);
		m_source.bindlessIndex = m_context->GetBindlessIndex(m_source.bindless);
		m_source.logical = m_rendergraph->AddExternalImageView(m_source.view, smvk::Format::RGBA8_UNorm, "blurSourceLogical");
		m_source.width = w;
		m_source.height = h;
		m_source.valid = true;
		m_context->FlushUploads();
		return true;
	}

	uint32_t DownsampleDivisor() const {
		// UI index 0..3 -> 1x / 2x / 4x / 8x.
		switch (m_downsampleIndex) {
		case 0: return 1;
		case 1: return 2;
		case 2: return 4;
		default: return 8;
		}
	}

	void FreeBlurTarget(BlurTarget& t) {
		if (!t.logical.empty()) {
			m_rendergraph->FreeImageView(t.logical);
			t.logical = {};
		}
		t.bindlessIndex = UINT32_MAX;
		t.bindless = {};
		t.view = {};
		t.image = {};
		t.extent = { 0, 0 };
		t.valid = false;
	}

	bool CreateBlurTarget(BlurTarget& t, smvk::Extent2D extent, const char* name) {
		FreeBlurTarget(t);
		smvk::ImageCreateInfo ci{};
		ci.type = smvk::ImageType::Image2D;
		ci.extent = { extent.width, extent.height, 1 };
		ci.format = smvk::Format::RGBA8_UNorm;
		ci.usage = smvk::ImageUsage::ColorAttachment | smvk::ImageUsage::Sampled;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = name;
		t.image = m_context->CreateImage(ci, nullptr);
		if (t.image.empty())
			return false;
		smvk::ImageViewCreateInfo vci{};
		vci.viewType = smvk::ImageViewType::View2D;
		vci.format = smvk::Format::Undefined;
		vci.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = name;
		t.view = m_context->CreateImageView(t.image, vci);
		t.bindless = m_context->CreateBindlessImage(t.view, smvk::ImageLayout::ShaderReadOnlyOptimal);
		t.bindlessIndex = m_context->GetBindlessIndex(t.bindless);
		t.logical = m_rendergraph->AddExternalImageView(t.view, smvk::Format::RGBA8_UNorm, name);
		t.extent = extent;
		t.valid = true;
		return true;
	}

	void EnsureBlurTargets() {
		auto se = m_swapchain->GetExtent();
		const uint32_t div = DownsampleDivisor();
		smvk::Extent2D want{ std::max(1u, se.width / div), std::max(1u, se.height / div) };
		if (m_blurA.valid && m_blurB.valid &&
			m_blurA.extent.width == want.width && m_blurA.extent.height == want.height)
			return;
		// Safe without idling: Holder destroy retires the old VkImage/
		// VkImageView on the graphics timeline (engine DeferDestroy), so the
		// GPU can still be reading them. Retirement drains in
		// ProcessDeferrredTasks each frame; pool slots stay reserved until
		// then, so there is no aliasing either.
		if (!CreateBlurTarget(m_blurA, want, "blurA") || !CreateBlurTarget(m_blurB, want, "blurB"))
			std::cerr << "[02_blur] Failed to create blur targets\n";
	}

	void RebuildSwapViews() {
		for (auto h : m_swapViews)
			m_rendergraph->FreeImageView(h);
		m_swapViews.clear();
		for (uint32_t i = 0; i < m_swapchain->GetImageCount(); ++i) {
			m_swapViews.push_back(m_rendergraph->AddExternalImageView(
				m_swapchain->GetImageViewHandle(i),
				m_swapchain->GetSurfaceFormat(),
				"swap"));
		}
		EnsureBlurTargets();
	}

	void DrawFrame() override {
		if (m_swapchain->FramebufferResized()) {
			m_swapchain->Recreate();
			RebuildSwapViews();
			m_swapchain->ClearFramebufferResized();
		}
		auto fb = m_swapchain->AcquireFramebuffer();
		UpdateDeltaTime();
		RenderFrame(fb);
		m_swapchain->EndAndPresentFramebuffer(fb);
	}

	void RenderFrame(smvk::Framebuffer fb) {
		m_rendergraph->NewFrame();
		EnsureBlurTargets();
		m_context->UpdateDescriptors();

		const auto swapExtent = fb.swapchain->GetExtent();
		const smvk::Extent2D swapEx{ swapExtent.width, swapExtent.height };
		auto swapView = m_swapViews[fb.imageIndex];

		uint32_t linearSampler = m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{
			.magFilter = smvk::Filter::Linear,
			.minFilter = smvk::Filter::Linear,
			.mipmapMode = smvk::MipmapMode::Linear,
			.addressU = smvk::AddressMode::ClampToEdge,
			.addressV = smvk::AddressMode::ClampToEdge,
			.addressW = smvk::AddressMode::ClampToEdge,
			.maxAnisotropy = 1.0f });

		const bool doBlur = m_enableBlur && !m_showOriginal && m_blurSteps > 0 && m_blurA.valid && m_blurB.valid;
		const float srcTexelX = 1.0f / float(std::max(1, m_source.width));
		const float srcTexelY = 1.0f / float(std::max(1, m_source.height));

		// Tracks the latest blurred result for the composite pass.
		smvk::LogicalImageViewHandle finalView = m_source.logical;
		uint32_t finalBindless = m_source.bindlessIndex;
		float finalTexelX = srcTexelX, finalTexelY = srcTexelY;

		if (doBlur) {
			const smvk::Extent2D be = m_blurA.extent;
			const float blurTexelX = 1.0f / float(std::max(1u, be.width));
			const float blurTexelY = 1.0f / float(std::max(1u, be.height));

			// Pass 0: downsample source -> blurA (plain copy, radius 0).
			{
				BlurPush push{};
				push.srcTextureIndex = m_source.bindlessIndex;
				push.samplerIndex = linearSampler;
				push.texelX = srcTexelX;
				push.texelY = srcTexelY;
				push.radius = 0.0f;
				push.direction = 0;
				m_rendergraph->AddPass("BlurDownsample")
					.SetOffset(0, 0)
					.SetExtent(be)
					.SetInputImages({ m_source.logical })
					.SetColor({ m_blurA.logical }, smvk::AttachmentLoadOp::DontCare, smvk::AttachmentStoreOp::Store)
					.SetRecordingFunc([this, push](smvk::CommandBuffer& cmd) mutable {
						cmd.BindRenderPipeline(m_program, smvk::VertexFormat::Empty,
							smvk::DepthSettings::Disabled(), smvk::StencilSettings::DefaultDisabled(),
							smvk::BlendSettings::Opaque(), smvk::MSAASettings::Count1(),
							smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
						cmd.PushConstants(0, sizeof(BlurPush), &push, smvk::ShaderStage::All);
						cmd.Draw(3, 1, 0, 0);
					});
			}

			// Passes 1..N: separable H/V ping-pong between A and B.
			const int steps = std::clamp(m_blurSteps, 0, 16);
			smvk::LogicalImageViewHandle curView = m_blurA.logical;
			uint32_t curBindless = m_blurA.bindlessIndex;
			for (int i = 0; i < steps; ++i) {
				const bool toB = (i % 2 == 0);
				BlurTarget& dst = toB ? m_blurB : m_blurA;
				BlurPush push{};
				push.srcTextureIndex = curBindless;
				push.samplerIndex = linearSampler;
				push.texelX = blurTexelX;
				push.texelY = blurTexelY;
				push.radius = m_blurRadius;
				push.direction = uint32_t(i % 2); // alternate H, V, H, V...
				m_rendergraph->AddPass(i % 2 == 0 ? "BlurH" : "BlurV")
					.SetOffset(0, 0)
					.SetExtent(be)
					.SetInputImages({ curView })
					.SetColor({ dst.logical }, smvk::AttachmentLoadOp::DontCare, smvk::AttachmentStoreOp::Store)
					.SetRecordingFunc([this, push](smvk::CommandBuffer& cmd) mutable {
						cmd.BindRenderPipeline(m_program, smvk::VertexFormat::Empty,
							smvk::DepthSettings::Disabled(), smvk::StencilSettings::DefaultDisabled(),
							smvk::BlendSettings::Opaque(), smvk::MSAASettings::Count1(),
							smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
						cmd.PushConstants(0, sizeof(BlurPush), &push, smvk::ShaderStage::All);
						cmd.Draw(3, 1, 0, 0);
					});
				curView = dst.logical;
				curBindless = dst.bindlessIndex;
			}
			finalView = curView;
			finalBindless = curBindless;
			finalTexelX = blurTexelX;
			finalTexelY = blurTexelY;
		}

		// Composite: blurred (or original) -> swapchain (upscale copy).
		{
			BlurPush push{};
			push.srcTextureIndex = finalBindless;
			push.samplerIndex = linearSampler;
			push.texelX = finalTexelX;
			push.texelY = finalTexelY;
			push.radius = 0.0f;
			push.direction = 0;
			m_rendergraph->AddPass("BlurComposite")
				.SetOffset(0, 0)
				.SetExtent(swapEx)
				.SetInputImages({ finalView })
				.SetColor({ swapView }, smvk::AttachmentLoadOp::DontCare, smvk::AttachmentStoreOp::Store)
				.SetRecordingFunc([this, push](smvk::CommandBuffer& cmd) mutable {
					cmd.BindRenderPipeline(m_program, smvk::VertexFormat::Empty,
						smvk::DepthSettings::Disabled(), smvk::StencilSettings::DefaultDisabled(),
						smvk::BlendSettings::Opaque(), smvk::MSAASettings::Count1(),
						smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
					cmd.PushConstants(0, sizeof(BlurPush), &push, smvk::ShaderStage::All);
					cmd.Draw(3, 1, 0, 0);
				});
		}

		// ImGui controls – built before Execute so AddPass sees the draw data.
		if (m_imgui && m_imgui->IsInitialized()) {
			m_imgui->NewFrame();

			ImGui::Begin("02_blur — Controls");
			ImGui::Text("Frame %.1f FPS (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
			ImGui::Text("Source %dx%d  Swap %ux%u", m_source.width, m_source.height, swapEx.width, swapEx.height);
			if (m_blurA.valid)
				ImGui::Text("Blur target %ux%u (/%u)", m_blurA.extent.width, m_blurA.extent.height, DownsampleDivisor());
			ImGui::Separator();
			ImGui::Checkbox("Enable Blur", &m_enableBlur);
			ImGui::Checkbox("Show Original (bypass)", &m_showOriginal);
			const char* downItems[] = { "1x (full)", "2x (half)", "4x (quarter)", "8x" };
			ImGui::SliderInt("Downsample", &m_downsampleIndex, 0, 3, downItems[m_downsampleIndex]);
			ImGui::SliderInt("Blur Steps (H/V passes)", &m_blurSteps, 0, 16);
			ImGui::SliderFloat("Blur Radius (texels)", &m_blurRadius, 0.0f, 5.0f, "%.2f");
			ImGui::Separator();
			ImGui::TextDisabled("Steps alternate H,V,H,V... ping-pong A<->B.");
			ImGui::TextDisabled("Downsample recreates targets; radius 0 = copy.");
			ImGui::TextDisabled("[ESC] quit");
			ImGui::End();

			m_imgui->AddPass(*m_rendergraph, swapView, swapEx);
		}

		m_rendergraph->Execute(fb);
		m_context->ProcessDeferrredTasks();
	}

private:
	std::unique_ptr<smvk::ImGuiOverlay> m_imgui{};
	std::vector<smvk::LogicalImageViewHandle> m_swapViews;
	smvk::Holder<smvk::ShaderModuleHandle> m_vert;
	smvk::Holder<smvk::ShaderModuleHandle> m_frag;
	smvk::ShaderProgram m_program{};

	SourceImage m_source{};
	BlurTarget m_blurA{};
	BlurTarget m_blurB{};

	// ImGui-controllable state.
	bool m_enableBlur = true;
	bool m_showOriginal = false;
	int m_downsampleIndex = 1; // 2x half-res by default
	int m_blurSteps = 4;       // H,V,H,V
	float m_blurRadius = 2.0f;
};

int main(int argc, const char* argv[]) {
	if (argc > 1) {
		std::cout << argv[0];
	}

	BlurApp app = BlurApp();
	try {
		app.Run();
	}
	catch (const std::exception& e) {
		std::cerr << "\nERROR: " << e.what() << std::endl;
		std::cerr << "\nPress Enter to exit...";
		std::cin.get();
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
