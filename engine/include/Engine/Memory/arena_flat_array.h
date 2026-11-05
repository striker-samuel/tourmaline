// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Arena-backed flat array for trivially copyable, renderer-lifetime data.
// Unlike std::vector, the array has no allocator-owned element lifecycle.
// Storage belongs to the arena, and is released in bulk with the array.
// Growth allocates a new chunk (old arena chunks remain until destruction).
// Pointer is stable between Reserve calls, NOT across a growth operation.

#include <Engine/Memory/arena.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace tour::mem {

template <typename T>
class ArenaFlatArray {
    static_assert(std::is_trivially_copyable_v<T>,
        "ArenaFlatArray only supports trivially copyable records");
public:
    ArenaFlatArray() = default;
    ArenaFlatArray(const ArenaFlatArray&) = delete;
    ArenaFlatArray& operator=(const ArenaFlatArray&) = delete;
    ArenaFlatArray(ArenaFlatArray&&) = delete;
    ArenaFlatArray& operator=(ArenaFlatArray&&) = delete;

    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] const T& operator[](size_t index) const noexcept { return data_[index]; }
    [[nodiscard]] T& operator[](size_t index) noexcept { return data_[index]; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] const T* end() const noexcept { return data_ ? data_ + size_ : nullptr; }

    void clear() noexcept { size_ = 0; }

    void Reserve(size_t requested) {
        if (requested <= capacity_) return;
        if (requested > std::numeric_limits<size_t>::max() / sizeof(T))
            smvk::mem::ReportOomAndCrash("ArenaFlatArray", requested * sizeof(T),
                capacity_ * sizeof(T), std::numeric_limits<size_t>::max());
        // Exact-size growth: the hot path is one Assign per frame with a
        // stable node count, and scene reloads need not precommit 1M nodes.
        T* next = static_cast<T*>(arena_.Allocate(requested * sizeof(T),
            std::max<size_t>(alignof(T), 16)));
        if (size_) std::memcpy(next, data_, size_ * sizeof(T));
        data_ = next;
        capacity_ = requested;
    }

    void Assign(std::span<const T> source) {
        // Check before growth so self-assignment from our own storage is safe.
        if (source.data() == data_ && source.size() == size_) return;
        Reserve(source.size());
        if (!source.empty()) std::memmove(data_, source.data(), source.size_bytes());
        size_ = source.size();
    }

private:
    smvk::Arena arena_{};
    T* data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

} // namespace tour::mem
