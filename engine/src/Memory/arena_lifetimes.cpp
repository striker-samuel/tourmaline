// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Renderer frame/flight arena lifetime policy; Core only supplies Arena.
#include <Engine/Memory/arena.h>
#include <Engine/Memory/memory.h>
#include <Engine/smvk.h>
#include <array>
#include <cassert>
#include <memory>

namespace smvk {

Arena& AppArena() {
	static Arena arena(mem::config::Runtime().appArenaBlock, &mem::ProcessAllocator());
	return arena;
}

Arena& FrameArena() {
	static Arena arena(mem::config::Runtime().frameArenaBlock, &mem::FrameAllocator());
	return arena;
}

void ResetFrameArena() {
	// Full frame rewind at the DrawFrame boundary: drop the arena's blocks
	// (without touching the backing store — the wholesale reset covers
	// them) and rewind the ENTIRE frame allocator, discarding dead
	// FrameVector temporaries (RenderGraph barriers) along with scratch.
	// Safe only because this runs at frame start, when no frame-lifetime
	// allocation is live (see mem::memory.h reset discipline).
	FrameArena().ResetForFrameReuse();
	mem::FrameAllocator().ResetAll();
}

namespace {
struct FlightArenaStorage {
	mem::LinearAllocator backing;
	Arena arena;

	FlightArenaStorage()
		: backing("flight", mem::config::Runtime().flightReserve,
			mem::config::Runtime().commitQuantum)
		, arena(mem::config::Runtime().frameArenaBlock, &backing) {}

	void Reset() {
		// The slot fence has retired; invalidate all its CPU views.
		arena.ResetForFrameReuse();
		backing.ResetAll();
	}
};

FlightArenaStorage& FlightStorage(uint32_t flightIndex) {
	assert(flightIndex < smvk::limits::MaxFramesInFlight);
	// Flight arenas are accessed on the render/acquire thread only.
	static std::array<std::unique_ptr<FlightArenaStorage>,
		smvk::limits::MaxFramesInFlight> slots;
	if (!slots[flightIndex])
		slots[flightIndex] = std::make_unique<FlightArenaStorage>();
	return *slots[flightIndex];
}
} // namespace

Arena& FlightArena(uint32_t flightIndex) {
	return FlightStorage(flightIndex).arena;
}

void ResetFlightArena(uint32_t flightIndex) {
	FlightStorage(flightIndex).Reset();
}

} // namespace smvk
