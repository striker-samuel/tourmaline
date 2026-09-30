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
	TestFrameReset();
	TestGlobals();
	TestArenaBacking();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
