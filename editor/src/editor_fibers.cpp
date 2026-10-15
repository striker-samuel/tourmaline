// Editor fibers backend implementation (see editor_fibers.h).
//
// Layout: the main thread + N worker threads each install the
// work_stealing algorithm over the SHARED thread count. Post() spawns
// the fiber on the MAIN thread's scheduler (main never yields, so the
// fiber just sits ready); idle workers steal it and run it. The fiber
// body wraps the job in try/catch, then parks the completion record in
// a mutex-guarded deque (no fiber condvars anywhere: nobody blocks on
// completions — the main thread polls via Pump()).

#include "editor_fibers.h"

#include <atomic>
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

	// Completions (fiber -> main). Plain mutex deque: pushes from
	// worker fibers, pops on the main thread — no fiber semantics
	// needed (nobody blocks on it).
	struct Record {
		std::string name;
		bool ok = false;
		std::string error;
		OnComplete onMain;
	};
	std::mutex doneMx;
	std::deque<Record> done;

	// The main thread's scheduler installs this on Start so spawned
	// fibers land in a steal-able ready queue (work_stealing shares
	// ready fibers across the pool; the default per-thread algorithm
	// does not).
	void installAlgo(unsigned count) {
		boost::fibers::use_scheduling_algorithm<
			boost::fibers::algo::work_stealing>(count);
	}

	void workerLoop(unsigned count) {
		// The pool count must match on every participating thread.
		installAlgo(count);
		// Parking loop: sleeping lets the scheduler idle/steal; the
		// loop also keeps the thread alive draining jobs until Stop.
		while (running.load(std::memory_order_relaxed)
			|| inFlight.load(std::memory_order_relaxed) != 0u)
			boost::this_fiber::sleep_for(std::chrono::milliseconds(2));
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
	// The MAIN scheduler joins the pool with the same count: fibers
	// Post()ed here become steal-able by the workers.
	impl_->installAlgo(workers + 1);
	impl_->running.store(true);
	for (unsigned i = 0; i < workers; ++i)
		impl_->workers.emplace_back(
			[this, workers]() { impl_->workerLoop(workers + 1); });
	return true;
}

void FiberBackend::Stop() {
	if (!impl_->running.exchange(false))
		return;
	// Workers exit once running==false AND in-flight jobs drained
	// (the parking loop condition covers both). Completions still in
	// the queue are dropped — call Pump() first if they matter.
	for (std::thread& t : impl_->workers)
		if (t.joinable())
			t.join();
	impl_->workers.clear();
}

bool FiberBackend::Post(const std::string& name, Job job,
	OnComplete onMain) {
	if (!impl_->running.load() || !job)
		return false;
	impl_->inFlight.fetch_add(1);
	// Spawned on the CALLING (main) thread's scheduler: ready
	// immediately, stolen by an idle worker. Named local + detach
	// (boost::fibers::fiber is RAII — detach hands the state to the
	// scheduler before the local dies).
	boost::fibers::fiber f([this, name, job = std::move(job),
			onMain = std::move(onMain)]() mutable {
		Impl::Record rec;
		rec.name = name;
		try {
			job();
			rec.ok = true;
		} catch (const std::exception& e) {
			rec.ok = false;
			rec.error = e.what();
		} catch (...) {
			rec.ok = false;
			rec.error = "unknown job error";
		}
		rec.onMain = std::move(onMain);
		{
			std::lock_guard<std::mutex> lk(impl_->doneMx);
			impl_->done.push_back(std::move(rec));
		}
		impl_->inFlight.fetch_sub(1);
	});
	f.detach();
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
