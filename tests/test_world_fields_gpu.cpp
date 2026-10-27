// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// WorldFieldsGpuTests: the DEVICE half of the world-field system's
// milestone-2 acceptance — a real (hidden-window) Vulkan context, the
// atlas pagers doing real staged region uploads through the streamer,
// and blocking readbacks proving:
//
//   * UploadDataToImage3DRegion lands brick payloads at the exact slot
//     offsets with the exact bytes (whole-atlas readback vs a CPU-side
//     application of the same upload sequence) — including slot REUSE
//     after eviction churn (a 4-slot pool under a 16-cell window);
//   * the page-table buffer upload roundtrips (readback == staging);
//   * every resident page points at a slot whose GPU bytes are that
//     page's brick (regenerated from the source and compared per-page —
//     the end-to-end semantic check, on device memory);
//   * the atlas ends in SHADER_READ_ONLY_OPTIMAL (the readback's layout
//     contract — also what every frame's sampling relies on).
//
// The CPU-side contracts (addressing, windows, seam-free filtering) live
// in WorldFieldsTests; this test covers the RHI publish path only.
//
// Hand-rolled runner (no test framework dependency), like the other
// tests. The bootstrap mirrors test_retirement_fuzz.cpp: the RM requires
// a window (surface-based device pick) — a HIDDEN 64x64 one; no
// swapchain is created, the test plays the frame role itself
// (ComputeResidency -> PublishGpu per frame, FlushUploads +
// WaitForUploads at the end — the streamer's own serial chain orders the
// batches; with no graphics submits recorded there is nothing to wait
// besides the transfer chain).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <glm/glm.hpp>

#include <volk.h>
#include <SDL3/SDL.h>

#include <Engine/WorldFields/WorldField.h>
#include <Engine/WorldFields/WorldFieldAtlas.h>
#include <Engine/WorldFields/WorldFieldGpu.h>
#include <Engine/RHI/resource_manager.h>

namespace wf = engine::worldfields;

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

// A world-LINEAR R8 source (texel centers, minus halo — see the
// WorldRenderer checkerboard source comment for the convention).
struct GpuLinearSource final : wf::IWorldFieldBrickSource
{
	static uint8_t Quantized(glm::vec3 p)
	{
		float v = 0.125f + 0.0031f * p.x + 0.0021f * p.y + 0.0013f * p.z;
		v = std::min(std::max(v, 0.0f), 1.0f);
		return uint8_t(v * 255.0f + 0.5f);
	}

	bool GenerateBrick(const wf::WorldFieldDesc& field, uint32_t lod,
		int32_t x, int32_t y, int32_t z,
		std::byte* payload, size_t payloadBytes) override
	{
		const uint32_t stored = wf::kBrickStoredRes;
		const float voxel = field.baseVoxelSizeMeters * float(1u << lod);
		const glm::vec3 brickMin = field.originMeters
			+ glm::vec3(float(x), float(y), float(z))
				* (float(wf::kBrickInteriorRes) * voxel);
		size_t i = 0;
		for (uint32_t tz = 0; tz < stored; ++tz)
			for (uint32_t ty = 0; ty < stored; ++ty)
				for (uint32_t tx = 0; tx < stored; ++tx) {
					const glm::vec3 texel = brickMin
						+ glm::vec3(float(tx) + 0.5f - float(wf::kBrickBorder),
							float(ty) + 0.5f - float(wf::kBrickBorder),
							float(tz) + 0.5f - float(wf::kBrickBorder)) * voxel;
					payload[i++] = std::byte(Quantized(texel));
				}
		return true;
	}
};

GpuLinearSource g_source;

} // namespace

int main()
{
	std::printf("[wf-gpu] stage: SDL window...\n");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "[wf-gpu] SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow("world fields gpu", 64, 64,
		SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window) {
		std::fprintf(stderr, "[wf-gpu] window failed: %s\n", SDL_GetError());
		return 1;
	}
	// volk's instance table must load before ANY vk* call.
	if (volkInitialize() != VK_SUCCESS) {
		std::fprintf(stderr, "[wf-gpu] volkInitialize failed\n");
		return 1;
	}
	std::printf("[wf-gpu] stage: ResourceManager ctor...\n");
	smvk::ResourceManager rm(window);

	// ---- field setup: R8, two LODs, a 16-cell window over a 4-slot
	// pool (2x2x1 slots = a 136x136x68 atlas) — forced eviction churn. ----
	wf::WorldFieldSystem sys;
	wf::WorldFieldStreamingPolicy policy{};
	policy.maxBrickLoadsPerFrame = 2;
	policy.evictHysteresisFrames = 1;
	policy.proactiveEvictFrames = 4;
	policy.protectCoarsestLod = true;
	sys.SetStreamingPolicy(policy);

	wf::WorldFieldDesc d{};
	d.name = "GpuTestField";
	d.format = smvk::Format::R8_UNorm;
	d.encoding = wf::WorldFieldEncoding::Linear;
	d.baseVoxelSizeMeters = 1.0f;
	d.lodCount = 2;
	d.tableDimX = 2;
	d.tableDimY = 2;
	d.tableDimZ = 2;
	d.poolSlotsX = 2;
	d.poolSlotsY = 2;
	d.poolSlotsZ = 1;
	d.fallbackValue = glm::vec4(0.0f);
	wf::WorldFieldHandle handles[1] = {};
	wf::IWorldFieldBrickSource* sources[1] = { &g_source };
	const uint32_t registered = sys.RegisterFields(&d, 1, handles);
	Check(registered == 1 && handles[0].Valid(), "gpu.register-batch");
	if (registered == 1)
		sys.SetBrickSources(handles, sources, 1);

	// CPU-side expected atlas bytes: the same region-upload sequence the
	// system enqueues, applied in order (the ground truth for readback).
	const uint32_t aw = 2 * 68, ah = 2 * 68, ad = 1 * 68;
	std::vector<uint8_t> expected(size_t(aw) * ah * ad, 0u);

	uint64_t evictions = 0;
	for (uint32_t frame = 0; frame < 16; ++frame) {
		// A walking + teleporting interest point (churn + window moves).
		glm::vec3 eye(40.0f + float(frame) * 48.0f, 24.0f, 40.0f);
		if (frame == 9)
			eye = glm::vec3(-420.0f, 180.0f, -260.0f);

		sys.ComputeResidency(eye, frame);
		for (uint32_t u = 0; u < sys.PendingUploadCount(); ++u) {
			const wf::WorldFieldRegionUpload& up = sys.PendingUploads()[u];
			const int32_t ox = up.region.offset.x, oy = up.region.offset.y, oz = up.region.offset.z;
			// Row-by-row (the payload rows are 68-contiguous, the atlas
			// row pitch is aw — a flat slice memcpy would smear).
			for (uint32_t z = 0; z < 68; ++z)
				for (uint32_t y = 0; y < 68; ++y)
					std::memcpy(&expected[(size_t(oz + z) * ah + size_t(oy + y)) * aw + size_t(ox)],
						up.payload + (size_t(z) * 68 + size_t(y)) * 68, 68);
		}
		evictions += sys.Stats().evictions;
		sys.PublishGpu(rm); // region uploads THEN the table upload, one batch
	}
	Check(evictions > 0, "gpu.churn-evictions-before-readback");

	// Block until every staged batch retired; the readbacks below need a
	// quiescent device (debug readback stalls by design).
	rm.FlushUploads();
	rm.WaitForUploads();

	// ---- 1) the GPU table buffer content matches the CPU staging ----
	{
		const wf::WorldFieldGpuBinding binding = sys.GpuBinding();
		Check(binding.Valid() && binding.fieldCount == 1, "gpu.binding-valid");
		Check(binding.tableBda != 0, "gpu.table-bda");

		const uint32_t used = sys.TableUsedBytes();
		std::vector<uint8_t> readback(used, 0u);
		const bool ok = rm.DownloadDataFromBuffer(sys.TableBuffer(),
			readback.data(), used);
		Check(ok, "gpu.table-readback-ok");
		Check(std::memcmp(readback.data(), sys.TableBytes(), used) == 0,
			"gpu.table-roundtrip (readback == staging)");
	}

	// ---- 2) the atlas image bytes match the applied upload sequence ----
	const smvk::ImageHandle atlas = sys.FieldAtlas(0);
	Check(!atlas.empty(), "gpu.atlas-created");
	{
		const size_t atlasBytes = size_t(aw) * ah * ad;
		std::vector<uint8_t> readback(atlasBytes, 0u);
		// The upload path leaves the atlas SHADER_READ_ONLY_OPTIMAL — the
		// layout the per-frame sampling relies on (and this readback must
		// declare; a wrong layout would DISCARD the contents on copy).
		const bool ok = rm.DownloadDataFromImage(atlas,
			smvk::ImageLayout::ShaderReadOnlyOptimal, readback.data(), atlasBytes);
		Check(ok, "gpu.atlas-readback-ok");
		bool equal = true;
		size_t firstDiff = SIZE_MAX;
		for (size_t i = 0; i < atlasBytes; ++i)
			if (readback[i] != expected[i]) {
				equal = false;
				if (firstDiff == SIZE_MAX)
					firstDiff = i;
				break;
			}
		(void)firstDiff;
		Check(equal, "gpu.atlas-bytes-match (region uploads land exactly)");
	}

	// ---- 3) semantic: every resident page's slot holds THAT page's
	// brick bytes (regenerate + compare, per page, through device memory) ----
	{
		const std::byte* table = sys.TableBytes();
		wf::WorldFieldTableHeader header{};
		std::memcpy(&header, table, sizeof(header));
		bool pagesHoldOwnData = true;
		int pagesChecked = 0;
		std::vector<uint8_t> atlasBytes(size_t(aw) * ah * ad, 0u);
		if (rm.DownloadDataFromImage(atlas, smvk::ImageLayout::ShaderReadOnlyOptimal,
			atlasBytes.data(), atlasBytes.size())) {
			for (uint32_t fId = 0; fId < header.fieldCount; ++fId) {
				wf::WorldFieldGpu f{};
				std::memcpy(&f, table + 16 + size_t(fId) * header.fieldStride, sizeof(f));
				std::vector<std::byte> brick(size_t(68) * 68 * 68);
				for (uint32_t lod = 0; lod < f.meta.z; ++lod) {
					const wf::WorldLevelGpu& lv = f.levels[lod];
					for (uint32_t e = 0;
						e < lv.windowDim.x * lv.windowDim.y * lv.windowDim.z; ++e) {
						wf::WorldPageGpu page{};
						std::memcpy(&page,
							table + lv.pageTableOffset + e * 16u, sizeof(page));
						if (page.atlasSlot < 0)
							continue;
						// regenerate the brick the page claims
						if (!g_source.GenerateBrick(d, lod,
							page.brickX, page.brickY, page.brickZ,
							brick.data(), brick.size())) {
							pagesHoldOwnData = false;
							continue;
						}
						const uint32_t s = uint32_t(page.atlasSlot);
						const uint32_t sx = s % f.atlasSlots.x;
						const uint32_t sy = (s / f.atlasSlots.x) % f.atlasSlots.y;
						const uint32_t sz = s / (f.atlasSlots.x * f.atlasSlots.y);
						const size_t off = (size_t(sz * 68) * ah + size_t(sy * 68)) * aw
							+ size_t(sx * 68);
						const std::byte* got = reinterpret_cast<const std::byte*>(
							&atlasBytes[off]);
						if (std::memcmp(got, brick.data(), brick.size()) != 0)
							pagesHoldOwnData = false;
						++pagesChecked;
					}
				}
			}
		}
		else {
			pagesHoldOwnData = false;
		}
		Check(pagesChecked > 0, "gpu.pages-checked");
		Check(pagesHoldOwnData, "gpu.page-points-at-own-slot (device memory)");
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";

	SDL_DestroyWindow(window);
	SDL_Quit();
	return g_failures == 0 ? 0 : 1;
}
