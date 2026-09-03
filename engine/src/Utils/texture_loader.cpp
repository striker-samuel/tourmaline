#include <Engine/Utils/texture_loader.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/RHI/resource_manager.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

#include "ktx.h"

namespace smvk {
namespace {

// Destroys the KTX object even when CreateTexture throws.
struct KtxGuard {
	ktxTexture2* tex = nullptr;
	explicit KtxGuard(ktxTexture2* t) : tex(t) {}
	~KtxGuard() { if (tex) ktxTexture2_Destroy(tex); }
	KtxGuard(const KtxGuard&) = delete;
	KtxGuard& operator=(const KtxGuard&) = delete;
};

} // namespace

Holder<TextureHandle> LoadKtxTextureFile(
	ResourceManager& ctx,
	const std::string& fullPath,
	const ImageCreateInfo& imageCI)
{
	ktxTexture2* ktxTex = nullptr;
	const KTX_error_code loadRes = ktxTexture2_CreateFromNamedFile(
		fullPath.c_str(),
		KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
		&ktxTex);
	if (loadRes != KTX_SUCCESS) {
		std::cerr << "Failed to load KTX texture: " << fullPath
			<< " code " << loadRes << std::endl;
		return {};
	}
	KtxGuard guard(ktxTex);

	try {
		ImageCreateInfo ci = imageCI;
		ci.hostTransfer = true;
		// debugName must outlive the call – fullPath is alive here.
		return ctx.CreateTexture(*ktxTex, fullPath.c_str(), ci);
	}
	catch (const std::exception& e) {
		std::cerr << "Failed to upload KTX texture: " << fullPath
			<< " : " << e.what() << std::endl;
		return {};
	}
}

std::unordered_map<int, Holder<TextureHandle>> LoadKtxTexturesParallel(
	ResourceManager& ctx,
	const std::vector<KtxLoadJob>& jobs,
	const ImageCreateInfo& imageCI,
	ParallelTextureStats* outStats,
	size_t maxThreads)
{
	std::unordered_map<int, Holder<TextureHandle>> out;
	if (jobs.empty()) {
		if (outStats) *outStats = {};
		return out;
	}

	const auto t0 = std::chrono::steady_clock::now();

	size_t threadCount = maxThreads;
	if (threadCount == 0) {
		threadCount = std::thread::hardware_concurrency();
		if (threadCount == 0) threadCount = 4;
	}
	if (threadCount > jobs.size()) threadCount = jobs.size();

	ParallelTextureStats stats{};
	stats.requested = jobs.size();

	if (threadCount <= 1) {
		// Degenerate case – stay sequential, same code path as one worker.
		for (const auto& job : jobs) {
			auto holder = LoadKtxTextureFile(ctx, job.path, imageCI);
			if (holder.valid()) {
				++stats.succeeded;
				out.emplace(job.key, std::move(holder));
			}
			else {
				++stats.failed;
			}
		}
	}
	else {
		ThreadPool pool(threadCount);
		std::mutex cerrMutex; // keep per-texture error lines intact
		std::vector<std::future<std::pair<int, Holder<TextureHandle>>>> futures;
		futures.reserve(jobs.size());

		for (const auto& job : jobs) {
			futures.push_back(pool.enqueue([&, job] {
				// Full read + transcode + Vulkan upload on the worker.
				// stderr is shared – serialize the lines from LoadKtxTextureFile?
				// LoadKtxTextureFile already logs; interleaving is cosmetic,
				// so no lock needed for correctness.
				(void)cerrMutex;
				Holder<TextureHandle> holder = LoadKtxTextureFile(ctx, job.path, imageCI);
				return std::make_pair(job.key, std::move(holder));
			}));
		}

		for (auto& f : futures) {
			try {
				auto [key, holder] = f.get();
				if (holder.valid()) {
					++stats.succeeded;
					out.emplace(key, std::move(holder));
				}
				else {
					++stats.failed;
				}
			}
			catch (const std::exception& e) {
				std::lock_guard<std::mutex> lock(cerrMutex);
				std::cerr << "Texture worker failed: " << e.what() << std::endl;
				++stats.failed;
			}
		}
	}

	const auto t1 = std::chrono::steady_clock::now();
	stats.seconds =
		std::chrono::duration<double>(t1 - t0).count();

	if (outStats) *outStats = stats;
	return out;
}

} // namespace smvk
