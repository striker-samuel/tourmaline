// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// EDITOR-ONLY job backend for the editor's synchronous work (the class
// keeps its historical FiberBackend name from the Boost.Fibers era —
// see editor_fibers.cpp's HISTORY note for why the fibers were refactored
// out to plain OS worker threads: the jobs never yielded, and the fiber
// stacks were the stack-overflow crash).
//
// One unified lane for every off-main-thread job the editor runs (the
// cook/bake workers today; autosave/snapshot/volume loads later). Jobs
// run as plain functions on a small pool of OS worker threads (a bounded,
// owned pool — no stray detached threads at shutdown: Stop() drains
// in-flight jobs before joining). Completions land in a mutex-guarded
// queue drained by Pump() ON THE MAIN THREAD each frame — callbacks
// (result logging, task-window finish, bake-asset adoption) run where the
// UI lives, exactly like the old future.wait_for branches.
//
// The main thread only ever touches plain std (Post pushes into a mutex
// queue, Pump drains a mutex deque) — the fibers-era "no backend calls on
// main" rule, now trivially true with nothing but std left.
//
// Not job-reentrant yet: the tool line sink stays one-instance (guards:
// m_cookRunning/m_bakeRunning) and CookFeed keeps std::mutex (jobs block
// their whole worker thread).

#include <functional>
#include <memory>
#include <string>

namespace editor {

class FiberBackend {
public:
	// One completed job (delivered on the main thread by Pump).
	struct Completion {
		std::string name;   // job label (console/status)
		bool ok = false;
		std::string error;   // exception text when the job threw
	};
	using Job = std::function<void()>;
	// Runs ON THE MAIN THREAD inside Pump() when the job finishes.
	using OnComplete = std::function<void(const Completion&)>;

	FiberBackend();
	~FiberBackend(); // Stop() (must be called on the main thread first)

	// Spawns the worker pool (idempotent). Must run on the main thread.
	bool Start(unsigned workers = 2);
	// Marks shutdown, waits for in-flight jobs to finish, joins the
	// workers. Completions still queued are dropped (shutdown order is
	// the caller's business: stop BEFORE tearing anything a completion
	// callback would touch). Must run on the main thread.
	void Stop();
	// Posts a job (runs on a worker fiber; exceptions are captured as
	// Completion.error). Safe to call any thread; jobs should only be
	// posted from the main thread (the completion must target live
	// members). Returns false before Start/after Stop.
	bool Post(const std::string& name, Job job,
		OnComplete onMain = nullptr);
	// Main-thread pump: drains completions, runs their callbacks HERE.
	void Pump();
	// Job counters (status readouts).
	unsigned InFlight() const;
	bool Busy() const { return InFlight() != 0; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace editor
