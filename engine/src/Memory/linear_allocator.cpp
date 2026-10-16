// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// LinearAllocator implementation: reserved-VA bump allocator.
//
// Windows: VirtualAlloc MEM_RESERVE the full range at init (address space
// only), MEM_COMMIT in commit-quantum chunks as the bump pointer advances.
// Other platforms: malloc the reservation upfront (project is Windows-first;
// the fallback keeps behavior identical except for lazy commit).
//
// OOM: every exhaustion path funnels into ReportOomAndCrash — stderr
// diagnostic + abort. No fallback, no compaction, by design.

#include <Engine/Memory/linear_allocator.h>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace smvk::mem {

[[noreturn]] void ReportOomAndCrash(std::string_view allocatorName,
	size_t requested, size_t used, size_t reserved) {
	std::fprintf(stderr,
		"[OOM] LinearAllocator '%.*s' exhausted: requested %zu bytes, "
		"used %zu / reserved %zu bytes. No recovery — aborting.\n",
		static_cast<int>(allocatorName.size()), allocatorName.data(),
		requested, used, reserved);
	std::fflush(stderr);
	std::abort();
}

namespace {

size_t AlignUp(size_t v, size_t align) {
	// align must be a non-zero power of two (callers pass alignof or 16).
	const size_t mask = align - 1;
	return (v + mask) & ~mask;
}

void* ReserveVa(size_t bytes) {
#ifdef _WIN32
	return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
	return std::malloc(bytes);
#endif
}

void ReleaseVa(void* p, size_t bytes) {
#ifdef _WIN32
	(void)bytes;
	VirtualFree(p, 0, MEM_RELEASE);
#else
	(void)bytes;
	std::free(p);
#endif
}

// Commits [base + from, base + to). Returns false when the OS refuses.
bool CommitVa(void* base, size_t from, size_t to) {
	if (to <= from)
		return true;
#ifdef _WIN32
	uint8_t* addr = static_cast<uint8_t*>(base) + from;
	return VirtualAlloc(addr, to - from, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
	// Non-Windows fallback commits upfront at reserve time.
	(void)base; (void)from; (void)to;
	return true;
#endif
}

} // namespace

LinearAllocator::LinearAllocator(std::string_view name,
	size_t reservedBytes, size_t commitQuantumBytes)
	: name_(name)
	, reserved_(reservedBytes)
	, commitQuantum_(commitQuantumBytes) {
	if (commitQuantum_ == 0)
		commitQuantum_ = config::kCommitQuantum;
}

LinearAllocator::~LinearAllocator() {
	if (base_)
		ReleaseVa(base_, reserved_);
}

void LinearAllocator::EnsureInitialized() {
	if (base_ || reserveFailed_)
		return;
	std::lock_guard<std::mutex> lock(commitMutex_);
	if (base_ || reserveFailed_)
		return;
	base_ = static_cast<uint8_t*>(ReserveVa(reserved_));
	if (!base_) {
		reserveFailed_ = true;
		ReportOomAndCrash(name_, reserved_, 0, reserved_);
	}
}

void* LinearAllocator::Allocate(size_t size, size_t align) {
	if (align == 0)
		align = config::kDefaultAlign;
	EnsureInitialized();

	// Lock-free bump: CAS the offset forward past alignment pad + size.
	size_t oldOff = offset_.load(std::memory_order_relaxed);
	for (;;) {
		const size_t aligned = AlignUp(oldOff, align);
		const size_t newOff = aligned + size;
		if (newOff > reserved_ || newOff < aligned) // overflow or exhausted
			ReportOomAndCrash(name_, size, oldOff, reserved_);
		if (offset_.compare_exchange_weak(oldOff, newOff,
				std::memory_order_relaxed, std::memory_order_relaxed)) {
			CommitUpTo(newOff);
			return base_ + aligned;
		}
	}
}

void LinearAllocator::CommitUpTo(size_t neededEnd) {
	if (neededEnd <= committed_.load(std::memory_order_acquire))
		return;
	std::lock_guard<std::mutex> lock(commitMutex_);
	const size_t already = committed_.load(std::memory_order_relaxed);
	if (neededEnd <= already)
		return; // another thread committed while we waited
	const size_t target = AlignUp(neededEnd, commitQuantum_);
	const size_t clamped = target > reserved_ ? reserved_ : target;
	if (!CommitVa(base_, already, clamped))
		ReportOomAndCrash(name_, clamped - already, already, reserved_);
	committed_.store(clamped, std::memory_order_release);
}

void LinearAllocator::Reset(size_t mark) noexcept {
	// Only rewind, never beyond what was handed out; committed pages stay
	// committed so the next frame reuses warm memory. A stale (too-new)
	// mark indicates a lifetime bug — clamp instead of corrupting.
	size_t cur = offset_.load(std::memory_order_relaxed);
	if (mark > cur)
		mark = cur;
	offset_.store(mark, std::memory_order_relaxed);
}

} // namespace smvk::mem
