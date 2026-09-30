#pragma once
// Linear (bump) allocators for URI/string scratch (see Memory/string_view.h).
//
// Two arenas exist:
//   - AppArena(): process-lifetime storage. Per-Cluster URI keys are NOT
//     freed individually — each Cluster owns a private Arena instead, so
//     unloading a cluster releases all of its strings wholesale.
//   - FrameArena(): per-frame scratch (sibling URI probing, normalized URI
//     builds). Reset once per frame by the renderer; views into it never
//     cross a frame boundary.
//
// Blocks are chunked: a request larger than the current block's remainder
// (or than blockSize itself) gets a dedicated block. Reset() frees every
// block — the frame arena stabilizes at its high-water mark instead of
// churning malloc.

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

#include <Engine/Memory/string_view.h>
#include <Engine/Memory/alloc_config.h>
#include <Engine/Memory/linear_allocator.h>

namespace smvk {

class Arena {
public:
	explicit Arena(size_t blockSize = kDefaultBlockSize,
		mem::LinearAllocator* backing = nullptr)
		: backing_(backing)
		, blockSize_(blockSize < 64 ? 64 : blockSize) {}
	Arena(const Arena&) = delete;
	Arena& operator=(const Arena&) = delete;
	Arena(Arena&&) = default;
	Arena& operator=(Arena&&) = default;

	// Raw bump allocation with alignment (>= 8, defaults to 16).
	[[nodiscard]] void* Allocate(size_t size, size_t align = 16);
	// Placement-constructs T in the arena (never freed individually).
	template<typename T, typename... Args>
	[[nodiscard]] T* Create(Args&&... args) {
		void* p = Allocate(sizeof(T), alignof(T));
		return new (p) T(std::forward<Args>(args)...);
	}
	// Copies the string into the arena, NUL-terminated, returns the view.
	[[nodiscard]] StringViewT AllocateString(StringViewT s);
	// printf-style formatting into the arena (URI construction etc.).
	[[nodiscard]] StringViewT FormatString(const char* fmt, ...);

	// Frees every block. The frame arena calls this once per frame; a
	// cluster's private arena effectively calls it from the destructor.
	// With a backing allocator, blocks are NOT freed — the backing bump
	// pointer rewinds to the mark captured at first backing use, so the
	// next frame reuses warm committed pages.
	void Reset();
	[[nodiscard]] size_t UsedBytes() const { return totalUsed_; }
	[[nodiscard]] size_t BlockCount() const { return blocks_.size(); }

	// Attaches a backing linear allocator for block storage (Phase 1/2
	// migration). Ideally called before the first Allocate; blocks already
	// taken from malloc stay malloc-owned and are freed on Reset, while new
	// blocks come from the backing store. Never changes after use — the
	// rewind mark is captured at first backing use.
	void SetBacking(mem::LinearAllocator* backing) { backing_ = backing; }
	[[nodiscard]] mem::LinearAllocator* Backing() const { return backing_; }

	// Default chunk sizes live in mem::config (alloc_config.h) — the single
	// place to resize. kDefaultBlockSize preserves the historic 1 MiB for
	// existing callers; AppArena/FrameArena pass their dedicated defaults.
	static constexpr size_t kDefaultBlockSize = mem::config::kAppArenaBlock;

private:
	struct Block {
		uint8_t* data = nullptr; // backing-owned when owned==null, else malloc
		std::unique_ptr<uint8_t[]> owned; // non-null iff malloc-backed
		size_t size = 0;
		size_t used = 0;
	};
	// Carves `bytes` of block storage from the backing allocator (capturing
	// the rewind mark on first use), or returns null when unbacked so the
	// caller falls back to malloc-owned storage.
	[[nodiscard]] uint8_t* AllocateBlockStorage(size_t bytes);
	// Appends a fresh block of `bytes` storage (backing-backed or malloc).
	void PushBlock(size_t bytes);
	std::vector<Block> blocks_;
	mem::LinearAllocator* backing_ = nullptr;
	size_t backingMark_ = 0;
	bool backingMarkValid_ = false;
	size_t blockSize_ = kDefaultBlockSize;
	size_t totalUsed_ = 0;

	size_t BlockRemainder() const {
		return blocks_.empty() ? 0
			: blocks_.back().size - blocks_.back().used;
	}
};

// Process-lifetime arena (app boot..shutdown; grows monotonically).
[[nodiscard]] Arena& AppArena();
// Per-frame scratch; ResetFrameArena() at frame start.
[[nodiscard]] Arena& FrameArena();
void ResetFrameArena();

} // namespace smvk
