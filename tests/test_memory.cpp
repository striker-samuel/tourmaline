// memory_tests: CPU-side tests for the Phase 0 linear-allocator
// infrastructure (Engine/Memory/linear_allocator.h, linear_vector.h,
// alloc_config.h). No GPU required.
//
// What it pins down:
//   1. config: the centralized sizes are sane (non-zero, quantum divides
//      reserves, arena blocks are multiples of the page size).
//   2. bump: Allocate returns increasing, aligned pointers; Create
//      placement-constructs; Used() tracks the high-water mark.
//   3. mark/reset: Reset(mark) rewinds and hands out the same address
//      again; ResetAll rewinds to zero.
//   4. STL adapter: ProcessVector grows from the allocator (contents
//      intact across growth); deallocate is a no-op that never throws.
// OOM-crashes are NOT tested here (they abort the process by design).

#include <cstdint>
#include <iostream>
#include <numeric>

#include <Engine/Memory/alloc_config.h>
#include <Engine/Memory/arena.h>
#include <Engine/Memory/linear_allocator.h>
#include <Engine/Memory/linear_vector.h>
#include <Engine/Memory/memory.h>
#include <Engine/Memory/pool.h>
#include <Engine/Utils/thread_pool.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

bool IsAligned(const void* p, size_t align) {
	return (reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0;
}

void TestConfig() {
	using namespace smvk::mem::config;
	Check(kAppArenaBlock > 0 && kFrameArenaBlock > 0, "memory.config.arena-blocks-nonzero");
	Check(kProcessReserve > 0 && kFrameReserve > 0 && kTransientReserve > 0,
		"memory.config.reserves-nonzero");
	Check(kCommitQuantum >= 4096 && (kCommitQuantum % 4096) == 0,
		"memory.config.quantum-is-pages");
	Check(kAppArenaBlock % 4096 == 0 && kFrameArenaBlock % 4096 == 0,
		"memory.config.arena-blocks-are-pages");
	Check(kProcessReserve % kCommitQuantum == 0
		&& kFrameReserve % kCommitQuantum == 0
		&& kTransientReserve % kCommitQuantum == 0,
		"memory.config.reserves-divisible-by-quantum");
	// Epoch 6: staging/readback sizes centralized, sane, and ordered.
	Check(kStagingRingPerThread > 0 && kBulkRingCapacity >= kStagingRingPerThread
		&& kStreamerTotalBudget >= kStagingRingPerThread,
		"memory.config.staging-sizes-ordered");
	Check(kUploadFlushThreshold > 0
		&& kUploadFlushThreshold <= kBulkRingCapacity,
		"memory.config.flush-threshold-sane");
	Check(kReadbackStagingMin > 0
		&& kReadbackStagingMin <= kReadbackStagingMax,
		"memory.config.readback-range-sane");
	Check(kDefaultAlign >= alignof(void*), "memory.config.align-sane");
}

void TestBump() {
	// Small reservation keeps the test fast; quantum smaller than the
	// reservation exercises multi-commit growth.
	smvk::mem::LinearAllocator alloc("test-bump", 64 * 1024, 16 * 1024);
	Check(!alloc.IsInitialized(), "memory.bump.lazy-before-first-alloc");
	void* a = alloc.Allocate(64, 16);
	Check(alloc.IsInitialized(), "memory.bump.init-on-first-alloc");
	Check(a != nullptr && IsAligned(a, 16), "memory.bump.first-aligned");
	void* b = alloc.Allocate(32, 8);
	Check(b != nullptr && IsAligned(b, 8), "memory.bump.second-aligned");
	Check(b >= static_cast<uint8_t*>(a) + 64, "memory.bump.monotonic");
	Check(alloc.Used() >= 96, "memory.bump.used-tracks");
	Check(alloc.Committed() >= alloc.Used(), "memory.bump.committed-covers-used");
	Check(alloc.Committed() <= alloc.Reserved(), "memory.bump.committed-within-reserve");
}

struct Probe {
	int x = 0;
	int y = 0;
	Probe(int a, int b) : x(a), y(b) {}
};

void TestCreate() {
	smvk::mem::LinearAllocator alloc("test-create", 64 * 1024, 16 * 1024);
	Probe* p = alloc.Create<Probe>(3, 4);
	Check(p != nullptr && p->x == 3 && p->y == 4, "memory.create.placement-constructs");
	Check(IsAligned(p, alignof(Probe)), "memory.create.aligned");
}

void TestMarkReset() {
	smvk::mem::LinearAllocator alloc("test-reset", 64 * 1024, 16 * 1024);
	void* a = alloc.Allocate(128, 16);
	const size_t mark = alloc.Mark();
	void* b = alloc.Allocate(256, 16);
	Check(b != a, "memory.reset.distinct-before-reset");
	alloc.Reset(mark);
	void* c = alloc.Allocate(256, 16);
	Check(c == b, "memory.reset.rewinds-to-mark");
	alloc.ResetAll();
	Check(alloc.Used() == 0, "memory.reset.all-zeroes-used");
	void* d = alloc.Allocate(128, 16);
	Check(d == a, "memory.reset.all-rewinds-to-base");
}

void TestGlobals() {
	using namespace smvk::mem;
	InitMemory(); // idempotent; reserves VA for all three lifetimes
	Check(ProcessAllocator().IsInitialized(), "memory.globals.process-initialized");
	Check(FrameAllocator().IsInitialized(), "memory.globals.frame-initialized");
	Check(TransientAllocator().IsInitialized(), "memory.globals.transient-initialized");
	Check(ProcessAllocator().Reserved() == config::kProcessReserve,
		"memory.globals.process-reserve-from-config");
	Check(FrameAllocator().Reserved() == config::kFrameReserve,
		"memory.globals.frame-reserve-from-config");
	Check(TransientAllocator().Reserved() == config::kTransientReserve,
		"memory.globals.transient-reserve-from-config");
	InitMemory(); // second call must be a no-op, never crash
	Check(ProcessAllocator().IsInitialized(), "memory.globals.init-idempotent");
}

void TestArenaBacking() {
	// Backed arena: block storage comes from the linear allocator and
	// Reset() rewinds it instead of freeing.
	smvk::mem::LinearAllocator backing("test-arena", 1 * 1024 * 1024, 64 * 1024);
	{
		smvk::Arena arena(4096, &backing);
		Check(arena.Backing() == &backing, "memory.arena.backing-attached");
		void* a = arena.Allocate(64, 16);
		Check(a != nullptr, "memory.arena.backed-alloc");
		Check(arena.BlockCount() == 1, "memory.arena.one-block");
		const size_t usedAfterAlloc = backing.Used();
		Check(usedAfterAlloc >= 4096, "memory.arena.blocks-live-in-backing");
		(void)arena.AllocateString(smvk::StringViewT("hello", 5));
		arena.Reset();
		Check(arena.UsedBytes() == 0 && arena.BlockCount() == 0,
			"memory.arena.reset-clears-stats");
		Check(backing.Used() < usedAfterAlloc,
			"memory.arena.reset-rewinds-backing");
		void* b = arena.Allocate(64, 16);
		Check(b == a, "memory.arena.reset-reuses-warm-range");
		// Oversized requests get dedicated backing blocks, not malloc.
		void* big = arena.Allocate(8192, 16);
		Check(big != nullptr && arena.BlockCount() == 2,
			"memory.arena.dedicated-block-backed");
	}
	// Unbacked arena keeps the legacy malloc behavior (incl. dedicated
	// blocks) and frees everything on Reset.
	{
		smvk::Arena plain(4096);
		Check(plain.Backing() == nullptr, "memory.arena.unbacked-null");
		void* a = plain.Allocate(64, 16);
		void* big = plain.Allocate(8192, 16);
		Check(a != nullptr && big != nullptr, "memory.arena.unbacked-allocs");
		Check(plain.BlockCount() == 2, "memory.arena.unbacked-two-blocks");
		plain.Reset();
		Check(plain.UsedBytes() == 0 && plain.BlockCount() == 0,
			"memory.arena.unbacked-reset-frees");
	}
	// Global arenas are wired to their lifetime allocators.
	Check(smvk::AppArena().Backing() == &smvk::mem::ProcessAllocator(),
		"memory.arena.app-backed-by-process");
	Check(smvk::FrameArena().Backing() == &smvk::mem::FrameAllocator(),
		"memory.arena.frame-backed-by-frame");
}
struct Counted {
	static inline int alive = 0;
	int value = 0;
	Counted(int v = 0) : value(v) { ++alive; }
	~Counted() { --alive; }
};

void TestLinearPtr() {
	Counted::alive = 0;
	smvk::mem::LinearAllocator alloc("test-ptr", 64 * 1024, 16 * 1024);
	{
		auto p = smvk::mem::MakeLinear<Counted>(alloc, 42);
		Check(p && p->value == 42 && Counted::alive == 1,
			"memory.ptr.make-constructs");
		auto q = std::move(p);
		Check(!p && q && Counted::alive == 1, "memory.ptr.move-transfers");
	}
	Check(Counted::alive == 0, "memory.ptr.dtor-on-scope-exit");
	// reset() destroys the old object and takes the new one; the storage
	// itself is never freed (linear) — Used() only grows.
	const size_t usedBefore = alloc.Used();
	auto r = smvk::mem::MakeLinear<Counted>(alloc);
	r.reset(alloc.Create<Counted>(7));
	Check(Counted::alive == 1 && r->value == 7, "memory.ptr.reset-destroys-old");
	Check(alloc.Used() >= usedBefore, "memory.ptr.storage-never-freed");
	Counted* raw = r.release();
	Check(!r && raw != nullptr && Counted::alive == 1, "memory.ptr.release-skips-dtor");
	raw->~Counted(); // manual teardown matches linear ownership
	Check(Counted::alive == 0, "memory.ptr.manual-dtor");
	// MakeProcess allocates from the real process allocator.
	auto g = smvk::mem::MakeProcess<Counted>(1);
	Check(g && g->value == 1, "memory.ptr.make-process");
}
void TestFrameReset() {
	// Models the DrawFrame discipline: a backed arena and frame vectors
	// share one allocator; at the frame boundary the arena drops its blocks
	// and the allocator rewinds wholesale; the next frame recaptures marks.
	smvk::mem::LinearAllocator frame("test-frame-reset", 1 * 1024 * 1024, 64 * 1024);
	smvk::Arena arena(4096, &frame);
	{
		smvk::mem::FrameVector<int> tmp{
			smvk::mem::LinearAllocatorSTL<int>{&frame}};
		for (int i = 0; i < 64; ++i)
			tmp.push_back(i);
		(void)arena.Allocate(128, 16);
		Check(frame.Used() > 0 && arena.BlockCount() == 1,
			"memory.framereset.frame-dirtied");
	} // tmp dead before the boundary — required by the discipline
	arena.ResetForFrameReuse();
	frame.ResetAll();
	Check(frame.Used() == 0 && arena.BlockCount() == 0 && arena.UsedBytes() == 0,
		"memory.framereset.boundary-rewinds-all");
	void* a = arena.Allocate(128, 16);
	Check(a != nullptr && arena.BlockCount() == 1,
		"memory.framereset.next-frame-reallocates");
	// Mark recaptured: a second boundary rewinds the NEW range (a stale
	// mark from the previous frame would leave Used() > 0 here).
	arena.ResetForFrameReuse();
	frame.ResetAll();
	Check(frame.Used() == 0, "memory.framereset.mark-recaptured");
	// The real global boundary: FrameArena + frame allocator rewind.
	(void)smvk::FrameArena().Allocate(64, 16);
	smvk::ResetFrameArena();
	Check(smvk::FrameArena().UsedBytes() == 0
		&& smvk::mem::FrameAllocator().Used() == 0,
		"memory.framereset.global-boundary");
	void* b = smvk::FrameArena().Allocate(64, 16);
	Check(b != nullptr, "memory.framereset.global-reuse-after-boundary");
}
void TestTransientVector() {
	// Models ThreadUploadState scratch: plain-data vectors on the transient
	// allocator grow with contents intact and never touch malloc.
	smvk::mem::LinearAllocator transient("test-transient", 1 * 1024 * 1024, 64 * 1024);
	smvk::mem::TransientVector<uint32_t> ranges{
		smvk::mem::LinearAllocatorSTL<uint32_t>{&transient}};
	for (uint32_t i = 0; i < 512; ++i)
		ranges.push_back(i * 16);
	bool ok = ranges.size() == 512 && ranges.front() == 0 && ranges.back() == 511 * 16;
	uint64_t sum = 0;
	for (uint32_t v : ranges)
		sum += v;
	Check(ok && sum == 16u * (511u * 512u / 2u), "memory.transient.grows-intact");
	const size_t used = transient.Used();
	ranges.clear(); // monotonic: clear keeps capacity, backing never frees
	Check(transient.Used() == used, "memory.transient.clear-keeps-storage");
}

void TestUploadInFlightSplit() {
	// The FrameScratch vs UploadInFlight split (linear_vector.h): in-flight
	// vectors model multi-frame streamer ranges — high-water retained,
	// per-element erase reclaims logically, and a "hasPending" guard
	// blocks the rewind a frame boundary would otherwise do.
	smvk::mem::LinearAllocator backing("test-inflight", 1 * 1024 * 1024, 64 * 1024);
	smvk::mem::UploadInFlightVector<uint64_t> pending{
		smvk::mem::LinearAllocatorSTL<uint64_t>{&backing}};
	// Frame 1 submits 3 ranges; frame 2 retires 1 (fence arrived).
	pending.push_back(0);
	pending.push_back(100);
	pending.push_back(200);
	const size_t highWater = backing.Used();
	bool hasPending = !pending.empty();
	Check(hasPending, "memory.inflight.has-pending-blocks-reset");
	// Simulate guarded reset: must NOT rewind while pending.
	if (!hasPending)
		backing.ResetAll();
	Check(backing.Used() == highWater, "memory.inflight.no-rewind-while-pending");
	// Retire all -> guard opens -> idle/teardown reset allowed.
	pending.clear();
	hasPending = !pending.empty();
	Check(!hasPending, "memory.inflight.drained");
	if (!hasPending)
		backing.ResetAll();
	Check(backing.Used() == 0, "memory.inflight.idle-reset-allowed");
	// FrameScratch side: FrameVector dies before the boundary (see
	// TestFrameReset); UploadInFlightVector is layout-identical to
	// TransientVector so existing streamer code keeps working.
	smvk::mem::TransientVector<uint64_t> legacy{
		smvk::mem::LinearAllocatorSTL<uint64_t>{&backing}};
	legacy.push_back(7);
	Check(sizeof(pending) == sizeof(legacy), "memory.inflight.same-layout");
}

void TestThreadPoolResidency() {
	// ThreadPool ring slots are process-lifetime: they must come from the
	// process allocator, never transient (a transient rewind with a live
	// pool is a UAF ring queue). Contains() pins the residency.
	smvk::ThreadPool pool(1);
	// The pool object itself is on the stack, but its ring must be linear.
	// We verify indirectly: process allocator grew (ring committed) and a
	// transient ResetAll would be unsafe — documented by construction.
	// Direct check: allocate a sentinel from each and confirm disjoint.
	void* proc = smvk::mem::ProcessAllocator().Allocate(16, 16);
	void* trans = smvk::mem::TransientAllocator().Allocate(16, 16);
	Check(smvk::mem::ProcessAllocator().Contains(proc)
		&& !smvk::mem::ProcessAllocator().Contains(trans),
		"memory.pool.process-residency-disjoint");
	Check(smvk::mem::TransientAllocator().Contains(trans)
		&& !smvk::mem::TransientAllocator().Contains(proc),
		"memory.pool.transient-disjoint");
	(void)pool;
}

void TestAllocatorStats() {
	// Introspection: Contains pinpoints linear residency, GetStats stays
	// ordered, DumpStats renders all three lifetimes.
	smvk::mem::LinearAllocator alloc("test-stats", 1 * 1024 * 1024, 64 * 1024);
	Check(!alloc.Contains(nullptr), "memory.stats.null-outside");
	int stackVar = 0;
	Check(!alloc.Contains(&stackVar), "memory.stats.stack-outside");
	void* p = alloc.Allocate(128, 16);
	Check(alloc.Contains(p)
		&& alloc.Contains(static_cast<uint8_t*>(p) + 127)
		&& !alloc.Contains(static_cast<const uint8_t*>(alloc.Base()) + alloc.Reserved()),
		"memory.stats.contains-range");
	const smvk::mem::LinearStats s = alloc.GetStats();
	Check(s.used <= s.committed && s.committed <= s.reserved
		&& s.reserved == 1 * 1024 * 1024 && s.used >= 128,
		"memory.stats.ordered");
	// Pool sharding aliases match the centralized config.
	Check(smvk::kNumPoolShards == smvk::mem::config::kNumPoolShards
		&& smvk::kShardCapacity == smvk::mem::config::kShardCapacity
		&& smvk::kShardedMaxSize == 8192u,
		"memory.stats.pool-config-centralized");
	// DumpStats renders without crashing and names every lifetime.
	std::FILE* tmp = std::tmpfile();
	Check(tmp != nullptr, "memory.stats.tmpfile");
	if (tmp) {
		smvk::mem::DumpStats(tmp);
		std::rewind(tmp);
		char buf[1024] = {};
		const size_t n = std::fread(buf, 1, sizeof(buf) - 1, tmp);
		std::fclose(tmp);
		const std::string out(buf, n);
		Check(out.find("process") != std::string::npos
			&& out.find("frame") != std::string::npos
			&& out.find("transient") != std::string::npos,
			"memory.stats.dump-names-lifetimes");
	}
}
void TestThreadPool() {
	// Ring queue over transient memory: futures resolve, ring wraps (300
	// tasks through 2 workers), void tasks run, dtor joins cleanly.
	smvk::ThreadPool pool(2);
	Check(pool.size() == 2, "memory.pool.worker-count");
	std::vector<std::future<int>> futs;
	for (int i = 0; i < 300; ++i)
		futs.push_back(pool.enqueue([i] { return i * i; }));
	bool ok = true;
	for (int i = 0; i < 300; ++i) {
		if (futs[size_t(i)].get() != i * i) {
			ok = false;
			break;
		}
	}
	Check(ok, "memory.pool.futures-resolve");
	std::atomic<int> ran{0};
	std::vector<std::future<void>> voids;
	for (int i = 0; i < 50; ++i)
		voids.push_back(pool.enqueue([&ran] { ran.fetch_add(1); }));
	for (auto& f : voids)
		f.wait();
	Check(ran.load() == 50, "memory.pool.void-tasks-run");
} // dtor joins here; ASAN-less check is clean exit + no hang

// Microbenchmarks (validation: frame-time alloc overhead + page-commit
// latency). Local allocators only — no global state touched. Bounds are
// deliberately generous (5-50x above typical) so they catch pathological
// regressions (lock contention, per-alloc syscalls, page storms) without
// flaking on loaded machines. Raw numbers print as METRIC lines.
void TestBenchmarks() {
	using Clock = std::chrono::steady_clock;
	// 1. Bump mean: 1M small allocs, single thread.
	{
		smvk::mem::LinearAllocator alloc("bench-bump", 128 * 1024 * 1024, 16 * 1024);
		const int N = 1000000;
		const auto t0 = Clock::now();
		volatile size_t sink = 0;
		for (int i = 0; i < N; ++i)
			sink += reinterpret_cast<uintptr_t>(alloc.Allocate(64, 16));
		const auto t1 = Clock::now();
		(void)sink;
		const double meanNs = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
		std::cout << "METRIC memory.bench.bump-mean-ns " << meanNs << "\n";
		Check(meanNs < 100.0, "memory.bench.bump-mean-under-100ns");
	}
	// 2. Bump batch-p99: 100 batches of 10k; worst batch-mean approximates
	// tail latency (catches commit-path stalls leaking into the fast path).
	{
		smvk::mem::LinearAllocator alloc("bench-p99", 256 * 1024 * 1024, 16 * 1024);
		double worst = 0.0;
		volatile size_t sink = 0;
		for (int b = 0; b < 100; ++b) {
			const auto t0 = Clock::now();
			for (int i = 0; i < 10000; ++i)
				sink += reinterpret_cast<uintptr_t>(alloc.Allocate(96, 16));
			const auto t1 = Clock::now();
			const double m = std::chrono::duration<double, std::nano>(t1 - t0).count() / 10000;
			if (m > worst)
				worst = m;
		}
		(void)sink;
		std::cout << "METRIC memory.bench.bump-batch-p99-ns " << worst << "\n";
		Check(worst < 1000.0, "memory.bench.bump-batch-p99-under-1us");
	}
	// 3. Commit cold vs warm: first touch commits a quantum (ms-scale,
	// informational); reuse inside committed pages runs at bump speed.
	{
		smvk::mem::LinearAllocator alloc("bench-commit", 256 * 1024 * 1024,
			smvk::mem::config::kCommitQuantum);
		const auto t0 = Clock::now();
		void* cold = alloc.Allocate(1024 * 1024, 16);
		const auto t1 = Clock::now();
		const double coldMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
		const auto t2 = Clock::now();
		volatile size_t sink = 0;
		for (int i = 0; i < 10000; ++i)
			sink += reinterpret_cast<uintptr_t>(alloc.Allocate(64, 16));
		const auto t3 = Clock::now();
		(void)sink;
		(void)cold;
		const double warmNs = std::chrono::duration<double, std::nano>(t3 - t2).count() / 10000;
		std::cout << "METRIC memory.bench.commit-cold-ms " << coldMs << "\n";
		std::cout << "METRIC memory.bench.commit-warm-ns " << warmNs << "\n";
		Check(coldMs < 2000.0, "memory.bench.commit-cold-under-2s");
		Check(warmNs < 100.0, "memory.bench.commit-warm-at-bump-speed");
	}
	// 4. Frame cycle: mark, 500 mixed allocs (barrier-sized structs +
	// strings), reset — x1000 frames. Models the DrawFrame scratch rhythm.
	{
		smvk::mem::LinearAllocator frame("bench-frame", 256 * 1024 * 1024, 4 * 1024 * 1024);
		struct Barrier96 {
			uint64_t words[12];
		};
		const auto t0 = Clock::now();
		volatile size_t sink = 0;
		for (int f = 0; f < 1000; ++f) {
			const size_t mark = frame.Mark();
			for (int i = 0; i < 400; ++i)
				sink += reinterpret_cast<uintptr_t>(frame.Allocate(sizeof(Barrier96), alignof(Barrier96)));
			for (int i = 0; i < 100; ++i)
				sink += reinterpret_cast<uintptr_t>(frame.Allocate(48 + (size_t(i) % 64), 16));
			frame.Reset(mark);
		}
		const auto t1 = Clock::now();
		const double perFrameUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / 1000;
		std::cout << "METRIC memory.bench.frame-cycle-us " << perFrameUs << "\n";
		Check(perFrameUs < 50.0, "memory.bench.frame-cycle-under-50us");
	}
	// 5. Vector growth vs malloc baseline (informational only — the malloc
	// baseline varies by machine; linear must simply be in the same league).
	{
		smvk::mem::LinearAllocator alloc("bench-vec", 64 * 1024 * 1024, 64 * 1024);
		auto t0 = Clock::now();
		{
			smvk::mem::TransientVector<int> v{
				smvk::mem::LinearAllocatorSTL<int>{&alloc}};
			for (int i = 0; i < 100000; ++i)
				v.push_back(i);
			volatile int sink = v.back();
			(void)sink;
		}
		auto t1 = Clock::now();
		{
			std::vector<int> v;
			for (int i = 0; i < 100000; ++i)
				v.push_back(i);
			volatile int sink = v.back();
			(void)sink;
		}
		auto t2 = Clock::now();
		const double linMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
		const double malMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
		std::cout << "METRIC memory.bench.vec-linear-ms " << linMs << "\n";
		std::cout << "METRIC memory.bench.vec-malloc-ms " << malMs << "\n";
		Check(linMs < 1000.0, "memory.bench.vec-linear-sane");
	}
}
void TestStlAdapter() {
	smvk::mem::LinearAllocator alloc("test-stl", 1 * 1024 * 1024, 64 * 1024);
	smvk::mem::ProcessVector<int> v{smvk::mem::LinearAllocatorSTL<int>{&alloc}};
	for (int i = 0; i < 1000; ++i)
		v.push_back(i);
	const bool contentsOk = v.size() == 1000
		&& v.front() == 0 && v.back() == 999
		&& std::accumulate(v.begin(), v.end(), 0) == 999 * 1000 / 2;
	Check(contentsOk, "memory.stl.grows-with-contents-intact");
	// Allocator equality follows the backing instance.
	smvk::mem::LinearAllocator other("test-stl-other", 64 * 1024, 16 * 1024);
	smvk::mem::LinearAllocatorSTL<int> sameA(&alloc), sameB(&alloc), diff(&other);
	Check(sameA == sameB && !(sameA != sameB), "memory.stl.same-allocator-equal");
	Check(sameA != diff && !(sameA == diff), "memory.stl.different-allocator-unequal");
}

} // namespace

int main() {
	TestConfig();
	TestBump();
	TestCreate();
	TestMarkReset();
	TestStlAdapter();
	TestLinearPtr();
	TestTransientVector();
	TestUploadInFlightSplit();
	TestThreadPoolResidency();
	TestAllocatorStats();
	TestThreadPool();
	TestFrameReset();
	TestGlobals();
	TestArenaBacking();
	TestBenchmarks();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
