// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Centralized default allocation-chunk sizes (Phase 0 infrastructure).
//
// SINGLE PLACE TO RESIZE: every default chunk size in the linear-allocator
// migration lives here as a constexpr. To retune memory usage at COMPILE
// time, edit the values below — no other file hard-codes a default size.
//
// To retune at PROGRAM ENTRY without a rebuild, set the TOURMALINE_MEM_*
// environment variables listed on RuntimeConfig below; Runtime() parses
// them once (before the first allocator is constructed), validates them,
// logs each accepted override, and falls back to the constexpr defaults
// above on bad values. App and Editor both call InitMemory() first in
// their entry path, so the parse always precedes Vulkan/worker/asset
// startup. Hot-reloading the values after startup is deliberately not a
// feature: the static heaps and pools are sized for the process lifetime.
//
// NOT runtime-configurable (compile-time by necessity): the ShardedPool
// shard count/capacity (they are array bounds / static_asserts against
// the RHI bindless budget, smvk::limits::MaxTextures — the pool/
// descriptor integration assert lives in Vulkan/detail/vulkan_impl.h)
// and kDefaultAlign. Everything else below is.
//
// Lifetime model:
//   - Process   : reserved once at startup, never reset (VulkanCore,
//                 Submitter, AppArena blocks, resource pools).
//   - Frame     : bumped per frame, reset at frame start (FrameArena
//                 blocks, RenderGraph temporaries).
//   - Transient : bumped sub-frame, reset after GPU sync (staging rings,
//                 readback, ThreadPool payloads).
//
// OOM policy (global): crash + report. LinearAllocator calls
// mem::ReportOomAndCrash() on exhaustion — there are no survival,
// fallback, or compaction strategies by design.

#include <cstddef>

namespace smvk::mem::config {

inline constexpr size_t kKiB = size_t(1) << 10;
inline constexpr size_t kMiB = size_t(1) << 20;
inline constexpr size_t kGiB = size_t(1) << 30;

// -- Arena block sizes (engine/src/Memory/arena.cpp) -----------------------
// AppArena: process-lifetime strings/URIs. FrameArena: per-frame scratch.
// Both default to 1 MiB (matches the pre-migration Arena behavior).
inline constexpr size_t kAppArenaBlock = 1 * kMiB;
inline constexpr size_t kFrameArenaBlock = 1 * kMiB;

// -- Linear allocator virtual-address reservations --------------------------
// Reserved (not committed) at startup via VirtualAlloc MEM_RESERVE on
// Windows. Reservation costs address space only — physical pages commit
// on demand in kCommitQuantum chunks.
inline constexpr size_t kProcessReserve = 4 * kGiB;
inline constexpr size_t kFrameReserve = 256 * kMiB;
inline constexpr size_t kTransientReserve = 1 * kGiB;

// -- Static geometry heaps (GPU, Renderer/mesh.cpp) --------------------------
// Three eternal device-local buffers: two vertex heaps (position + shading)
// and one index monoblock. Allocated once in WorldRenderer::onAttach before
// any scene upload; the pak loader carves them via VmaVirtualBlock. This is
// 1.5 GiB of the process's VRAM by default — THE largest static GPU cost.
inline constexpr size_t kStaticVertexHeap = 512 * kMiB;
inline constexpr size_t kStaticIndexHeap = 512 * kMiB;

// -- Transient target heap blocks (GPU, rendergraph design-B targets) --------
// Graph-owned VkDeviceMemory blocks carve packed attachment ranges. A new
// block starts at this size and halves on allocation failure down to the
// request size (impl.cpp AllocateTargetHeapRange).
inline constexpr size_t kTargetHeapBlock = 64 * kMiB;

// -- Upload staging (UploadStreamer rings, VMA host-visible buffers) ---------
// Total streamer budget split across shards at init (init divides by 8 and
// clamps to the per-thread floor); shard rings lazily create on first
// upload so unused shards cost nothing. GPU-side VMA memory — the sizes
// live here so CPU and GPU budgets resize in one place.
inline constexpr size_t kStreamerTotalBudget = 256 * kMiB;
inline constexpr size_t kStagingRingPerThread = 32 * kMiB;
inline constexpr size_t kBulkRingCapacity = 512 * kMiB;
// Batch submit threshold: a shard submits its transfer batch once staged
// bytes since flush exceed this.
inline constexpr size_t kUploadFlushThreshold = 256 * kMiB;

// -- Readback staging (DownloadDataFromBuffer, VMA host-cached buffer) ------
// Grow-on-demand doubling from the floor; requests above the cap and VMA
// allocation failures crash (no graceful degradation — see OOM policy).
inline constexpr size_t kReadbackStagingMin = 256 * kKiB;
inline constexpr size_t kReadbackStagingMax = 256 * kMiB;

// -- Sharded resource pools (Engine/Memory/pool.h) --------------------------
// Lock-free sharded pools (images, textures, buffers, ...): one shard per
// thread, fixed entries per shard. Storage lives wherever the owning
// manager lives (process linear memory since Epoch 5) — these constants
// size the reservation need, so they live here.
inline constexpr uint32_t kNumPoolShards = 16;
inline constexpr uint32_t kShardCapacity = 512;
inline constexpr uint32_t kShardedMaxSize = kNumPoolShards * kShardCapacity; // 8192

// -- ThreadPool task ring ----------------------------------------------------
// Bounded task queue: ring slots pre-allocated in transient memory at pool
// construction. Enqueue blocks when full (backpressure — the queue never
// grows and never OOMs). Sized for worst-case fan-out (pak decompress +
// GI bakes enqueue in the low thousands); costs ~160 KB of transient VA.
inline constexpr size_t kThreadPoolQueueSlots = 4096;

// -- Page-commit granularity ------------------------------------------------
// Physical pages commit in multiples of this quantum as the bump pointer
// advances. Larger = fewer syscalls, coarser working set. Must be a
// multiple of the OS page size (4 KiB).
inline constexpr size_t kCommitQuantum = 16 * kMiB;

// -- Default bump alignment --------------------------------------------------
inline constexpr size_t kDefaultAlign = 16;

// ---------------------------------------------------------------------------
// Runtime sizes (program-entry configuration)
// ---------------------------------------------------------------------------
// Defaults mirror the constexpr values above; Runtime() applies the
// TOURMALINE_MEM_* environment overrides once at startup. Every field is in
// BYTES except threadPoolQueueSlots (a slot count). Env spellings:
//
//   TOURMALINE_MEM_PROCESS_MB           processReserve
//   TOURMALINE_MEM_FRAME_MB             frameReserve
//   TOURMALINE_MEM_TRANSIENT_MB         transientReserve
//   TOURMALINE_MEM_APP_ARENA_MB         appArenaBlock
//   TOURMALINE_MEM_FRAME_ARENA_MB       frameArenaBlock
//   TOURMALINE_MEM_COMMIT_MB            commitQuantum (power of two)
//   TOURMALINE_MEM_STREAMER_MB          streamerTotalBudget (GPU staging)
//   TOURMALINE_MEM_STAGING_RING_MB      stagingRingPerThread (GPU staging)
//   TOURMALINE_MEM_BULK_RING_MB         bulkRingCapacity (GPU staging)
//   TOURMALINE_MEM_FLUSH_MB             uploadFlushThreshold
//   TOURMALINE_MEM_READBACK_MIN_KB      readbackStagingMin (GPU readback)
//   TOURMALINE_MEM_READBACK_MAX_MB      readbackStagingMax (GPU readback)
//   TOURMALINE_MEM_THREAD_SLOTS         threadPoolQueueSlots
//   TOURMALINE_MEM_STATIC_VERTEX_MB     staticVertexHeap (GPU, each of 2)
//   TOURMALINE_MEM_STATIC_INDEX_MB      staticIndexHeap (GPU)
//   TOURMALINE_MEM_TARGET_BLOCK_MB      targetHeapBlock (GPU graph targets)
struct RuntimeConfig {
	size_t appArenaBlock = kAppArenaBlock;
	size_t frameArenaBlock = kFrameArenaBlock;

	size_t processReserve = kProcessReserve;
	size_t frameReserve = kFrameReserve;
	size_t transientReserve = kTransientReserve;

	size_t streamerTotalBudget = kStreamerTotalBudget;
	size_t stagingRingPerThread = kStagingRingPerThread;
	size_t bulkRingCapacity = kBulkRingCapacity;
	size_t uploadFlushThreshold = kUploadFlushThreshold;
	size_t readbackStagingMin = kReadbackStagingMin;
	size_t readbackStagingMax = kReadbackStagingMax;

	size_t threadPoolQueueSlots = kThreadPoolQueueSlots;

	size_t commitQuantum = kCommitQuantum;

	size_t staticVertexHeap = kStaticVertexHeap;
	size_t staticIndexHeap = kStaticIndexHeap;
	size_t targetHeapBlock = kTargetHeapBlock;
};

// The process-wide runtime sizes, parsed from TOURMALINE_MEM_* on first use
// (InitMemory calls it first thing; any earlier lazy use parses then).
// Never mutated after this function returns — callers hold a const ref.
const RuntimeConfig& Runtime();

} // namespace smvk::mem::config
