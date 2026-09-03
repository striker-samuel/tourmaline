#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace smvk {

// Minimal fixed-size thread pool for CPU-bound asset work
// (KTX file reads, Basis transcoding, mesh processing).
// Header-only so both engine and samples can use it without
// adding a new translation unit.
class ThreadPool {
public:
	explicit ThreadPool(size_t threadCount = 0) {
		if (threadCount == 0) {
			threadCount = std::thread::hardware_concurrency();
			if (threadCount == 0) threadCount = 4;
		}
		// Cap to avoid oversubscription on massive-content scenes;
		// KTX transcode is memory-bandwidth heavy past ~16 threads.
		if (threadCount > 16) threadCount = 16;
		workers_.reserve(threadCount);
		for (size_t i = 0; i < threadCount; ++i) {
			workers_.emplace_back([this] {
				for (;;) {
					std::function<void()> task;
					{
						std::unique_lock<std::mutex> lock(mutex_);
						cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
						if (stop_ && tasks_.empty()) return;
						task = std::move(tasks_.front());
						tasks_.pop();
					}
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
		cv_.notify_all();
		for (auto& t : workers_) if (t.joinable()) t.join();
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	size_t size() const noexcept { return workers_.size(); }

	template <typename F>
	auto enqueue(F&& f) -> std::future<decltype(f())> {
		using R = decltype(f());
		auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
		std::future<R> fut = task->get_future();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (stop_) throw std::runtime_error("ThreadPool is stopped");
			tasks_.emplace([task] { (*task)(); });
		}
		cv_.notify_one();
		return fut;
	}

private:
	std::vector<std::thread> workers_;
	std::queue<std::function<void()>> tasks_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_ = false;
};

} // namespace smvk
