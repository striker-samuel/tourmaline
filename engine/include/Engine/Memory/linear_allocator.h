// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Linear (bump-pointer) allocator over reserved virtual memory
// (Phase 0 infrastructure).
//
// Model: reserve a large virtual range at startup (no physical commit),
// commit pages on demand as the bump pointer advances, never free.
// Reset only via mark/reset (frame/transient lifetimes); process-lifetime
// allocators never reset.
//
// Thread safety: Allocate() is lock-free on the bump fast path (atomic
// CAS); page commit serializes on a mutex (overlapping commits are
// idempotent, so no allocation is ever lost or double-handed-out).
//
// OOM policy: crash + report. Exhaustion (or a commit failure) calls
// ReportOomAndCrash(), which logs to stderr and aborts. No fallback,
// no compaction, no survival strategy — by design.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <utility>

#include <Engine/Memory/alloc_config.h>

namespace tour::mem {

// Logs "OOM in <name>: requested X, used Y / reserved Z" to stderr,
// flushes, then aborts. [[noreturn]] — there is no recovery path.
[[noreturn]] void ReportOomAndCrash(std::string_view allocatorName,
	size_t requested, size_t used, size_t reserved);

// Snapshot for tracking/stats (see memory.h DumpStats). All three are
// monotonic within a reset epoch: used <= committed <= reserved.
struct LinearStats {
	size_t reserved = 0;
	size_t committed = 0;
	size_t used = 0;
};

class LinearAllocator {
public:
	// name is stored as a view — pass a string literal (static storage).
	explicit LinearAllocator(std::string_view name,
		size_t reservedBytes,
		size_t commitQuantumBytes = config::kCommitQuantum);
	~LinearAllocator();

	LinearAllocator(const LinearAllocator&) = delete;
	LinearAllocator& operator=(const LinearAllocator&) = delete;
	LinearAllocator(LinearAllocator&&) = delete;
	LinearAllocator& operator=(LinearAllocator&&) = delete;

	// Reserves the virtual range. Called automatically on first Allocate;
	// call explicitly at startup (InitMemory) to fail fast before
	// subsystems initialize. OOM-crashes on failure.
	void EnsureInitialized();
	[[nodiscard]] bool IsInitialized() const noexcept { return base_ != nullptr; }

	// Bump-allocate `size` bytes with `align` (power of two) alignment.
	// Commits pages as needed. OOM-crashes when the request would exceed
	// the reservation or when the OS refuses to commit.
	[[nodiscard]] void* Allocate(size_t size, size_t align = config::kDefaultAlign);

	// Placement-construct T in the allocator (never freed individually).
	template<typename T, typename... Args>
	[[nodiscard]] T* Create(Args&&... args) {
		void* p = Allocate(sizeof(T), alignof(T));
		return new (p) T(std::forward<Args>(args)...);
	}

	// Mark/reset for frame/scope lifetimes. Reset(mark) rewinds the bump
	// pointer; committed pages stay committed (no decommit — reuse is the
	// point). Marks from an older state must not exceed the current offset.
	[[nodiscard]] size_t Mark() const noexcept {
		return offset_.load(std::memory_order_relaxed);
	}
	void Reset(size_t mark) noexcept;
	void ResetAll() noexcept { Reset(0); }

	[[nodiscard]] std::string_view Name() const noexcept { return name_; }
	[[nodiscard]] size_t Reserved() const noexcept { return reserved_; }
	[[nodiscard]] size_t Committed() const noexcept {
		return committed_.load(std::memory_order_relaxed);
	}
	[[nodiscard]] size_t Used() const noexcept {
		return offset_.load(std::memory_order_relaxed);
	}
	[[nodiscard]] LinearStats GetStats() const noexcept {
		return LinearStats{reserved_, Committed(), Used()};
	}
	// Base of the reserved range (null until EnsureInitialized/Allocate).
	[[nodiscard]] const uint8_t* Base() const noexcept { return base_; }
	// True when p points inside the reserved range. Used to verify that
	// migrated objects (arenas, pools, process singletons) really live in
	// linear memory. Null and uninitialized → false.
	[[nodiscard]] bool Contains(const void* p) const noexcept {
		if (!base_ || !p)
			return false;
		const auto* addr = static_cast<const uint8_t*>(p);
		return addr >= base_ && addr < base_ + reserved_;
	}

private:
	void CommitUpTo(size_t neededEnd);

	std::string_view name_;
	size_t reserved_ = 0;
	size_t commitQuantum_ = 0;
	uint8_t* base_ = nullptr;
	std::atomic<size_t> offset_{0};
	std::atomic<size_t> committed_{0};
	std::mutex commitMutex_;
	bool reserveFailed_ = false;
};

} // namespace tour::mem
// COMPAT (the boundary audit): LinearAllocator lives in tour::mem; the
// smvk aliases keep call sites compiling through the migration.
namespace smvk::mem {
	using ::tour::mem::LinearAllocator;
	using ::tour::mem::LinearStats;
}