#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Engine/Memory/pool.h>
#include <Engine/smvk.h>

namespace smvk {

class ResourceManager;

// One texture file to read + transcode + upload. `key` is the caller's
// dedup id (the glTF importer passes imageIndex); `path` is the resolved
// filesystem path to a .ktx2 file.
struct KtxLoadJob {
	int key = -1;
	std::string path;
};

struct ParallelTextureStats {
	size_t requested = 0;
	size_t succeeded = 0;
	size_t failed = 0;
	uint64_t uploadedBytes = 0;
	double seconds = 0.0;
};

// Decode + transcode + Vulkan upload for a single KTX2 file on the calling
// thread. Returns an empty holder on failure (error already logged).
// Shared core used by the parallel batch below and by the sequential
// fallback in the glTF importer.
Holder<TextureHandle> LoadKtxTextureFile(
	ResourceManager& ctx,
	const std::string& fullPath,
	const ImageCreateInfo& imageCI = {});

// Same as LoadKtxTextureFile but decodes from already-loaded bytes
// (pak-embedded payloads, VFS reads). debugName is used for the Vulkan
// object label and log lines. Returns an empty holder on failure.
Holder<TextureHandle> LoadKtxTextureMemory(
	ResourceManager& ctx,
	const void* data,
	size_t dataSize,
	const std::string& debugName,
	const ImageCreateInfo& imageCI = {});

// Multicore read + upload: each job's file IO, Basis transcode and Vulkan
// image creation/upload runs on a worker thread; the returned holders are
// collected on the calling thread. Failed jobs are skipped (logged) and
// simply absent from the result map.
//
// Thread-safety: one local shard per thread (see Memory/pool.h). Each
// ThreadPool worker owns a unique pool shard and a private upload ring, so
// concurrent CreateTexture calls allocate lock-free with no pool/streamer
// locks – only queue submit, the sampler cache and deferred-task pushes
// take brief locks. VMA/vkCreate* are thread-safe for distinct objects.
// The caller must join all jobs (the future .get() loop below does) before
// using the handles on its own thread, and flush uploads on its own thread.
std::unordered_map<int, Holder<TextureHandle>> LoadKtxTexturesParallel(
	ResourceManager& ctx,
	const std::vector<KtxLoadJob>& jobs,
	const ImageCreateInfo& imageCI = {},
	ParallelTextureStats* outStats = nullptr,
	size_t maxThreads = 0);

// One in-memory payload to decode + upload. key is the caller's dedup id.
struct KtxMemoryJob {
	int key = -1;
	const void* data = nullptr;
	size_t dataSize = 0;
	std::string debugName;
};

// Parallel version of LoadKtxTextureMemory over a batch of payloads.
// The payload memory must outlive the call (_UPLOAD joins all workers_
// before returning). Same threading contract as LoadKtxTexturesParallel.
std::unordered_map<int, Holder<TextureHandle>> LoadKtxTexturesParallelMemory(
	ResourceManager& ctx,
	const std::vector<KtxMemoryJob>& jobs,
	const ImageCreateInfo& imageCI = {},
	ParallelTextureStats* outStats = nullptr,
	size_t maxThreads = 0);

} // namespace smvk
