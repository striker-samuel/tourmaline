// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// STL adapter over mem::LinearAllocator (Phase 0 infrastructure).
//
// Lets std:: containers allocate from a linear allocator instead of
// malloc. Deallocate is a no-op (linear memory is reclaimed wholesale by
// mark/reset, never per-object) — so only use these aliases with
// containers whose lifetime is bounded by the backing allocator's reset:
//
//   ProcessVector<T>       — lives until process teardown (never reset).
//   FrameVector<T>         — destroyed/reset before the frame mark resets.
//   TransientVector<T>     — destroyed/reset before the transient mark resets.
//   UploadInFlightVector<T> — monotonic UploadInFlight storage (fence-scoped,
//     per-element clear/erase reclaims; the backing bump is NEVER rewound
//     while any element is live). Same storage as TransientVector, distinct
//     name so frame scratch can never be mis-declared in-flight: use
//     FrameVector for scope-local temps, UploadInFlightVector for streamer
//     pendingRanges/acquires that span frames (see vulkan_impl.h).
//
// Construction always takes an explicit allocator instance:
//   smvk::mem::ProcessVector<int> v{smvk::mem::LinearAllocatorSTL<int>{&alloc}};
// Container growth past the reservation OOM-crashes (see linear_allocator.h).

#include <cstddef>
#include <vector>

#include <Engine/Memory/linear_allocator.h>

namespace smvk::mem {

template<typename T>
struct LinearAllocatorSTL {
	using value_type = T;
	using propagate_on_container_move_assignment = std::true_type;
	using is_always_equal = std::false_type;

	LinearAllocator* allocator = nullptr;

	LinearAllocatorSTL() noexcept = default;
	explicit LinearAllocatorSTL(LinearAllocator* a) noexcept : allocator(a) {}
	template<typename U>
	LinearAllocatorSTL(const LinearAllocatorSTL<U>& o) noexcept : allocator(o.allocator) {}

	[[nodiscard]] T* allocate(size_t n) {
		// Crash early with the allocator's own diagnostic when stateless.
		if (!allocator)
			ReportOomAndCrash("LinearAllocatorSTL(null)", n * sizeof(T), 0, 0);
		return static_cast<T*>(allocator->Allocate(n * sizeof(T), alignof(T)));
	}
	void deallocate(T*, size_t) noexcept {
		// No-op: reclaimed by LinearAllocator::Reset / process teardown.
	}

	template<typename U>
	bool operator==(const LinearAllocatorSTL<U>& o) const noexcept {
		return allocator == o.allocator;
	}
	template<typename U>
	bool operator!=(const LinearAllocatorSTL<U>& o) const noexcept {
		return allocator != o.allocator;
	}
};

template<typename T>
using ProcessVector = std::vector<T, LinearAllocatorSTL<T>>;
template<typename T>
using FrameVector = std::vector<T, LinearAllocatorSTL<T>>;
template<typename T>
using TransientVector = std::vector<T, LinearAllocatorSTL<T>>;
// Monotonic in-flight storage over the transient allocator (never reset
// while live; reclaimed per-element). Identical layout to TransientVector.
template<typename T>
using UploadInFlightVector = std::vector<T, LinearAllocatorSTL<T>>;

} // namespace smvk::mem
