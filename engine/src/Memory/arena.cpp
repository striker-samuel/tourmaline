#include <Engine/Memory/arena.h>

#include <Engine/Memory/memory.h>

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
	// Dedicated block when the request cannot fit a default block.
	if (size > blockSize_) {
		PushBlock(size + align);
		Block& big = blocks_.back();
		uint8_t* p = big.data;
		const size_t pad = (align - (reinterpret_cast<uintptr_t>(p) & (align - 1))) & (align - 1);
		big.used = pad + size;
		totalUsed_ += size;
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
	// Truncated: format into a heap buffer, then copy into the arena.
	std::string heap(static_cast<size_t>(n) + 1, '\0');
	va_start(args, fmt);
	std::vsnprintf(heap.data(), heap.size(), fmt, args);
	va_end(args);
	return AllocateString(StringViewT(heap.data(), static_cast<size_t>(n)));
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
	static Arena arena(mem::config::kAppArenaBlock, &mem::ProcessAllocator());
	return arena;
}

Arena& FrameArena() {
	static Arena arena(mem::config::kFrameArenaBlock, &mem::FrameAllocator());
	return arena;
}

void ResetFrameArena() {
	FrameArena().Reset();
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
