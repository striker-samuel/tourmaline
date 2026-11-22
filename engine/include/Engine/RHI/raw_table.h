// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Canonical generational CPU table with a legacy raw-payload GPU view.
// Keeps identity in the CPU table without changing the shader's T[] ABI.
// The writer sees contiguous packed T records, never Row<T> trailers.
#include <Engine/RHI/table.h>
#include <cstring>
#include <type_traits>
#include <vector>

namespace smvk::table {

// GPU record ABI. The shader's T[] row stride is sizeof(T); the field OFFSETS
// must still match the HLSL mirror (the per-record sizeof(T) static_assert at
// the consumer is that layout contract). This trait enforces the size/stride
// rules the raw packed upload relies on: a trivially-copyable standard-layout
// type whose alignment is a power of two <= 16 and whose size is a multiple of
// that alignment (tightly packed rows, no C++-inserted tail padding).
template <typename T> struct GpuRecordTraits {
    static_assert(std::is_trivially_copyable_v<T>, "GPU record must be trivially copyable");
    static_assert(std::is_standard_layout_v<T>, "GPU record must be standard layout");
    static_assert(alignof(T) == 1 || alignof(T) == 2 || alignof(T) == 4 || alignof(T) == 8 ||
                      alignof(T) == 16,
                  "GPU record alignment must be a power of two <= 16");
    static_assert(sizeof(T) % alignof(T) == 0,
                  "GPU record size must be a multiple of its alignment (no tail padding)");
    static constexpr size_t kStride = sizeof(T);
};

template <typename T> struct alignas(16) RawStorage {
    T payload{};
};

template <typename T, uint32_t FrameCount> class RawTable {
    static_assert(GpuRecordTraits<T>::kStride > 0, "GPU record ABI validation failed");
    using Storage = RawStorage<T>;
    Table<Storage, FrameCount> table_;

  public:
    using Id = Handle<Storage>;
    [[nodiscard]] Id Insert(const T& value) {
        return table_.Insert(Storage{value});
    }
    [[nodiscard]] bool Update(Id id, const T& value) {
        return table_.Update(id, Storage{value});
    }
    [[nodiscard]] bool Erase(Id id) {
        return table_.Erase(id);
    }
    [[nodiscard]] bool Contains(Id id) const {
        return table_.Contains(id);
    }
    [[nodiscard]] const T* Get(Id id) const {
        const Storage* row = table_.Get(id);
        return row ? &row->payload : nullptr;
    }
    [[nodiscard]] uint32_t Size() const {
        return table_.Size();
    }
    [[nodiscard]] uint32_t LiveCount() const {
        return table_.LiveCount();
    }
    [[nodiscard]] size_t PendingRows(uint32_t flight) const {
        return table_.PendingRows(flight);
    }
    // Writer: bool(destination byte offset, packed T pointer, byte count).
    // The packed run uses a reused member scratch (resized once to the largest
    // range), never a per-range heap allocation — a fragmented update reuses
    // the same buffer instead of churning many small mallocs. A failed writer
    // leaves the underlying per-flight journal intact; forceFull is still
    // required when the destination buffer was freshly allocated.
    template <typename Writer>
    [[nodiscard]] bool Publish(uint32_t flight, bool forceFull, Writer&& writer) {
        using Entry = typename Table<Storage, FrameCount>::Entry;
        return table_.Publish(flight, forceFull, [&](size_t offset, const void* src, size_t bytes) {
            if (offset % sizeof(Entry) || bytes % sizeof(Entry))
                return false;
            const size_t begin = offset / sizeof(Entry);
            const size_t count = bytes / sizeof(Entry);
            const Entry* entries = static_cast<const Entry*>(src);
            if (scratch_.size() < count)
                scratch_.resize(count);
            for (size_t i = 0; i < count; ++i)
                scratch_[i] = entries[i].identity.y ? entries[i].value.payload : T{};
            return writer(begin * sizeof(T), scratch_.data(), count * sizeof(T));
        });
    }

  private:
    std::vector<T> scratch_;
};
} // namespace smvk::table
