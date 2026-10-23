// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <condition_variable>
#include <functional>
#include <future>
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
// Multithread model: one local shard per thread. Each worker registers a
// unique pool shard on startup (see Memory/pool.h); the main thread keeps
// shard 0. ShardedPool::create() then allocates lock-free in the calling
// thread's shard and UploadStreamer records into the calling thread's
// staging ring – no pool/streamer locks on the hot path. Callers must join
// (or wait on the returned futures) before touching another thread's
// handles on the render thread; that join is the happens-before edge.
class ThreadPool {
public:
	explicit ThreadPool(size_t threadCount = 0) {
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
		// Ring storage from the process allocator (allocation failure
		// crashes with the OOM diagnostic — no fallback). Slots are
		// default-constructed (empty std::function: no allocation) and
		// torn down explicitly; the storage itself is never freed.
		slots_ = static_cast<std::function<void()>*>(mem::ProcessAllocator().Allocate(
			capacity_ * sizeof(std::function<void()>), alignof(std::function<void()>)));
		for (size_t i = 0; i < capacity_; ++i)
			new (&slots_[i]) std::function<void()>();
		workers_.reserve(threadCount);
		for (size_t i = 0; i < threadCount; ++i) {
			workers_.emplace_back([this] {
				// Own one pool/upload shard for lock-free resource creation.
				// Wraps past kNumPoolShards (then the caller must serialize
				// externally); the pool caps workers at 16 so each worker
				// normally owns a unique shard.
				RegisterPoolShard();
				for (;;) {
					std::function<void()> task;
					{
						std::unique_lock<std::mutex> lock(mutex_);
						cvEmpty_.wait(lock, [this] { return stop_ || count_ > 0; });
						if (stop_ && count_ == 0) return;
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

	~ThreadPool() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		cvEmpty_.notify_all();
		cvFull_.notify_all();
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
	const size_t capacity_ = mem::config::Runtime().threadPoolQueueSlots;
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
