// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor job backend implementation (see editor_fibers.h).
//
// HISTORY (why plain worker threads): this was a Boost.Fibers backend —
// each worker owned a round_robin scheduler, spawned every job as a
// detached fiber on a custom 8 MiB protected_fixedsize_stack and parked
// its main fiber in this_fiber::sleep_for(2ms) while the job fibers ran.
// Two hard-won lessons drove it back to OS threads:
//   1. Installing work_stealing on the MAIN thread hung the process
//      (SDL/Streamline/Vulkan-loaded editor; isolated processes were
//      fine) — the redesign had already quarantined every boost call
//      onto the workers, which left the fiber layer with ZERO concurrent
//      benefit: jobs are plain functions using std::mutex, so they never
//      yield anyway — the pool provided the concurrency all along (the
//      v1 header's own admission: "the pool, not the fiber, provides the
//      concurrency today").
//   2. The default fiber stack is SMALL and boost's fixedsize_stack
//      carries no guard page: a deep job (the cook's tour build ->
//      glTF import -> tinygltf parser) silently overran into ADJACENT
//      HEAP (corrupted std::string captures freed at the job epilogue,
//      AV read at 0xFFFFFFFFFFFFFFFF in std::string::_Tidy_deallocate).
//      The 8 MiB PROTECTED stack was the workaround; an OS-thread stack
//      has both (full size + a real guard page) by construction.
// So: each worker drains the submission queue and runs jobs DIRECTLY on
// its own thread stack, parking in std::this_thread::sleep_for(2ms)
// when idle. The public API (Post/Pump/Stop/InFlight) is unchanged — the
// editor call sites never knew about the fibers (the class keeps its
// historical FiberBackend name).
//
// THE RULE survives from the fiber era (now trivially true): the MAIN
// thread only ever touches plain std — Post() pushes {job, name, onMain}
// into a mutex-guarded queue; Pump() drains the completion deque and
// runs the callbacks ON the main thread.

#include "editor_fibers.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <Engine/Profile/profile.h>

namespace editor {

struct FiberBackend::Impl {
	std::atomic<bool> running{ false };
	std::atomic<unsigned> inFlight{ 0 };
	std::vector<std::thread> workers;

	// Submission queue (main -> workers). Mutex deque: the main thread
	// pushes, workers drain non-blocking.
	struct Submission {
		std::string name;
		Job job;
		OnComplete onMain;
	};
	std::mutex submitMx;
	std::deque<Submission> submit;

	// Completions (worker -> main). Mutex deque (nobody blocks on it:
	// the main thread polls via Pump()).
	struct Record {
		std::string name;
		bool ok = false;
		std::string error;
		OnComplete onMain;
	};
	std::mutex doneMx;
	std::deque<Record> done;

	// Job body — runs on the worker's own OS-thread stack (full size +
	// guard page; the fiber-era stack-overflow saga in the file header):
	// run the job under try/catch, park the completion record.
	static void runJob(Impl* impl, Submission sub) {
		// Optick: the JOB's OWN name as a dynamic scope — imports,
		// cooks and bakes each show under their submitted name on the
		// worker lane (the scope lives for the whole record build, i.e.
		// the job's execution).
		SMVK_PROFILE_SCOPE_DYNAMIC(sub.name.c_str());
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

	// Worker loop: drain the queue, run each job directly on this
	// thread, then park briefly before draining again. The 2 ms park is
	// a poll interval, not a scheduling yield — nothing else shares the
	// thread.
	void workerLoop() {
		// Optick: name the worker lane (unique per worker so the two
		// editor lanes stay distinguishable in the timeline; the string
		// outlives the ThreadScope registration).
		static std::atomic<unsigned> s_workerSeq{ 0u };
		const std::string threadName =
			"EditorWorker" + std::to_string(s_workerSeq.fetch_add(1u));
		SMVK_PROFILE_THREAD(threadName.c_str());
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
				runJob(this, std::move(sub));
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
	// loop condition covers both). Completions still in the queue are
	// dropped — call Pump() first if they matter.
	for (std::thread& t : impl_->workers)
		if (t.joinable())
			t.join();
	impl_->workers.clear();
}

bool FiberBackend::Post(const std::string& name, Job job,
	OnComplete onMain) {
	if (!impl_->running.load() || !job)
		return false;
	// PURE std on the calling (main) thread — the no-backend-calls-on-
	// main rule (see the file header).
	impl_->inFlight.fetch_add(1);
	{
		std::lock_guard<std::mutex> lk(impl_->submitMx);
		impl_->submit.push_back(
			Impl::Submission{ name, std::move(job), std::move(onMain) });
	}
	return true;
}

void FiberBackend::Pump() {
	// Optick: the main-thread completion delivery — a completed
	// background job applying its result (cook/bake finish logic) is a
	// classic main-thread hitch source, and each callback carries the
	// job's own name as a dynamic scope.
	SMVK_PROFILE_SCOPE("Editor_FibersPump");
	std::deque<Impl::Record> ready;
	{
		std::lock_guard<std::mutex> lk(impl_->doneMx);
		ready.swap(impl_->done);
	}
	SMVK_PROFILE_TAG("completions", ready.size());
	for (Impl::Record& rec : ready) {
		if (rec.onMain) {
			Completion c;
			c.name = rec.name;
			c.ok = rec.ok;
			c.error = rec.error;
			SMVK_PROFILE_SCOPE_DYNAMIC(rec.name.c_str());
			rec.onMain(c);
		}
	}
}

unsigned FiberBackend::InFlight() const {
	return impl_->inFlight.load(std::memory_order_relaxed);
}

} // namespace editor
