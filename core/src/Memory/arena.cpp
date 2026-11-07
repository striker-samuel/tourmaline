// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Core/Memory/arena.h>

#include <Core/Memory/memory.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace smvk {

uint8_t* Arena::AllocateBlockStorage(size_t bytes) {
	// Backing-backed storage is reclaimed wholesale by Reset() (rewind to
	// backingMark_), never per block — the caller just stores the pointer
	// in the new Block with null `owned`.
	if (!backing_)
		return nullptr; // caller falls back to malloc-owned storage
	if (!backingMarkValid_) {
		backingMark_ = backing_->Mark();
		backingMarkValid_ = true;
	}
	// LinearAllocator OOM-crashes with its own diagnostic when exhausted.
	return static_cast<uint8_t*>(backing_->Allocate(bytes, alignof(void*)));
}

// Appends a fresh block of `bytes` storage: backing-backed when the arena
// has backing (null `owned`), malloc-owned otherwise.
void Arena::PushBlock(size_t bytes) {
	Block b;
	b.size = bytes;
	if (uint8_t* mem = AllocateBlockStorage(bytes)) {
		b.data = mem;
	}
	else {
		b.owned = std::make_unique<uint8_t[]>(bytes);
		b.data = b.owned.get();
	}
	blocks_.push_back(std::move(b));
}

void* Arena::Allocate(size_t size, size_t align) {
	if (align < 8)
		align = 8;
	++allocationCount_;
	totalRequestedBytes_ += size;
	// Dedicated block when the request cannot fit a default block.
	if (size > blockSize_) {
		PushBlock(size + align);
		Block& big = blocks_.back();
		uint8_t* p = big.data;
		const size_t pad = (align - (reinterpret_cast<uintptr_t>(p) & (align - 1))) & (align - 1);
		big.used = pad + size;
		totalUsed_ += size;
		peakUsedBytes_ = std::max(peakUsedBytes_, totalUsed_);
		return p + pad;
	}
	// Bump inside the current block; chain a fresh one when full.
	if (blocks_.empty() || BlockRemainder() < size + align) {
		PushBlock(blockSize_);
	}
	Block& cur = blocks_.back();
	uint8_t* p = cur.data + cur.used;
	const size_t pad = (align - (reinterpret_cast<uintptr_t>(p) & (align - 1))) & (align - 1);
	cur.used += pad + size;
	totalUsed_ += size;
	peakUsedBytes_ = std::max(peakUsedBytes_, totalUsed_);
	return p + pad;
}

StringViewT Arena::AllocateString(StringViewT s) {
	if (!s.valid() || s.empty())
		return StringViewT("", 0);
	char* p = static_cast<char*>(Allocate(s.size() + 1));
	std::memcpy(p, s.data(), s.size());
	p[s.size()] = '\0';
	return StringViewT(p, s.size());
}

StringViewT Arena::FormatString(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char stackBuf[1024];
	const int n = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
	va_end(args);
	if (n < 0)
		return {};
	if (static_cast<size_t>(n) < sizeof(stackBuf))
		return AllocateString(StringViewT(stackBuf, static_cast<size_t>(n)));
	// Large output writes directly into the same arena. No temporary
	// std::string allocation or second arena copy.
	char* output = static_cast<char*>(Allocate(static_cast<size_t>(n) + 1));
	va_start(args, fmt);
	std::vsnprintf(output, static_cast<size_t>(n) + 1, fmt, args);
	va_end(args);
	return StringViewT(output, static_cast<size_t>(n));
}

void Arena::Reset() {
	// Backing-backed blocks are NOT freed: rewind the backing bump pointer
	// to the mark captured at first backing use (malloc-owned blocks in the
	// list, if any, free via their unique_ptr as the vector clears). The
	// mark stays valid — the next frame reuses the same warm range.
	// Unbacked arenas (e.g. per-cluster) free everything as before.
	if (backing_ && backingMarkValid_)
		backing_->Reset(backingMark_);
	blocks_.clear();
	totalUsed_ = 0;
}

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
	assert(flightIndex < tour::mem::config::kFlightArenaSlots);
	// Flight arenas are accessed on the render/acquire thread only.
	static std::array<std::unique_ptr<FlightArenaStorage>,
		tour::mem::config::kFlightArenaSlots> slots;
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

// Late definition (needs the complete Arena type): compose two views.
// External (non-inline) definition matching string_view.h's declaration.
namespace smvk {

StringViewT ConcatInto(class Arena& arena, StringViewT a, StringViewT b) {
	if (a.empty())
		return arena.AllocateString(b);
	if (b.empty())
		return arena.AllocateString(a);
	char* p = static_cast<char*>(arena.Allocate(a.size() + b.size() + 1));
	std::memcpy(p, a.data(), a.size());
	std::memcpy(p + a.size(), b.data(), b.size());
	p[a.size() + b.size()] = '\0';
	return StringViewT(p, a.size() + b.size());
}

} // namespace smvk
