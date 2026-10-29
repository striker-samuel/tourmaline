// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// RhiUploadBarrierGpuTests: the DEVICE-level regression for the
// split-writer upload hazard (todo.txt's stranded-upload-write class,
// retired from production placement by PR1 and reproduced here
// INTENTIONALLY, independent of the placement renderer - the PR2 spec):
//
//   * a BufferType_Indirect command buffer whose STATIC halves
//     (indexCount/firstIndex/vertexOffset) come from UploadDataToBuffer
//     in the SAME FRAME a compute pass rewrites the DYNAMIC halves
//     (instanceCount/firstInstance) - the exact two-writer shape,
//   * an immediate DrawIndexedIndirect that consumes BOTH halves, into
//     a small offscreen color target,
//   * readback of the command AND the pixels.
//
// The upload declares its FIRST consumer (the compute rewrite:
// Compute / ShaderRead|ShaderWrite) - the placement seed's exact hint
// shape. The draw's hint uses the production pairing (IndirectCommandRead
// @ PipelineStageBit_DrawIndirect - gfx_filter's AddConsumeHints rule;
// the engine's AllGraphics maps to the shader-stage union ONLY and never
// covers the fixed-function command fetch). With correct hints the test
// asks the ENGINE question PR2 exists for: does the frame's
// synchronization (the draw's COMPUTE-src barrier + the frame submit's
// transfer-timeline wait) deliver the UPLOAD's bytes to the DRAW_INDIRECT
// fetch on the FIRST frame?
//
//   Failure of "first-frame" checks = a stale static half at the draw
//   (a silent no-op draw - zero pixels) or a stale dynamic half (zero
//   instances) - either way the first-frame draw did not see the
//   complete command. The command readback distinguishes which half
//   (the bytes land eventually; the draw's fetch is the question).
//
// The second frame is the stability leg (the next frame's ALL_COMMANDS
// timeline wait always heals stranded writes - the historical one-frame
// lag; a first-frame PASS + second-frame PASS is the healthy shape, a
// first-frame FAIL + second-frame PASS is the stranded write).
//
// Bootstrap mirrors test_placement_gpu.cpp (hidden window + RM +
// swapchain + heap-allocated graph). CI-excluded like the other GPU
// tests (hosted runners lack a display/ICD).

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include <volk.h>
#include <SDL3/SDL.h>

#include <Engine/platform.h> // readFile (the exe-relative fallback)
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name)
{
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

// The command ABI (VkDrawIndexedIndirectCommand, 20 bytes - the same
// shape smvk::placement::PlacementIndirectCmd pins with its own
// static_assert).
struct IndirectCmd {
	uint32_t indexCount = 0;
	uint32_t instanceCount = 0;
	uint32_t firstIndex = 0;
	int32_t vertexOffset = 0;
	uint32_t firstInstance = 0;
};
static_assert(sizeof(IndirectCmd) == 20);

// Mirrors rhitest_indirect_write.cs.hlsl RhiTestIndirectWritePush
// (16 bytes).
struct IndirectWritePush {
	uint64_t indirect = 0;
	uint32_t instanceCount = 1;
	uint32_t firstInstance = 0;
};
static_assert(sizeof(IndirectWritePush) == 16);

} // namespace

int main()
{
	std::setvbuf(stderr, nullptr, _IONBF, 0);
	std::fprintf(stderr, "[rhi-gpu] stage: SDL window...\n");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "[rhi-gpu] SDL_Init failed: %s\n",
			SDL_GetError());
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow("rhi upload gpu", 64, 64,
		SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window) {
		std::fprintf(stderr, "[rhi-gpu] window failed: %s\n",
			SDL_GetError());
		return 1;
	}
	if (volkInitialize() != VK_SUCCESS) {
		std::fprintf(stderr, "[rhi-gpu] volkInitialize failed\n");
		return 1;
	}
	std::fprintf(stderr, "[rhi-gpu] stage: ResourceManager + swapchain...\n");
	smvk::ResourceManager rm(window);
	smvk::Swapchain swap(rm, window);
	auto rgPtr = std::make_unique<smvk::RenderGraph>(rm);
	smvk::RenderGraph& rg = *rgPtr;
	std::fprintf(stderr, "[rhi-gpu] stage: host up\n");

	// ---- the programs (the three include-free test shaders). ----
	smvk::Holder<smvk::ShaderModuleHandle> csModule, vsModule, psModule;
	smvk::ShaderProgram csProgram{}, drawProgram{};
	{
		std::vector<char> code = readFile(
			"content/shaders/rhitest_indirect_write.cs.hlsl.spv");
		Check(!code.empty(), "rhi.shader.cs.present");
		if (code.empty())
			return 1;
		csModule = rm.CreateShaderModule(code, "rhiTestIndirectCS");
		smvk::Shader cs{ .module = csModule,
			.entryPoint = "mainCS",
			.stage = smvk::ShaderStage::Compute };
		csProgram = { .shaders = { cs }, .shaderCount = 1 };

		code = readFile("content/shaders/rhitest_triangle.vert.hlsl.spv");
		Check(!code.empty(), "rhi.shader.vs.present");
		if (code.empty())
			return 1;
		vsModule = rm.CreateShaderModule(code, "rhiTestTriVS");
		code = readFile("content/shaders/rhitest_triangle.frag.hlsl.spv");
		Check(!code.empty(), "rhi.shader.ps.present");
		if (code.empty())
			return 1;
		psModule = rm.CreateShaderModule(code, "rhiTestTriPS");
		smvk::Shader vs{ .module = vsModule,
			.entryPoint = "mainVS",
			.stage = smvk::ShaderStage::Vertex };
		smvk::Shader ps{ .module = psModule,
			.entryPoint = "mainPS",
			.stage = smvk::ShaderStage::Fragment };
		drawProgram = { .shaders = { vs, ps }, .shaderCount = 2 };
	}

	// ---- the indirect command buffer: the SPLIT WRITER. ----
	// Static halves: the UPLOAD, declared for its FIRST consumer (the
	// compute rewrite - the placement seed's exact hint shape). Dynamic
	// halves: the same-frame compute pass. One 20-byte command.
	smvk::Holder<smvk::BufferHandle> indirectBuf;
	smvk::LogicalBufferHandle indirectL{};
	{
		indirectBuf = rm.CreateBuffer(sizeof(IndirectCmd),
			smvk::BufferType_Indirect, "rhi test indirect");
		Check(!indirectBuf.empty(), "rhi.indirect.buffer");
		indirectL = rg.AddExternalBuffer(indirectBuf, "rhi test indirect");
		Check(!indirectL.empty(), "rhi.indirect.logical");
		IndirectCmd statics{};
		statics.indexCount = 3; // the triangle
		statics.firstIndex = 0;
		statics.vertexOffset = 0;
		statics.instanceCount = 0; // the compute pass owns these...
		statics.firstInstance = 0; // ...(rewritten same-frame)
		rm.UploadDataToBuffer(indirectBuf, &statics, sizeof(statics),
			smvk::PipelineStageBit_Compute,
			smvk::AccessFlags::ShaderRead | smvk::AccessFlags::ShaderWrite,
			0, 0);
	}

	// ---- the index stream: the CLEAN-upload leg (uploaded once, never
	// rewritten - the pattern production uses for static meshes). Its
	// consumer is the draw's index fetch.
	smvk::Holder<smvk::BufferHandle> indexBuf;
	smvk::LogicalBufferHandle indexL{};
	{
		indexBuf = rm.CreateBuffer(sizeof(uint32_t) * 3,
			smvk::BufferType_Storage, "rhi test indices");
		Check(!indexBuf.empty(), "rhi.index.buffer");
		indexL = rg.AddExternalBuffer(indexBuf, "rhi test indices");
		Check(!indexL.empty(), "rhi.index.logical");
		const uint32_t indices[3] = { 0, 1, 2 };
		rm.UploadDataToBuffer(indexBuf, indices, sizeof(indices),
			smvk::PipelineStageBit_AllGraphics, smvk::AccessFlags::IndexRead,
			0, 0);
	}

	// ---- the offscreen color target (the readback proof surface). ----
	const uint32_t targetW = 256, targetH = 256;
	smvk::Holder<smvk::ImageHandle> target;
	smvk::Holder<smvk::ImageViewHandle> targetView; // test-scope: the
	// graph's external-view handle must outlive every frame that uses it
	// (a block-scoped Holder retires the view at the closing brace and
	// the attachment silently stops resolving).
	smvk::LogicalImageViewHandle targetL{};
	{
		smvk::ImageCreateInfo ci{};
		ci.extent = { targetW, targetH, 1 };
		ci.format = smvk::Format::RGBA8_UNorm;
		ci.usage = smvk::ImageUsage::ColorAttachment
			| smvk::ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "rhi test draw target";
		target = rm.CreateImage(ci, nullptr);
		Check(!target.empty(), "rhi.target.image");
		const smvk::ImageViewCreateInfo vci{
			.viewType = smvk::ImageViewType::View2D,
			.format = smvk::Format::RGBA8_UNorm,
			.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 },
			.name = "rhi test draw target view",
		};
		targetView = rm.CreateImageView(target, vci);
		Check(!targetView.empty(), "rhi.target.view");
		targetL = rg.AddExternalImageView(targetView, smvk::Format::RGBA8_UNorm,
			"rhi test draw target logical");
		Check(!targetL.empty(), "rhi.target.logical");
	}

	// ---- the frame record: compute (dynamic halves) + draw, one frame,
	// the SAME buffers. ----
	const auto recordFrame = [&](smvk::RenderGraph& g, uint32_t instanceCount) {
		rg.NewFrame();
		{
			smvk::RenderPass& cs = g.AddComputePass("RhiTestIndirectWrite");
			smvk::BufferSyncRequest write{};
			write.barrierMasks.accessMask = smvk::AccessFlags::ShaderWrite;
			write.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
			cs.AddBufferHint(indirectL, write);
			const IndirectWritePush push{
				rm.GetBufferDeviceAddress(indirectBuf),
				instanceCount, 0u };
			smvk::ShaderProgram* prog = &csProgram;
			cs.SetRecordingFunc([prog, push](smvk::CommandBuffer& cmd) {
				cmd.BindComputePipeline(*prog);
				cmd.PushConstants(0, sizeof(push), &push,
					smvk::ShaderStage::Compute);
				cmd.Dispatch(1, 1, 1);
			});
		}
		{
			smvk::RenderPass& draw = g.AddPass("RhiTestDraw");
			draw.SetOffset(0, 0);
			draw.SetExtent({ targetW, targetH });
			draw.SetColor({ targetL }, smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0.1f, 0.2f, 0.3f, 1.0f));
			// The PRODUCTION pairing rule (gfx_filter's AddConsumeHints):
			// the indirect command fetch runs at the fixed-function
			// DRAW_INDIRECT stage - the engine's AllGraphics maps to the
			// shader-stage union ONLY, so IndirectCommandRead MUST pair
			// with PipelineStageBit_DrawIndirect. The index fetch is
			// IndexRead at the graphics stages.
			smvk::BufferSyncRequest indirectRead{};
			indirectRead.barrierMasks.accessMask =
				smvk::AccessFlags::IndirectCommandRead;
			indirectRead.barrierMasks.stageMask =
				smvk::PipelineStageBit_DrawIndirect;
			draw.AddBufferHint(indirectL, indirectRead);
			smvk::BufferSyncRequest indexRead{};
			indexRead.barrierMasks.accessMask = smvk::AccessFlags::IndexRead;
			indexRead.barrierMasks.stageMask =
				smvk::PipelineStageBit_AllGraphics;
			draw.AddBufferHint(indexL, indexRead);
			const smvk::BufferHandle cmd = indirectBuf;
			const smvk::BufferHandle idx = indexBuf;
			smvk::ShaderProgram* prog = &drawProgram;
			draw.SetRecordingFunc([cmd, idx, prog](
					smvk::CommandBuffer& c) {
				c.BindIndexBuffer(idx, 0);
				c.BindRenderPipeline(*prog, smvk::VertexLayoutId::Empty,
					smvk::DepthSettings{ .depthFunc = smvk::CompareOp::Always,
						.writeEnable = false, .depthTestEnable = false },
					smvk::StencilSettings{}, smvk::BlendSettings{},
					smvk::MSAASettings{},
					smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
				c.DrawIndexedIndirect(cmd, 0, 1, sizeof(IndirectCmd));
			});
		}
		rm.UpdateDescriptors();
		smvk::Framebuffer fb = swap.AcquireFramebuffer();
		rg.Execute(fb);
		swap.EndAndPresentFramebuffer(fb);
	};

	// ---- FRAME 1: the split-writer frame (the whole point). ----
	std::fprintf(stderr, "[rhi-gpu] stage: frame 1 (split writer)\n");
	recordFrame(rg, 1);
	rm.FlushUploads();
	rm.WaitForUploads();

	// The command readback: which halves landed in the buffer (the bytes
	// always land eventually - the quiescent-device readback says which
	// half a same-frame draw could have trusted only if BOTH are here).
	IndirectCmd cmds[2] = {};
	{
		const bool ok = rm.DownloadDataFromBuffer(indirectBuf, cmds,
			sizeof(IndirectCmd));
		Check(ok, "rhi.frame1.cmd.readback");
	}
	Check(cmds[0].indexCount == 3 && cmds[0].firstIndex == 0
		&& cmds[0].vertexOffset == 0,
		"rhi.frame1.cmd.static.halves");
	Check(cmds[0].instanceCount == 1 && cmds[0].firstInstance == 0,
		"rhi.frame1.cmd.dynamic.halves");
	std::fprintf(stderr, "[rhi-gpu] frame1 cmd {idx=%u inst=%u fi=%u vo=%d"
		" firstInstance=%u}\n", cmds[0].indexCount, cmds[0].instanceCount,
		cmds[0].firstIndex, cmds[0].vertexOffset, cmds[0].firstInstance);

	// The pixels: the FIRST-frame draw must see the COMPLETE command
	// (both halves) - a stale static half reads indexCount 0 (a silent
	// no-op draw, zero pixels), a stale dynamic half reads instanceCount
	// 0 (same). THE regression.
	std::vector<uint8_t> px(size_t(targetW) * targetH * 4, 0);
	{
		const bool ok = rm.DownloadDataFromImage(target,
			smvk::ImageLayout::ColorAttachmentOptimal, px.data(), px.size());
		Check(ok, "rhi.frame1.target.readback");
		size_t lit = 0;
		for (size_t i = 0; i + 3 < px.size(); i += 4)
			if (px[i] > 100) ++lit; // clear is (25,51,76); the tri is white
		Check(lit > 10000, "rhi.frame1.pixels.first-frame-draw");
		std::fprintf(stderr, "[rhi-gpu] frame1: %zu/%zu pixels lit\n", lit,
			px.size() / 4);
	}

	// ---- FRAME 2: the stability leg (the historical healing frame). ----
	std::fprintf(stderr, "[rhi-gpu] stage: frame 2 (stability)\n");
	recordFrame(rg, 1);
	rm.FlushUploads();
	rm.WaitForUploads();
	{
		const bool ok = rm.DownloadDataFromImage(target,
			smvk::ImageLayout::ColorAttachmentOptimal, px.data(), px.size());
		Check(ok, "rhi.frame2.target.readback");
		size_t lit = 0;
		for (size_t i = 0; i + 3 < px.size(); i += 4)
			if (px[i] > 100) ++lit;
		Check(lit > 10000, "rhi.frame2.pixels.stability");
		std::fprintf(stderr, "[rhi-gpu] frame2: %zu/%zu pixels lit\n", lit,
			px.size() / 4);
	}

	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
