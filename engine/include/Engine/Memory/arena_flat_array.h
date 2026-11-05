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
#include <utility>
#include <vector>
#include <span>
#include <type_traits>

namespace tour::mem {

template <typename T>
class ArenaFlatArray {
    static_assert(std::is_trivially_copyable_v<T>,
        "ArenaFlatArray only supports trivially copyable records");
public:
    ArenaFlatArray() = default;
    // Scene is copyable and movable (editor transactions, cluster reloads).
    // Copies duplicate the elements; moves transfer the arena ownership.
    ArenaFlatArray(const ArenaFlatArray& other) { Assign(std::span<const T>(other)); }
    ArenaFlatArray& operator=(const ArenaFlatArray& other) {
        if (this != &other) Assign(std::span<const T>(other));
        return *this;
    }
    ArenaFlatArray(ArenaFlatArray&& other) noexcept
        : arena_(std::move(other.arena_)),
          data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}
    ArenaFlatArray& operator=(ArenaFlatArray&& other) noexcept {
        if (this != &other) {
            arena_ = std::move(other.arena_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }

    ArenaFlatArray(const std::vector<T>& source) {
        Assign(std::span<const T>(source));
    }
    ArenaFlatArray& operator=(const std::vector<T>& source) {
        Assign(std::span<const T>(source));
        return *this;
    }
    ArenaFlatArray& operator=(std::vector<T>&& source) {
        // No vector allocator ownership is adopted: copy into arena.
        Assign(std::span<const T>(source));
        return *this;
    }
    [[nodiscard]] operator std::vector<T>() const {
        return data_ ? std::vector<T>(data_, data_ + size_) : std::vector<T>{};
    }

    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T& back() const noexcept { return data_[size_ - 1]; }
    [[nodiscard]] T& back() noexcept { return data_[size_ - 1]; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] const T& operator[](size_t index) const noexcept { return data_[index]; }
    [[nodiscard]] T& operator[](size_t index) noexcept { return data_[index]; }
    [[nodiscard]] T* begin() noexcept { return data_; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] T* end() noexcept { return data_ ? data_ + size_ : nullptr; }
    [[nodiscard]] const T* end() const noexcept { return data_ ? data_ + size_ : nullptr; }

    void clear() noexcept { size_ = 0; }

    void resize(size_t count, const T& value = T{}) {
        Reserve(count);
        for (size_t i = size_; i < count; ++i)
            data_[i] = value;
        size_ = count;
    }

    void assign(size_t count, const T& value) {
        Reserve(count);
        for (size_t i = 0; i < count; ++i)
            data_[i] = value;
        size_ = count;
    }

    void push_back(const T& value) {
        if (size_ == capacity_) {
            const size_t maxCount = std::numeric_limits<size_t>::max() / sizeof(T);
            if (size_ == maxCount)
                tour::mem::ReportOomAndCrash("ArenaFlatArray", 1, size_, maxCount);
            const size_t grown = capacity_ > maxCount / 2 ? maxCount :
                std::max<size_t>(8, capacity_ * 2);
            Reserve(grown);
        }
        data_[size_++] = value;
    }

    void reserve(size_t requested) { Reserve(requested); }

    void Reserve(size_t requested) {
        if (requested <= capacity_) return;
        if (requested > std::numeric_limits<size_t>::max() / sizeof(T))
            tour::mem::ReportOomAndCrash("ArenaFlatArray", requested,
                capacity_, std::numeric_limits<size_t>::max() / sizeof(T));
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
    smvk::Arena arena_{4096};
    T* data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

} // namespace tour::mem
