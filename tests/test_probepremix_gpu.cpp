// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// ProbePremixGpuTests: the DEVICE half of the probe pre-mix module's
// frame discipline. The CPU mirror (the NDF weight math) lives in
// CubemapTests; this pins the module's RENDERGRAPH contract on a real
// (hidden-window) Vulkan context:
//
//   1. POOL DISCIPLINE (the regression): AppendPasses runs every
//      active frame for the process lifetime - it must NOT mint graph
//      logical resources per call. The pre-fix code re-registered the
//      selection buffer once per pass per frame (AddExternalBuffer
//      twice per frame; NewFrame clears passes_ only, never the
//      logical pools), leaking two live LogicalBuffer entries per
//      frame into the 8192-slot pool until exhaustion (and the empty
//      handles then tripped AddBufferHint's assert). The test runs
//      thousands of NewFrame/AppendPasses cycles and asserts the
//      logical buffer + view pool live counts stay EXACTLY constant.
//   2. RECONFIGURE DISCIPLINE: Ensure()'s re-creation path (face size
//      or mip count change) must retire the old logical handles
//      through Destroy - the counts track the new shape, and a full
//      Destroy returns them to the pre-module baseline.
//   3. EXECUTE SMOKE + SELECTION READBACK: one real frame with a live
//      inner-hit probe entry (the fpLights tail shape,
//      MakeProbeLightEntry) and a real bindless source cubemap: both
//      passes dispatch on device (the mix pass's per-mip loop pushes
//      pc.dstIndex - the bindless storage-array indexing), and the
//      weights pass's selection reads back exactly (count 1, the
//      source cube's bindless index, exclusive weight 1 - the inner
//      dominant case).
//
// The MIX pass's CONTENT parity (every blended texel = the weighted
// sum of the sources, per mip) is the TOURMALINE_PROBE_PREMIX=0|1
// golden capture contract (the documented 0.00%-differing-pixels A/B
// against the per-pixel walk) - the blended image carries no
// TransferSrc usage, so it cannot be read back here without widening
// the production image's flags for a test.
//
// Bootstrap mirrors test_placement_gpu.cpp (RM needs a surface-based
// window; the graph must be heap-allocated - its inline pools are
// megabytes). CI-excluded like the other GPU tests (hosted runners
// lack a display/ICD); plain local ctest runs it on a real GPU.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <volk.h>
#include <SDL3/SDL.h>

#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/Rendergraph/probepremix.h>
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

} // namespace

int main()
{
	// Unbuffered progress: stderr survives an abort (stdout
	// block-buffers into a pipe and is lost).
	std::setvbuf(stderr, nullptr, _IONBF, 0);
	std::fprintf(stderr, "[premix-gpu] stage: SDL window...\n");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "[premix-gpu] SDL_Init failed: %s\n",
			SDL_GetError());
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow("premix gpu", 64, 64,
		SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window) {
		std::fprintf(stderr, "[premix-gpu] window failed: %s\n",
			SDL_GetError());
		return 1;
	}
	if (volkInitialize() != VK_SUCCESS) {
		std::fprintf(stderr, "[premix-gpu] volkInitialize failed\n");
		return 1;
	}
	std::fprintf(stderr, "[premix-gpu] stage: ResourceManager + swapchain...\n");
	smvk::ResourceManager rm(window);
	smvk::Swapchain swap(rm, window);
	auto rgPtr = std::make_unique<smvk::RenderGraph>(rm);
	smvk::RenderGraph& rg = *rgPtr;
	std::fprintf(stderr, "[premix-gpu] stage: host up\n");

	smvk::probepremix::ProbePremix premix;

	// ---- the probe's SOURCE cubemap: 8x8, 2 mips, RGBA16F, sampled
	// only. Content stays uninitialized (the smoke asserts STRUCTURE -
	// the selection - not radiance; content parity is the A/B golden
	// contract, see the header).
	smvk::Holder<smvk::ImageHandle> srcCube;
	smvk::Holder<smvk::ImageViewHandle> srcView;
	smvk::Holder<smvk::BindlessImageHandle> srcBindless;
	uint32_t cubeIndex = UINT32_MAX;
	{
		smvk::ImageCreateInfo ci{};
		ci.type = smvk::ImageType::Image2D;
		ci.extent = { 8, 8, 1 };
		ci.format = smvk::Format::RGBA16_Float;
		ci.usage = smvk::ImageUsage::Sampled;
		ci.mipLevels = 2;
		ci.arrayLayers = smvk::kCubemapFaceCount;
		ci.isCubemap = true;
		ci.name = "premix-test source cube";
		srcCube = rm.CreateImage(ci, nullptr);
		Check(!srcCube.empty(), "premix.src.image");
		smvk::ImageViewCreateInfo vci{};
		vci.viewType = smvk::ImageViewType::ViewCube;
		vci.format = smvk::Format::Undefined; // inherit
		vci.subresourceRange = { smvk::ImageAspect::Color, 0, 2, 0,
			smvk::kCubemapFaceCount };
		vci.name = "premix-test source cube view";
		srcView = rm.CreateImageView(srcCube, vci);
		Check(!srcView.empty(), "premix.src.view");
		srcBindless = rm.CreateBindlessImage(srcView,
			smvk::ImageLayout::ShaderReadOnlyOptimal);
		Check(!srcBindless.empty(), "premix.src.bindless");
		cubeIndex = rm.GetBindlessIndex(srcBindless);
		Check(cubeIndex != UINT32_MAX, "premix.src.bindless.index");
	}

	// ---- the probe entry tail (the fpLights shape the weights pass
	// walks): ONE inner-hit probe at the POI. Influence 10, fade 5,
	// no fitted box (equal boxMin/boxMax -> the symmetric-sphere
	// fallback inside the shader).
	const glm::vec3 poi(4.0f, 3.0f, 2.0f);
	smvk::Holder<smvk::BufferHandle> lightsBuf;
	uint64_t lightsBda = 0;
	{
		constexpr uint32_t kTailEntries = 8;
		std::vector<smvk::ForwardPlusLight> tail(kTailEntries);
		tail[0] = smvk::MakeProbeLightEntry(poi, 10.0f, 5.0f,
			cubeIndex, 2, poi, poi);
		lightsBuf = rm.CreateBuffer(
			sizeof(smvk::ForwardPlusLight) * kTailEntries,
			smvk::BufferType_Storage, "premix-test lights tail");
		Check(!lightsBuf.empty(), "premix.lights.buffer");
		rm.UploadDataToBuffer(lightsBuf, tail.data(),
			tail.size() * sizeof(tail[0]),
			smvk::PipelineStageBit_Compute,
			smvk::AccessFlags::ShaderRead, 0, 0);
		lightsBda = rm.GetBufferDeviceAddress(lightsBuf);
		Check(lightsBda != 0, "premix.lights.bda");
	}

	// ---- the bindless clamp/linear-mips sampler (the lit path's own
	// cubemap sampler description).
	uint32_t samplerIndex = UINT32_MAX;
	{
		smvk::SamplerCreateInfo sci{};
		sci.addressU = smvk::AddressMode::ClampToEdge;
		sci.addressV = smvk::AddressMode::ClampToEdge;
		sci.addressW = smvk::AddressMode::ClampToEdge;
		samplerIndex = rm.GetOrCreateSampler(sci);
		Check(samplerIndex != UINT32_MAX, "premix.sampler");
	}

	// =====================================================================
	// 1) Ensure + the pool-discipline loop.
	// =====================================================================
	std::fprintf(stderr, "[premix-gpu] stage: pool discipline\n");
	const uint32_t buffersBefore = rg.buffers_.numObjects();
	const uint32_t viewsBefore = rg.imageViews_.numObjects();
	Check(premix.Ensure(rm, rg, 64, 4), "premix.ensure");
	// Ensure's expected footprint: ONE logical buffer (the selection)
	// + (mipCount + 1) logical views (per-mip storage + the cube).
	const uint32_t expectedBuffers = buffersBefore + 1;
	const uint32_t expectedViews = viewsBefore + 4 + 1;
	Check(rg.buffers_.numObjects() == expectedBuffers,
		"premix.ensure.buffers.footprint");
	Check(rg.imageViews_.numObjects() == expectedViews,
		"premix.ensure.views.footprint");

	// Thousands of frames: AppendPasses per frame, NEVER executing -
	// the leak was purely in handle minting. Every iteration must see
	// the SAME live counts (the old code grew buffers by 2/frame; by
	// 4096 frames the 8192-entry pool exhausted and AddExternalBuffer
	// handed back empty handles).
	constexpr int kFrames = 3000;
	bool poolStable = true;
	for (int i = 0; i < kFrames; ++i) {
		rg.NewFrame();
		if (!premix.AppendPasses(rg, lightsBda, 0, 1, poi, samplerIndex)) {
			poolStable = false;
			std::fprintf(stderr, "[premix-gpu] AppendPasses failed at %d\n", i);
			break;
		}
		if (rg.buffers_.numObjects() != expectedBuffers
			|| rg.imageViews_.numObjects() != expectedViews) {
			poolStable = false;
			std::fprintf(stderr, "[premix-gpu] pool drift at frame %d:"
				" buffers %u (want %u), views %u (want %u)\n", i,
				rg.buffers_.numObjects(), expectedBuffers,
				rg.imageViews_.numObjects(), expectedViews);
			break;
		}
	}
	Check(poolStable, "premix.pool.discipline");

	// =====================================================================
	// 2) Reconfigure + teardown discipline.
	// =====================================================================
	std::fprintf(stderr, "[premix-gpu] stage: reconfigure discipline\n");
	Check(premix.Ensure(rm, rg, 32, 3), "premix.ensure.reconfigure");
	Check(rg.imageViews_.numObjects() == viewsBefore + 3 + 1,
		"premix.reconfigure.views.retired");
	Check(rg.buffers_.numObjects() == buffersBefore + 1,
		"premix.reconfigure.buffers.retired");
	premix.Destroy(rg);
	Check(rg.imageViews_.numObjects() == viewsBefore,
		"premix.destroy.views.baseline");
	Check(rg.buffers_.numObjects() == buffersBefore,
		"premix.destroy.buffers.baseline");

	// =====================================================================
	// 3) The execute smoke: a REAL frame with the inner-hit probe, then
	//    the selection readback. The mix pass runs its full per-mip
	//    dispatch loop (pc.dstIndex per mip) on device; the weights
	//    pass's output is the deterministic oracle below.
	// =====================================================================
	std::fprintf(stderr, "[premix-gpu] stage: execute smoke\n");
	Check(premix.Ensure(rm, rg, 64, 4), "premix.ensure.smoke");
	{
		rg.NewFrame();
		Check(premix.AppendPasses(rg, lightsBda, 0, 1, poi, samplerIndex),
			"premix.smoke.append");
		rm.UpdateDescriptors();
		smvk::Framebuffer fb = swap.AcquireFramebuffer();
		rg.Execute(fb);
		swap.EndAndPresentFramebuffer(fb);
		rm.FlushUploads();
		rm.WaitForUploads();
	}
	smvk::probepremix::BlendSelectionGpu sel{};
	{
		const bool ok = rm.DownloadDataFromBuffer(
			smvk::BufferHandle(premix.selectionBuffer), &sel, sizeof(sel));
		Check(ok, "premix.smoke.selection.readback");
	}
	Check(sel.count == 1, "premix.smoke.selection.count");
	Check(sel.indices[0] == cubeIndex,
		"premix.smoke.selection.cubemap.index");
	Check(sel.weights[0] == 1.0f,
		"premix.smoke.selection.inner.dominant");
	Check(sel.pos[0] == poi.x && sel.pos[1] == poi.y && sel.pos[2] == poi.z,
		"premix.smoke.selection.position");
	// The symmetric fallback box: no fitted box packed -> the shader
	// uses the influence sphere (h = influence = 10 per axis).
	Check(sel.boxMinus[0] == 10.0f && sel.boxMinus[1] == 10.0f
		&& sel.boxMinus[2] == 10.0f && sel.boxPlus[0] == 10.0f,
		"premix.smoke.selection.box");

	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
