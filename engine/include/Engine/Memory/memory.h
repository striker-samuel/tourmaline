// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Global linear allocators, one per lifetime (Phase 0 infrastructure).
//
//   ProcessAllocator()   — reserved at startup, never reset. Process-lifetime
//                          objects (Phase 1), AppArena blocks, resource pools.
//   FrameAllocator()     — reset at frame start. FrameArena blocks, per-frame
//                          temporaries (Phase 2).
//   TransientAllocator() — UploadInFlight-monotonic (Phase 4). Staging
//                          rings, readback, in-flight ranges. The backing
//                          bump is NEVER rewound while streamer vectors are
//                          live (they span frames); reclamation is per-
//                          element clear/erase. FrameScratch (FrameVector +
//                          ResetFrameArena at DrawFrame start) is the ONLY
//                          per-frame-reset scratch — never put in-flight
//                          state in it. A guarded ResetAll is allowed solely
//                          at device-idle/teardown when nothing is pending.
//
// Sizes come from mem::config (alloc_config.h) — resize there.
//
// InitMemory() reserves virtual address space for all three. Call it FIRST
// in main(), before any subsystem init or worker thread spawn, so an
// unsatisfiable reservation fails fast with the OOM diagnostic instead of
// mid-init. All three also lazily initialize on first Allocate, so library
// / test use without InitMemory still works.
//
// Reset discipline: a Reset(mark) rewinds the SHARED bump pointer. Only
// reset a frame/transient allocator at a point where nothing allocated
// above the rewind mark is still live (frame start after GPU sync for
// transient; frame start for frame). Violating this corrupts live data —
// the allocators cannot detect it. Arena (arena.h) handles its own marks.

#include <Engine/Memory/linear_allocator.h>

#include <cstdio>
#include <memory>
#include <utility>

namespace tour::mem {

LinearAllocator& ProcessAllocator();
LinearAllocator& FrameAllocator();
LinearAllocator& TransientAllocator();

void InitMemory();
void ShutdownMemory(); // DumpStats report + teardown hook; call at exit

// One stderr line per allocator: used / committed / reserved. Call at
// shutdown (ShutdownMemory does) or on demand when diagnosing footprint.
void DumpStats(std::FILE* out = stderr);

// Ownership for process-lifetime objects (Phase 1 migration).
//
// LinearPtr<T> is a unique_ptr whose deleter runs the destructor WITHOUT
// freeing: the storage lives in a LinearAllocator and is reclaimed
// wholesale (process teardown) or by mark/reset, never per-object.
// Drop-in for std::unique_ptr at owned-process-object sites: get(),
// reset(), release(), operator-> all behave identically; only the
// allocation (MakeProcess/MakeLinear + Create) and the free (none)
// differ. OOM still crashes via ReportOomAndCrash.
template<typename T>
struct LinearDeleter {
	void operator()(T* p) const noexcept {
		if (p)
			p->~T();
	}
};

template<typename T>
using LinearPtr = std::unique_ptr<T, LinearDeleter<T>>;

template<typename T, typename... Args>
[[nodiscard]] LinearPtr<T> MakeLinear(LinearAllocator& allocator, Args&&... args) {
	return LinearPtr<T>(allocator.Create<T>(std::forward<Args>(args)...));
}

template<typename T, typename... Args>
[[nodiscard]] LinearPtr<T> MakeProcess(Args&&... args) {
	return MakeLinear<T>(ProcessAllocator(), std::forward<Args>(args)...);
}

} // namespace tour::mem

// COMPAT (the namespace/RHI boundary audit, todo item 8.4): the memory
// subsystem is ENGINE-OWNED and lives in tour::mem; these smvk::mem
// aliases keep the existing call sites compiling through the consumer
// migration. RETIRE them as call sites move to tour::mem.
namespace smvk::mem {
	using ::tour::mem::InitMemory;
	using ::tour::mem::ShutdownMemory;
	using ::tour::mem::DumpStats;
	using ::tour::mem::LinearDeleter;
	using ::tour::mem::LinearPtr;
	using ::tour::mem::MakeLinear;
	using ::tour::mem::MakeProcess;
	using ::tour::mem::ProcessAllocator;
	using ::tour::mem::FrameAllocator;
	using ::tour::mem::TransientAllocator;
}
