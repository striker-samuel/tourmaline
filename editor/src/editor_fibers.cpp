// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor fibers backend implementation (see editor_fibers.h).
//
// THE RULE (hard-won): the MAIN thread NEVER touches boost::fibers —
// `use_scheduling_algorithm<work_stealing>` on the main thread HANGS
// IN THIS PROCESS (SDL/Streamline/Vulkan-loaded editor: [FIB] Start
// trace never returned; isolated processes are fine). Redesign:
//   - Post() is PURE std on the main thread: push {job, name,
//     onMain} into a mutex-guarded queue. Zero boost calls on main.
//   - Each WORKER owns its scheduler (default round_robin — created
//     lazily on the worker's first fiber op; no algorithm installs,
//     no cross-thread registry). The worker's MAIN CONTEXT loop:
//     drain the queue (non-blocking), spawn each job as a detached
//     fiber, then park the main fiber in sleep_for(2ms) — while the
//     main fiber sleeps, the worker's scheduler RUNS the ready job
//     fibers; it wakes every 2ms to drain more.
//   - Completions: the job fiber parks its record in a mutex-guarded
//     deque; Pump() drains it on the main thread (plain std).
// Concurrency = the OS pool (2 workers); fiber interleaving arrives
// later if jobs adopt fiber-aware primitives ON the workers.

#include "editor_fibers.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/fiber/all.hpp>

namespace editor {

struct FiberBackend::Impl {
	std::atomic<bool> running{ false };
	std::atomic<unsigned> inFlight{ 0 };
	std::vector<std::thread> workers;

	// Submission queue (main -> workers). Plain std mutex deque: the
	// main thread pushes (no boost), workers drain non-blocking.
	struct Submission {
		std::string name;
		Job job;
		OnComplete onMain;
	};
	std::mutex submitMx;
	std::deque<Submission> submit;

	// Completions (fiber -> main). Plain std mutex deque (nobody
	// blocks on it: the main thread polls via Pump()).
	struct Record {
		std::string name;
		bool ok = false;
		std::string error;
		OnComplete onMain;
	};
	std::mutex doneMx;
	std::deque<Record> done;

	// Fiber body: run the job under try/catch, park the completion.
	static void runJob(Impl* impl, Submission sub) {
		Record rec;
		rec.name = sub.name;
		try {
			sub.job();
			rec.ok = true;
		} catch (const std::exception& e) {
			rec.ok = false;
			rec.error = e.what();
		} catch (...) {
			rec.ok = false;
			rec.error = "unknown job error";
		}
		rec.onMain = std::move(sub.onMain);
		{
			std::lock_guard<std::mutex> lk(impl->doneMx);
			impl->done.push_back(std::move(rec));
		}
		impl->inFlight.fetch_sub(1);
	}

	// Job fiber stack: the default fiber stack is SMALL and boost's
	// fixedsize_stack carries NO guard page — a deep job (the cook's
	// tour build: ApplySceneReferences -> glTF import -> tinygltf
	// parser) silently overruns into ADJACENT HEAP (the user crash:
	// corrupted std::strings in the job lambda's captures freed at the
	// job epilogue, AV read at 0xFFFFFFFFFFFFFFFF in
	// std::string::_Tidy_deallocate). The old std::async workers ran
	// on full OS-thread stacks and never saw this. 8 MiB virtual
	// reservation, committed on use; the PROTECTED variant adds a
	// guard page so a future overflow faults LOUD at the guard
	// instead of corrupting the heap.
	// TOURMALINE_FIBER_JOB_STACK=<bytes> overrides (floor 64 KiB).
	// The bisect that found this bug cooked powers of two: 64 KiB
	// crashed, 128 KiB ran clean — the 8 MiB default has ~64x headroom.
	static std::size_t jobFiberStack() {
		static const std::size_t v = []() {
			const char* e = std::getenv("TOURMALINE_FIBER_JOB_STACK");
			const std::size_t def = 8u * 1024u * 1024u;
			if (e && *e) {
				const unsigned long long n =
					std::strtoull(e, nullptr, 0);
				if (n >= 64u * 1024u)
					return std::size_t(n);
			}
			return def;
		}();
		return v;
	}

	// Worker main-context loop: drain + spawn + park. While the main
	// fiber sleeps, this worker's scheduler runs the job fibers.
	void workerLoop() {
		while (running.load(std::memory_order_relaxed)
			|| inFlight.load(std::memory_order_relaxed) != 0u) {
			std::deque<Submission> taken;
			{
				std::lock_guard<std::mutex> lk(submitMx);
				taken.swap(submit);
			}
			while (!taken.empty()) {
				Submission sub = std::move(taken.front());
				taken.pop_front();
				boost::fibers::fiber(std::allocator_arg,
					boost::fibers::protected_fixedsize_stack(
						jobFiberStack()),
					&Impl::runJob, this, std::move(sub)).detach();
			}
			// Park THIS fiber (fiber-aware): ready job fibers on this
			// worker run while we sleep; wakes every 2ms to drain.
			boost::this_fiber::sleep_for(std::chrono::milliseconds(2));
		}
	}
};

FiberBackend::FiberBackend() : impl_(std::make_unique<Impl>()) {}

FiberBackend::~FiberBackend() {
	if (impl_ && impl_->running.load())
		Stop();
}

bool FiberBackend::Start(unsigned workers) {
	if (impl_->running.load())
		return true; // idempotent
	if (workers == 0)
		workers = 1;
	impl_->running.store(true);
	for (unsigned i = 0; i < workers; ++i)
		impl_->workers.emplace_back([this]() { impl_->workerLoop(); });
	return true;
}

void FiberBackend::Stop() {
	if (!impl_->running.exchange(false))
		return;
	// Workers exit once running==false AND in-flight jobs drained (the
	// parking loop condition covers both). Completions still in the
	// queue are dropped — call Pump() first if they matter.
	for (std::thread& t : impl_->workers)
		if (t.joinable())
			t.join();
	impl_->workers.clear();
}

bool FiberBackend::Post(const std::string& name, Job job,
	OnComplete onMain) {
	if (!impl_->running.load() || !job)
		return false;
	// PURE std on the calling (main) thread — the no-boost-on-main
	// rule (see the file header). The worker spawns the fiber.
	impl_->inFlight.fetch_add(1);
	{
		std::lock_guard<std::mutex> lk(impl_->submitMx);
		impl_->submit.push_back(
			Impl::Submission{ name, std::move(job), std::move(onMain) });
	}
	return true;
}

void FiberBackend::Pump() {
	std::deque<Impl::Record> ready;
	{
		std::lock_guard<std::mutex> lk(impl_->doneMx);
		ready.swap(impl_->done);
	}
	for (Impl::Record& rec : ready) {
		if (rec.onMain) {
			Completion c;
			c.name = rec.name;
			c.ok = rec.ok;
			c.error = rec.error;
			rec.onMain(c);
		}
	}
}

unsigned FiberBackend::InFlight() const {
	return impl_->inFlight.load(std::memory_order_relaxed);
}

} // namespace editor
