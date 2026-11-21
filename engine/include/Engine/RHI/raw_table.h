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

template <typename T> struct alignas(16) RawStorage {
    T payload{};
};

template <typename T, uint32_t FrameCount>
class RawTable {
    static_assert(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>);
    using Storage = RawStorage<T>;
    Table<Storage, FrameCount> table_;

public:
    using Id = Handle<Storage>;
    [[nodiscard]] Id Insert(const T& value) { return table_.Insert(Storage{value}); }
    [[nodiscard]] bool Update(Id id, const T& value) {
        return table_.Update(id, Storage{value});
    }
    [[nodiscard]] bool Erase(Id id) { return table_.Erase(id); }
    [[nodiscard]] bool Contains(Id id) const { return table_.Contains(id); }
    [[nodiscard]] const T* Get(Id id) const {
        const Storage* row = table_.Get(id);
        return row ? &row->payload : nullptr;
    }
    [[nodiscard]] uint32_t Size() const { return table_.Size(); }
    [[nodiscard]] uint32_t LiveCount() const { return table_.LiveCount(); }
    [[nodiscard]] size_t PendingRows(uint32_t flight) const {
        return table_.PendingRows(flight);
    }
    // Writer: bool(destination byte offset, packed T pointer, byte count).
    // One temporary packed run per dirty range. A failed writer leaves the
    // underlying per-flight journal intact; forceFull is still required
    // when the destination buffer was freshly allocated.
    template <typename Writer>
    [[nodiscard]] bool Publish(uint32_t flight, bool forceFull, Writer&& writer) {
        using Entry = typename Table<Storage, FrameCount>::Entry;
        return table_.Publish(flight, forceFull,
            [&](size_t offset, const void* src, size_t bytes) {
                if (offset % sizeof(Entry) || bytes % sizeof(Entry))
                    return false;
                const size_t begin = offset / sizeof(Entry);
                const size_t count = bytes / sizeof(Entry);
                const Entry* entries = static_cast<const Entry*>(src);
                std::vector<T> packed(count);
                for (size_t i = 0; i < count; ++i)
                    if (entries[i].identity.y)
                        packed[i] = entries[i].value.payload;
                return writer(begin * sizeof(T), packed.data(), count * sizeof(T));
            });
    }
};
} // namespace smvk::table
