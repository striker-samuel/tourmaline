// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include <Engine/Memory/pool.h>
#include <Engine/Memory/memory.h>

namespace smvk {

// Minimal fixed-size thread pool for CPU-bound asset work
// (KTX file reads, Basis transcoding, mesh processing).
// Header-only so both engine and samples can use it without
// adding a new translation unit.
//
// Task queue: bounded ring of task slots pre-allocated in PROCESS
// linear memory (capacity = mem::config::kThreadPoolQueueSlots, the single
// place to resize). Process, not transient: slots live for the pool's whole
// lifetime, and any transient reset while the pool lives would be a UAF
// ring queue (the transient allocator is UploadInFlight-monotonic — see
// Engine/Memory/memory.h + Vulkan/detail/vulkan_impl.h). Steady state
// performs ZERO container mallocs — std::queue's deque chunk churn is
// gone. The per-task packaged_task shared state still mallocs (std::future
// internals; removing that needs custom futures — explicitly out of scope).
//
// Backpressure contract: enqueue BLOCKS when the ring is full instead of
// growing. Never enqueue from a pool worker task — workers drain the ring,
// so a full ring with every worker blocked in enqueue would deadlock. All
// current call sites enqueue from outer loops and join afterwards.
//
// Multithread model: one local shard per thread. The constructor
// PRE-LEASES a unique pool shard per worker (see Memory/pool.h) and hands
// each worker its shard; a worker releases its lease on exit. The main
// thread keeps shard 0. ShardedPool::create() then allocates lock-free in
// the calling thread's shard and UploadStreamer records into the calling
// thread's staging ring – no pool/streamer locks on the hot path. When
// every shard is leased (another pool's workers are live), the
// constructor spawns FEWER workers rather than ever sharing a shard.
// Callers must join (or wait on the returned futures) before touching
// another thread's handles on the render thread; that join is the
// happens-before edge.
class ThreadPool {
public:
	explicit ThreadPool(size_t threadCount = 0)
			: capacity_(RingSlotCount()) {
		if (threadCount == 0) {
			threadCount = std::thread::hardware_concurrency();
			if (threadCount == 0) threadCount = 4;
		}
		// Cap to avoid oversubscription on massive-content scenes;
		// KTX transcode is memory-bandwidth heavy past ~16 threads.
		// Also capped so workers + main thread fit in kNumPoolShards
		// unique shards (main owns shard 0).
		if (threadCount > 16) threadCount = 16;
		if (threadCount > kNumPoolShards - 1) threadCount = kNumPoolShards - 1;
		// Lease the shards BEFORE spawning (on the constructing thread):
		// uniqueness is guaranteed by the CAS lease registry, and a
		// scarcity (concurrent pools already holding shards) shrinks the
		// worker count instead of ever handing two live threads the same
		// shard — the old modulo counter could wrap onto shard 0 or a
		// live worker's shard after enough sequential pools.
		std::vector<uint32_t> leases;
		leases.reserve(threadCount);
		for (size_t i = 0; i < threadCount; ++i) {
			const uint32_t shard = AcquirePoolShardLease();
			if (shard == kNoShardLease)
				break; // every shard leased: run with the workers we have
			leases.push_back(shard);
		}
		threadCount = leases.size();
		// Ring storage from the process allocator (allocation failure
		// crashes with the OOM diagnostic — no fallback). Slots are
		// default-constructed (empty std::function: no allocation) and
		// torn down explicitly; the storage itself is never freed.
		slots_ = static_cast<std::function<void()>*>(mem::ProcessAllocator().Allocate(
			capacity_ * sizeof(std::function<void()>), alignof(std::function<void()>)));
		for (size_t i = 0; i < capacity_; ++i)
			new (&slots_[i]) std::function<void()>();
		workers_.reserve(threadCount);
		size_t spawned = 0;
		try {
			for (; spawned < threadCount; ++spawned) {
				const uint32_t shard = leases[spawned];
				workers_.emplace_back([this, shard] {
					// This worker owns THIS shard for its lifetime (leased
					// by the constructor); release it on exit so later
					// pools can lease it.
					SetCurrentPoolShard(shard);
					for (;;) {
						std::function<void()> task;
						{
							std::unique_lock<std::mutex> lock(mutex_);
							cvEmpty_.wait(lock, [this] { return stop_ || count_ > 0; });
							if (stop_ && count_ == 0) {
								ReleasePoolShardLease(shard);
								return;
							}
							task = std::move(slots_[head_]);
							slots_[head_] = nullptr;
							head_ = (head_ + 1) % capacity_;
							--count_;
						}
						cvFull_.notify_one();
						task();
					}
				});
			}
		}
		catch (...) {
			// Spawn failure (resource exhaustion): the workers already
			// running release their own leases on the stop path below;
			// the leases for workers that never spawned must go back
			// here, then rethrow.
			{
				std::lock_guard<std::mutex> lock(mutex_);
				stop_ = true;
			}
			cvEmpty_.notify_all();
			cvFull_.notify_all();
			for (auto& t : workers_) if (t.joinable()) t.join();
			for (size_t i = spawned; i < leases.size(); ++i)
				ReleasePoolShardLease(leases[i]);
			for (size_t i = 0; i < capacity_; ++i)
				slots_[i].~function();
			throw;
		}
	}

	~ThreadPool() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		cvEmpty_.notify_all();
		cvFull_.notify_all();
		// Workers release their own shard leases on the exit path above.
		for (auto& t : workers_) if (t.joinable()) t.join();
		for (size_t i = 0; i < capacity_; ++i)
			slots_[i].~function();
		// Linear-owned storage: destructors run, memory never freed.
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	size_t size() const noexcept { return workers_.size(); }

	template <typename F>
	auto enqueue(F&& f) -> std::future<decltype(f())> {
		using R = decltype(f());
		auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
		std::future<R> fut = task->get_future();
		std::function<void()> fn([task] { (*task)(); });
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (stop_) throw std::runtime_error("ThreadPool is stopped");
			cvFull_.wait(lock, [this] { return stop_ || count_ < capacity_; });
			if (stop_) throw std::runtime_error("ThreadPool is stopped");
			slots_[tail_] = std::move(fn);
			tail_ = (tail_ + 1) % capacity_;
			++count_;
		}
		cvEmpty_.notify_one();
		return fut;
	}

private:
	// The ring slot count with OVERFLOW-GUARDED byte math: a huge
	// TOURMALINE_MEM_THREAD_SLOTS used to wrap `capacity * sizeof(slot)`
	// and under-allocate the ring — every enqueue past the wrap then wrote
	// out of bounds. The config parse rejects counts above
	// kMaxThreadPoolQueueSlots; this is the belt-and-braces check at the
	// allocation site (checked multiply against SIZE_MAX, fall back to the
	// compile-time default loudly — same fallback policy as the env
	// parse).
	static size_t RingSlotCount() {
		size_t slots = mem::config::Runtime().threadPoolQueueSlots;
		if (slots == 0
				|| slots > (std::numeric_limits<size_t>::max)()
					/ sizeof(std::function<void()>)) {
			std::fprintf(stderr,
				"[ThreadPool] threadPoolQueueSlots=%zu overflows the ring"
				" allocation (%zu-byte slots); using default %zu\n",
				slots, sizeof(std::function<void()>),
				size_t(mem::config::kThreadPoolQueueSlots));
			slots = mem::config::kThreadPoolQueueSlots;
		}
		return slots;
	}

	const size_t capacity_;
	std::vector<std::thread> workers_;
	std::function<void()>* slots_ = nullptr;
	size_t head_ = 0;  // next slot to consume (guarded by mutex_)
	size_t tail_ = 0;  // next slot to fill (guarded by mutex_)
	size_t count_ = 0; // occupied slots (guarded by mutex_)
	std::mutex mutex_;
	std::condition_variable cvEmpty_; // signaled when count_ grows
	std::condition_variable cvFull_;  // signaled when count_ shrinks
	bool stop_ = false;
};

} // namespace smvk
