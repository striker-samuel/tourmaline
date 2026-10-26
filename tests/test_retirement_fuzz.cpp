// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// retirement_fuzz: randomized soak for the deferred-destruction retirement
// system (GPU resource retirement vs frames in flight) on a real offscreen
// Vulkan context - no swapchain, no presentation.
//
// What is fuzzed (the three retirement legs + the drain):
//   * Cluster mesh heap ranges: UploadStaticMesh / UploadStaticMeshes /
//     UploadStaticMeshesDirect (incl. degenerate entries -> the all-or-nothing
//     carve-loop rollback) + DestroyMesh, whose virtual-block frees must be
//     RETIREMENT-ORDERED (DeferDestroy), never in place.
//   * Bindless descriptor slots: CreateTexture + Holder release (the
//     destroy(TextureHandle) chain) - the slot must stay HELD until its
//     retire key drains, then recycle.
//   * ProcessDeferrredTasks drain cadence.
//
// The fake swapchain: the fuzzer owns a private TIMELINE semaphore and plays
// the retirement contract's both sides, like VulkanSwapchain does:
//   NotifyGraphicsSubmit(sem, v) + vkSignalSemaphore(sem, v)
//     == "frame v submitted and completed" (published + completed in one
//        step - the streamer's dedicated-queue transfer submits WAIT the
//        last published graphics point, so a lagging host-side completion
//        would deadlock the single-threaded fuzz host inside DeferDestroy's
//        waitAll; the real swapchain's completions are GPU-signaled)
//   ProcessDeferrredTasks() == the between-frames drain
// Only the fuzzer signals this semaphore, so the model knows EXACTLY when a
// retirement has drained: DeferDestroy keys a destroy at
//   published + smvk::limits::MaxFramesInFlight   (the retire margin rule)
// and it drains once (counter >= key) AND (a drain ran past that signal).
//
// Oracles (checked after EVERY op):
//   1. EXACT block accounting: vmaGetVirtualBlockStatistics must equal the
//      model's live + UNDRAINED-retiring ranges (count + used bytes, both the
//      vertex and the index block). A premature free (the pre-fix immediate
//      vmaVirtualFree) drops the stats below the model at the very first
//      destroy; a leak (a rollback that loses ranges) leaves them above.
//   2. Range disjointness: live + undrained-retiring ranges must be pairwise
//      disjoint per block (a premature free hands the same range to a new
//      mesh while the model still holds it retiring).
//   3. Bindless slot hold: a newly created texture's slot must not collide
//      with any live or undrained-retiring slot (the pre-fix immediate
//      bindless release recycled the slot under in-flight retirement).
//   4. Full reclaim at the end: destroy everything, complete the timeline,
//      drain - both blocks must return to zero allocations / zero used bytes.
//
// Deterministic: fixed default seed + iteration count, overridable via
// TOURMALINE_FUZZ_SEED / TOURMALINE_FUZZ_ITERS; a failure prints the seed +
// iteration + expected/actual for exact reproduction.
//
// Hand-rolled runner (no test framework dependency), like the other tests.

#include <Engine/App/cluster.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/mesh.h>
#include "Vulkan/detail/vulkan_impl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace {

int g_checks = 0;

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

// One heap range (vertex units for the vertex block, bytes for the index
// block). retirePoint == 0 means LIVE; otherwise the retire key the owning
// resource's free was deferred at (published + smvk::limits::MaxFramesInFlight).
struct RangeRow {
	uint64_t base = 0;
	uint64_t size = 0;
	uint64_t retirePoint = 0;
};

bool Overlaps(const RangeRow& a, const RangeRow& b)
{
	return a.base < b.base + b.size && b.base < a.base + a.size;
}

// One live mesh: the handle + the EXACT rows it owns (no row matching at
// destroy time - the rows ride with the entry).
struct MeshEntry {
	smvk::ClusterStaticMeshHandle handle;
	RangeRow vertex;
	RangeRow index; // size 0 = index-free mesh
};

struct FuzzModel {
	std::vector<RangeRow> vertexRetiring;
	std::vector<RangeRow> indexRetiring;
	std::vector<RangeRow> texSlotsRetiring; // base = bindless slot, size = 1
	std::vector<MeshEntry> entries;
	std::vector<smvk::Holder<smvk::TextureHandle>> textures;
	std::vector<uint32_t> texSlotsLive;
	uint64_t published = 0;  // last NotifyGraphicsSubmit value
	uint64_t signaled = 0;   // timeline semaphore counter (fuzzer-signaled)
	uint64_t processed = 0;   // counter value at the last drain: retires with
	                          // key <= processed have drained

	[[nodiscard]] bool Held(uint64_t point) const
	{
		return point > processed;
	}

	void PruneDrained()
	{
		auto drained = [this](const RangeRow& r) { return !Held(r.retirePoint); };
		vertexRetiring.erase(std::remove_if(
			vertexRetiring.begin(), vertexRetiring.end(), drained),
			vertexRetiring.end());
		indexRetiring.erase(std::remove_if(
			indexRetiring.begin(), indexRetiring.end(), drained),
			indexRetiring.end());
		texSlotsRetiring.erase(std::remove_if(
			texSlotsRetiring.begin(), texSlotsRetiring.end(), drained),
			texSlotsRetiring.end());
	}
};

FuzzModel g_model;
uint64_t g_seed = 0;

[[noreturn]] void Fail(const char* what, uint32_t iter, const char* op,
	const char* detail)
{
	std::printf("FAIL %s (seed %llu, iter %u, op '%s'): %s\n",
		what, (unsigned long long)g_seed, iter, op ? op : "?",
		detail ? detail : "?");
	std::printf("     reproduce: TOURMALINE_FUZZ_SEED=%llu\n",
		(unsigned long long)g_seed);
	std::exit(1);
}

// ---------------------------------------------------------------------------
// The oracle: exact block accounting + disjointness against the model
// ---------------------------------------------------------------------------
void CheckBlocks(uint32_t iter, const char* op)
{
	FuzzModel& m = g_model;
	smvk::StaticHeapArena::Statistics vs{}, is{};
	vs = smvk::staticVertexFmtHeap.m_staticVertexBlock.Stats();
	is = smvk::staticVertexFmtHeap.m_staticIndexBlock.Stats();

	uint32_t wantVertexCount = 0, wantIndexCount = 0;
	uint64_t wantVertexBytes = 0, wantIndexBytes = 0;
	for (const MeshEntry& e : m.entries) {
		++wantVertexCount;
		wantVertexBytes += e.vertex.size;
		if (e.index.size) {
			++wantIndexCount;
			wantIndexBytes += e.index.size;
		}
	}
	for (const RangeRow& r : m.vertexRetiring)
		if (m.Held(r.retirePoint)) {
			++wantVertexCount;
			wantVertexBytes += r.size;
		}
	for (const RangeRow& r : m.indexRetiring)
		if (m.Held(r.retirePoint)) {
			++wantIndexCount;
			wantIndexBytes += r.size;
		}

	char detail[256];
	if (vs.allocationCount != wantVertexCount
		|| vs.allocationBytes != wantVertexBytes) {
		std::snprintf(detail, sizeof detail,
			"vertex block allocs %u (want %u), used %llu (want %llu): "
			"premature free (stats below model) or leak (above)",
			vs.allocationCount, wantVertexCount,
			(unsigned long long)vs.allocationBytes,
			(unsigned long long)wantVertexBytes);
		Fail("vertex-block accounting", iter, op, detail);
	}
	if (is.allocationCount != wantIndexCount
		|| is.allocationBytes != wantIndexBytes) {
		std::snprintf(detail, sizeof detail,
			"index block allocs %u (want %u), used %llu (want %llu)",
			is.allocationCount, wantIndexCount,
			(unsigned long long)is.allocationBytes,
			(unsigned long long)wantIndexBytes);
		Fail("index-block accounting", iter, op, detail);
	}

	// Disjointness: live + undrained-retiring must never overlap per block
	// (a premature free hands the same range to a new mesh while the model
	// still holds it retiring).
	std::vector<RangeRow> heldV, heldI;
	for (const MeshEntry& e : m.entries) {
		heldV.push_back(e.vertex);
		if (e.index.size)
			heldI.push_back(e.index);
	}
	for (const RangeRow& r : m.vertexRetiring)
		if (m.Held(r.retirePoint))
			heldV.push_back(r);
	for (const RangeRow& r : m.indexRetiring)
		if (m.Held(r.retirePoint))
			heldI.push_back(r);
	for (size_t a = 0; a < heldV.size(); ++a)
		for (size_t b = a + 1; b < heldV.size(); ++b)
			if (Overlaps(heldV[a], heldV[b])) {
				std::snprintf(detail, sizeof detail,
					"vertex ranges [%llu+%llu) and [%llu+%llu) overlap",
					(unsigned long long)heldV[a].base,
					(unsigned long long)heldV[a].size,
					(unsigned long long)heldV[b].base,
					(unsigned long long)heldV[b].size);
				Fail("vertex-range disjointness", iter, op, detail);
			}
	for (size_t a = 0; a < heldI.size(); ++a)
		for (size_t b = a + 1; b < heldI.size(); ++b)
			if (Overlaps(heldI[a], heldI[b])) {
				std::snprintf(detail, sizeof detail,
					"index ranges [%llu+%llu) and [%llu+%llu) overlap",
					(unsigned long long)heldI[a].base,
					(unsigned long long)heldI[a].size,
					(unsigned long long)heldI[b].base,
					(unsigned long long)heldI[b].size);
				Fail("index-range disjointness", iter, op, detail);
			}

	// Bindless slots: every live or undrained-retiring slot is unique.
	std::vector<uint64_t> slots;
	for (uint32_t s : m.texSlotsLive)
		slots.push_back(s);
	for (const RangeRow& r : m.texSlotsRetiring)
		if (m.Held(r.retirePoint))
			slots.push_back(r.base);
	for (size_t a = 0; a < slots.size(); ++a)
		for (size_t b = a + 1; b < slots.size(); ++b)
			if (slots[a] == slots[b]) {
				std::snprintf(detail, sizeof detail,
					"bindless slot %llu held twice (live/retiring)",
					(unsigned long long)slots[a]);
				Fail("bindless-slot uniqueness", iter, op, detail);
			}
	++g_checks; // one accounting check per op
}

// The bindless-slot hold oracle at creation time: a newly created texture's
// slot must not collide with any live or UNDRAINED-RETIRING slot (the pre-fix
// immediate bindless release recycled the slot under in-flight retirement).
void CheckNewSlot(uint32_t slot, uint32_t iter, const char* op)
{
	FuzzModel& m = g_model;
	char detail[160];
	for (uint32_t s : m.texSlotsLive)
		if (s == slot) {
			std::snprintf(detail, sizeof detail,
				"new texture slot %u collides with LIVE slot", slot);
			Fail("bindless-slot hold", iter, op, detail);
		}
	for (const RangeRow& r : m.texSlotsRetiring)
		if (m.Held(r.retirePoint) && r.base == slot) {
			std::snprintf(detail, sizeof detail,
				"new texture slot %u recycled while its retirement "
				"(key %llu > processed %llu) is still undrained",
				slot, (unsigned long long)r.retirePoint,
				(unsigned long long)m.processed);
			Fail("bindless-slot hold", iter, op, detail);
		}
}

} // namespace

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const uint64_t seedEnv = std::strtoull(
		std::getenv("TOURMALINE_FUZZ_SEED")
			? std::getenv("TOURMALINE_FUZZ_SEED") : "0",
		nullptr, 0);
	g_seed = seedEnv ? seedEnv : 0x7AB5EED1ull;
	const uint32_t iters = std::getenv("TOURMALINE_FUZZ_ITERS")
		? (uint32_t)std::strtoul(
			std::getenv("TOURMALINE_FUZZ_ITERS"), nullptr, 0)
		: 400;

	std::printf("[fuzz] retirement soak: seed %llu, %u iterations\n",
		(unsigned long long)g_seed, iters);

	// Offscreen context: the RMI core currently REQUIRES a window (the
	// surface-based device pick + queue selection - the old "window may be
	// nullptr for headless" header comment was aspirational;
	// CreateVulkanCoreWithWindow asserts it). A HIDDEN 64x64 window gives
	// the RM its surface; the fuzzer never creates a Swapchain - the
	// retirement system needs no presentation, and the fuzzer plays the
	// swapchain's role in the retirement contract itself.
	std::printf("[fuzz] stage: SDL window...\n");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::printf("FAIL setup: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow("retirement fuzz", 64, 64,
		SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window) {
		std::printf("FAIL setup: SDL_CreateWindow: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	// volk's instance table must be loaded BEFORE any vk* call (the App
	// does this ahead of its RM ctor; without it vkCreateInstance resolves
	// to a null function pointer - the first fuzz runs AV'd exactly there).
	if (volkInitialize() != VK_SUCCESS) {
		std::printf("FAIL setup: volkInitialize\n");
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}
	std::printf("[fuzz] stage: ResourceManager ctor...\n");
	smvk::ResourceManager rm(window);
	std::printf("[fuzz] stage: AllocateStaticVertexFmt...\n");
	smvk::AllocateStaticVertexFmt(rm);
	// HEAP-allocated: the cluster carries several inline-storage pools and
	// is far too large for the default stack (the engine always owns
	// clusters by pointer too).
	std::printf("[fuzz] stage: Cluster ctor...\n");
	auto cluster = std::make_unique<smvk::Cluster>(&rm);
	auto* vctx = static_cast<smvk::VulkanResourceManager*>(
		rm.GetVulkanOpaque());
	const VkDevice dev = vctx->VkDeviceHandle();

	VkSemaphoreTypeCreateInfo semType{
		VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr,
		VK_SEMAPHORE_TYPE_TIMELINE, 0 };
	VkSemaphoreCreateInfo semCi{
		VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &semType, 0 };
	VkSemaphore gpuTimeline = VK_NULL_HANDLE;
	if (vkCreateSemaphore(dev, &semCi, nullptr, &gpuTimeline) != VK_SUCCESS) {
		std::printf("FAIL setup: timeline semaphore creation\n");
		return 1;
	}

	FuzzModel& m = g_model;
	std::mt19937_64 rng(g_seed);
	// The content blob for UploadStaticMeshesDirect: sources address
	// packed-SoA stream subranges of one blob by byte offset (all picked
	// strictly in-bounds below).
	std::vector<uint8_t> blob(1u << 20, 0xA5);
	constexpr uint32_t kMaxVerts = 2048;
	constexpr uint32_t kMaxIdx = 4096;

	// Publish the first point up front: retires must key on a real point
	// (a zero value would make DeferDestroy run everything inline).
	m.published = 1;
	vctx->NotifyGraphicsSubmit(gpuTimeline, m.published);

	const auto randVerts = [&]() { return uint32_t(1 + rng() % kMaxVerts); };
	// 0 indices = index-free mesh (the no-index-allocation path).
	const auto randIdx = [&]() { return uint32_t(rng() % (kMaxIdx + 1)); };

	const auto fillUpload = [&](smvk::StaticMeshUpload& up,
		uint32_t vc, uint32_t ic) {
		up.vertices.resize(vc);
		up.indices.resize(ic);
		for (uint32_t v = 0; v < vc; ++v) {
			up.vertices[v].pos = glm::vec3(float(v), float(v) * 0.25f, 0.f);
			up.vertices[v].normal = glm::vec3(0.f, 1.f, 0.f);
		}
		for (uint32_t i = 0; i < ic; ++i)
			up.indices[i] = i % vc;
		up.aabbMin = glm::vec3(-1.f);
		up.aabbMax = glm::vec3(1.f);
	};

	// Reads the returned handle's surface rows into the model (the draw args
	// ARE the heap ranges: absolute baseVertex / firstIndex).
	const auto addMesh = [&](smvk::ClusterStaticMeshHandle h,
		uint32_t vc, uint32_t ic) {
		const smvk::StaticMesh* mesh = cluster->staticMeshes.get(h);
		if (!mesh || mesh->geometry.surfaces.empty())
			Fail("upload shape", 0, "addMesh",
				"returned handle has no surface");
		const smvk::Surface& s = mesh->geometry.surfaces[0];
		MeshEntry e{ h, { uint64_t(s.baseVertex), vc, 0 }, {} };
		if (ic)
			e.index = { uint64_t(s.firstIndex) * sizeof(uint32_t),
				uint64_t(ic) * sizeof(uint32_t), 0 };
		m.entries.push_back(e);
	};

	// The DeferDestroy margin rule (see the file header): a destroy at the
	// current published point keys its retirement at
	// published + smvk::limits::MaxFramesInFlight.
	const auto retireKey = [&]() {
		return m.published + smvk::limits::MaxFramesInFlight;
	};

	// Destroy a live mesh: the model rows move to retiring keyed at the
	// margin rule.
	const auto destroyEntry = [&](size_t slot) {
		const MeshEntry e = m.entries[slot];
		m.entries.erase(m.entries.begin() + slot);
		cluster->DestroyMesh(e.handle);
		m.vertexRetiring.push_back(
			{ e.vertex.base, e.vertex.size, retireKey() });
		if (e.index.size)
			m.indexRetiring.push_back(
				{ e.index.base, e.index.size, retireKey() });
	};

	for (uint32_t iter = 0; iter < iters; ++iter) {
		const char* op = "advance";
		if (iter % 25 == 0)
			std::printf("[fuzz] iter %u/%u (pub %llu sig %llu proc %llu, live %zu)\n",
				iter, iters, (unsigned long long)m.published,
				(unsigned long long)m.signaled,
				(unsigned long long)m.processed, m.entries.size());

		// ---- advance step (the fake swapchain rhythm) ----
		// "submitted" and "completed" are FOLDED: the streamer's dedicated-
		// queue transfer submits WAIT the last published graphics point
		// (UploadStreamer::submitTransfer), so a lagging host-side completion
		// signal would deadlock the single-threaded fuzz host inside
		// DeferDestroy's waitAll (the real swapchain's completions are
		// GPU-signaled, never host-blocked like that). Signaling eagerly at
		// publish keeps every waiter satisfied; the RETIREMENT cadence
		// still lags by construction - a destroy keys at
		// published + smvk::limits::MaxFramesInFlight, so it drains only after two MORE
		// publishes pass + a drain runs.
		{
			const uint64_t roll = rng() % 100;
			if (roll < 65) {
				// "frame round trip": 1-2 frames submit + complete.
				const uint32_t frames = uint32_t(1 + rng() % 2);
				for (uint32_t f = 0; f < frames; ++f) {
					++m.published;
					vctx->NotifyGraphicsSubmit(gpuTimeline, m.published);
					VkSemaphoreSignalInfo si{
						VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr,
						gpuTimeline, m.published };
					if (vkSignalSemaphore(dev, &si) != VK_SUCCESS)
						Fail("setup", iter, op,
							"timeline signal failed (non-monotonic?)");
					m.signaled = m.published;
				}
				op = "publish";
			} else if (m.signaled > m.processed) {
				// Between-frames drain: everything keyed <= the signaled
				// counter is complete and retires now.
				vctx->ProcessDeferrredTasks();
				m.processed = m.signaled;
				m.PruneDrained();
				op = "process";
			}
		}

		// ---- main op ----
		const uint64_t kind = rng() % 100;
		if (kind < 26) {
			// Single upload (sometimes degenerate: 0 vertices -> clean {}).
			op = "uploadSingle";
			const bool degenerate = (rng() % 10) == 0;
			smvk::StaticMeshUpload up;
			if (!degenerate)
				fillUpload(up, randVerts(), randIdx());
			const auto h = cluster->UploadStaticMesh(up);
			if (degenerate != h.empty()) {
				char d[96];
				std::snprintf(d, sizeof d, "degenerate=%d but handle %s",
					int(degenerate), h.empty() ? "empty" : "non-empty");
				Fail("uploadSingle result", iter, op, d);
			}
			if (!degenerate)
				addMesh(h, uint32_t(up.vertices.size()),
					uint32_t(up.indices.size()));
		} else if (kind < 36) {
			// Batch upload (sometimes with a degenerate entry mixed in: the
			// whole batch must fail with NOTHING held - the carve-loop
			// rollback frees every carved range).
			op = "uploadBatch";
			const size_t n = 1 + size_t(rng() % 6);
			const bool withDegenerate = n > 1 && (rng() % 3) == 0;
			std::vector<smvk::StaticMeshUpload> ups(n);
			for (size_t i = 0; i < n; ++i) {
				const bool deg = withDegenerate && i == n / 2;
				if (!deg)
					fillUpload(ups[i], randVerts(), randIdx());
			}
			const auto hs = cluster->UploadStaticMeshes(ups);
			if (withDegenerate) {
				if (!hs.empty())
					Fail("uploadBatch all-or-nothing", iter, op,
						"degenerate batch returned handles");
			} else {
				if (hs.size() != n)
					Fail("uploadBatch all-or-nothing", iter, op,
						"clean batch returned a short handle list");
				for (size_t i = 0; i < n; ++i)
					addMesh(hs[i], uint32_t(ups[i].vertices.size()),
						uint32_t(ups[i].indices.size()));
			}
		} else if (kind < 46) {
			// Direct batch (same degenerate coverage; sources address the
			// content blob with in-bounds offsets).
			op = "uploadDirect";
			const size_t n = 1 + size_t(rng() % 4);
			const bool withDegenerate = n > 1 && (rng() % 3) == 0;
			std::vector<smvk::Cluster::StaticMeshDirectSource> srcs(n);
			for (size_t i = 0; i < n; ++i) {
				smvk::Cluster::StaticMeshDirectSource& s = srcs[i];
				if (withDegenerate && i == n / 2) {
					s.vertexCount = 0; // degenerate entry
					continue;
				}
				s.vertexCount = randVerts();
				s.indexCount = randIdx();
				s.posSrc = rng() % (blob.size()
					- size_t(s.vertexCount) * sizeof(smvk::VertexPos));
				s.shadeSrc = rng() % (blob.size()
					- size_t(s.vertexCount) * sizeof(smvk::VertexShading));
				s.idxSrc = rng() % (blob.size()
					- size_t(s.indexCount) * sizeof(uint32_t));
				s.aabbMin = glm::vec3(-1.f);
				s.aabbMax = glm::vec3(1.f);
			}
			const auto hs = cluster->UploadStaticMeshesDirect(
				blob.data(), srcs);
			if (withDegenerate) {
				if (!hs.empty())
					Fail("uploadDirect all-or-nothing", iter, op,
						"degenerate batch returned handles");
			} else {
				if (hs.size() != n)
					Fail("uploadDirect all-or-nothing", iter, op,
						"clean batch returned a short handle list");
				for (size_t i = 0; i < n; ++i)
					addMesh(hs[i], srcs[i].vertexCount, srcs[i].indexCount);
			}
		} else if (kind < 72 && !m.entries.empty()) {
			op = "destroyMesh";
			destroyEntry(size_t(rng() % m.entries.size()));
		} else if (kind < 82) {
			op = "texCreate";
			smvk::ImageCreateInfo ci{};
			ci.type = smvk::ImageType::Image2D;
			ci.extent = { 1, 1, 1 };
			ci.format = smvk::Format::RGBA8_UNorm;
			ci.usage = smvk::ImageUsage::Sampled;
			ci.name = "fuzz texture";
			auto holder = rm.CreateTexture(ci, nullptr);
			if (holder.empty())
				Fail("texCreate", iter, op, "texture creation failed");
			const uint32_t slot = rm.GetTextureBindlessIndex(holder);
			CheckNewSlot(slot, iter, op);
			m.texSlotsLive.push_back(slot);
			m.textures.push_back(std::move(holder));
		} else if (kind < 90 && !m.textures.empty()) {
			// Holder release: the bindless slot must stay held until the
			// destroy's margin key drains.
			op = "texDestroy";
			const size_t slot = size_t(rng() % m.textures.size());
			const uint32_t bindlessSlot = m.texSlotsLive[slot];
			m.textures.erase(m.textures.begin() + slot);
			m.texSlotsLive.erase(m.texSlotsLive.begin() + slot);
			m.texSlotsRetiring.push_back({ bindlessSlot, 1, retireKey() });
		} else if (kind < 92) {
			op = "flushUploads";
			vctx->FlushUploads();
			vctx->WaitForUploads();
		} else if (!m.entries.empty()) {
			// Shaping fallback: extra destroy when no other op applied.
			op = "destroyMesh*";
			destroyEntry(size_t(rng() % m.entries.size()));
		} else {
			op = "uploadSingle*";
			smvk::StaticMeshUpload up;
			fillUpload(up, randVerts(), randIdx());
			addMesh(cluster->UploadStaticMesh(up),
				uint32_t(up.vertices.size()),
				uint32_t(up.indices.size()));
		}

		CheckBlocks(iter, op);
	}

	// ---- final drain: everything must return to the pool ----
	const char* op = "finalDrain";
	while (!m.entries.empty())
		destroyEntry(0);
	m.textures.clear(); // Holder dtors -> the deferred bindless retire chain
	for (uint32_t s : m.texSlotsLive)
		m.texSlotsRetiring.push_back({ s, 1, retireKey() });
	m.texSlotsLive.clear();

	// Complete the timeline past every margin key + drain: nothing may
	// remain (the last destroy keyed at published + smvk::limits::MaxFramesInFlight).
	{
		VkSemaphoreSignalInfo si{
			VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr,
			gpuTimeline, m.published + smvk::limits::MaxFramesInFlight };
		if (vkSignalSemaphore(dev, &si) != VK_SUCCESS)
			Fail("finalDrain", iters, op, "final signal failed");
		m.signaled = m.published + smvk::limits::MaxFramesInFlight;
		vctx->ProcessDeferrredTasks();
		m.processed = m.signaled;
		m.PruneDrained();
	}
	CheckBlocks(iters, op);

	smvk::StaticHeapArena::Statistics vs{}, is{};
	vs = smvk::staticVertexFmtHeap.m_staticVertexBlock.Stats();
	is = smvk::staticVertexFmtHeap.m_staticIndexBlock.Stats();
	if (vs.allocationCount != 0 || vs.allocationBytes != 0
		|| is.allocationCount != 0 || is.allocationBytes != 0) {
		char d[160];
		std::snprintf(d, sizeof d,
			"vertex %u/%llu, index %u/%llu remain after full drain",
			vs.allocationCount, (unsigned long long)vs.allocationBytes,
			is.allocationCount, (unsigned long long)is.allocationBytes);
		Fail("full reclaim", iters, op, d);
	}
	++g_checks; // full-reclaim check

	vkDestroySemaphore(dev, gpuTimeline, nullptr);

	std::printf("[fuzz] checks=%d failures=0\n", g_checks);
	std::printf("PASS retirement_fuzz (seed %llu, %u iterations)\n",
		(unsigned long long)g_seed, iters);
	// rm dies on scope exit (before window: the core's surface teardown
	// needs the live window), then the window + SDL.
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
