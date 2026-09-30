#pragma once
// Global linear allocators, one per lifetime (Phase 0 infrastructure).
//
//   ProcessAllocator()   — reserved at startup, never reset. Process-lifetime
//                          objects (Phase 1), AppArena blocks, resource pools.
//   FrameAllocator()     — reset at frame start. FrameArena blocks, per-frame
//                          temporaries (Phase 2).
//   TransientAllocator() — reset after GPU sync at the frame boundary.
//                          Staging rings, readback, scratch (Phase 4).
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

namespace smvk::mem {

LinearAllocator& ProcessAllocator();
LinearAllocator& FrameAllocator();
LinearAllocator& TransientAllocator();

void InitMemory();
void ShutdownMemory(); // releases reservations; only call at teardown

} // namespace smvk::mem
