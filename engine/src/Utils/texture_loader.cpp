#include <Engine/Utils/texture_loader.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/texture_streamer.h>

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

Holder<TextureHandle> LoadKtxTextureMemory(
	ResourceManager& ctx,
	const void* data,
	size_t dataSize,
	const std::string& debugName,
	const ImageCreateInfo& imageCI)
{
	if (data == nullptr || dataSize == 0) {
		std::cerr << "Failed to load KTX texture (empty payload): " << debugName << std::endl;
		return {};
	}
	ktxTexture2* ktxTex = nullptr;
	const KTX_error_code loadRes = ktxTexture2_CreateFromMemory(
		static_cast<const ktx_uint8_t*>(data),
		static_cast<ktx_size_t>(dataSize),
		KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
		&ktxTex);
	if (loadRes != KTX_SUCCESS) {
		std::cerr << "Failed to decode KTX texture: " << debugName
			<< " code " << loadRes << std::endl;
		return {};
	}
	KtxGuard guard(ktxTex);

	try {
		ImageCreateInfo ci = imageCI;
		ci.hostTransfer = true;
		return ctx.CreateTexture(*ktxTex, debugName.c_str(), ci);
	}
	catch (const std::exception& e) {
		std::cerr << "Failed to upload KTX texture: " << debugName
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

std::unordered_map<int, Holder<TextureHandle>> LoadKtxTexturesParallelMemory(
	ResourceManager& ctx,
	const std::vector<KtxMemoryJob>& jobs,
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

	// Payload memory must outlive the workers; every path below joins
	// before returning, so borrowing job pointers is safe.
	auto runOne = [&](const KtxMemoryJob& job) {
		return LoadKtxTextureMemory(ctx, job.data, job.dataSize, job.debugName, imageCI);
	};

	if (threadCount <= 1) {
		for (const auto& job : jobs) {
			auto holder = runOne(job);
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
		std::mutex cerrMutex;
		std::vector<std::future<std::pair<int, Holder<TextureHandle>>>> futures;
		futures.reserve(jobs.size());

		for (const auto& job : jobs) {
			futures.push_back(pool.enqueue([&, job] {
				(void)cerrMutex;
				Holder<TextureHandle> holder = runOne(job);
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

std::unordered_map<int, Holder<TextureHandle>> LoadStreamingTailTexturesParallel(
	ResourceManager& ctx,
	const std::vector<KtxTailJob>& jobs,
	TextureStreamer* streamer,
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

	// Payload memory must outlive the workers; every path below joins
	// before returning, so borrowing job pointers is safe. The payload
	// bytes themselves stay borrowed after return — the caller pins the
	// pak image for the streamer's demand upgrades.
	auto runOne = [&](const KtxTailJob& job) {
		if (job.data == nullptr || job.dataSize == 0) {
			std::cerr << "Failed to load digest texture (empty payload): "
				<< job.debugName << std::endl;
			return Holder<TextureHandle>{};
		}
		ktxTexture2* ktxTex = nullptr;
		const KTX_error_code loadRes = ktxTexture2_CreateFromMemory(
			static_cast<const ktx_uint8_t*>(job.data),
			static_cast<ktx_size_t>(job.dataSize),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
			&ktxTex);
		if (loadRes != KTX_SUCCESS || !ktxTex) {
			std::cerr << "Failed to decode digest texture: " << job.debugName
				<< " code " << loadRes << std::endl;
			return Holder<TextureHandle>{};
		}
		KtxGuard guard(ktxTex);

		try {
			ImageCreateInfo ci = imageCI;
			ci.hostTransfer = true;
			// Clamp a stale digest split into range (renders the coarsest
			// mip rather than dropping the texture).
			const uint32_t fullMips = std::max(1u, ktxTex->numLevels);
			const uint32_t firstMip = std::min(job.tailFirstMip, fullMips - 1);
			Holder<TextureHandle> holder = ctx.CreateStreamingTexture(
				*ktxTex, firstMip, fullMips - firstMip,
				job.debugName.c_str(), ci);
			if (holder.valid() && streamer) {
				const uint32_t baseW = std::max(1u, ktxTex->baseWidth);
				const uint32_t baseH = std::max(1u, ktxTex->baseHeight);
				const uint32_t layers = std::max(1u, ktxTex->numLayers)
					* std::max(1u, ktxTex->numFaces);
				streamer->RegisterExisting(holder, job.data, job.dataSize,
					job.debugName, fullMips, baseW, baseH, layers, firstMip);
			}
			return holder;
		}
		catch (const std::exception& e) {
			std::cerr << "Failed to upload digest texture: " << job.debugName
				<< " : " << e.what() << std::endl;
			return Holder<TextureHandle>{};
		}
	};

	if (threadCount <= 1) {
		for (const auto& job : jobs) {
			auto holder = runOne(job);
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
		std::mutex cerrMutex;
		std::vector<std::future<std::pair<int, Holder<TextureHandle>>>> futures;
		futures.reserve(jobs.size());

		for (const auto& job : jobs) {
			futures.push_back(pool.enqueue([&, job] {
				(void)cerrMutex;
				Holder<TextureHandle> holder = runOne(job);
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
				std::cerr << "Digest texture worker failed: " << e.what() << std::endl;
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
