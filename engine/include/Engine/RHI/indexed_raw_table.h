// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Raw GPU table with stable externally assigned indices (mats[worldMaterialId]).
// CPU generations and dirty publication remain owned by RawTable; physical
// indices are append-only and removed tail slots are reset, never compacted.
#include <Engine/RHI/raw_table.h>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace smvk::table {

template <typename T, uint32_t FrameCount>
class IndexedRawTable {
    static_assert(std::is_trivially_copyable_v<T>);
    // Caller guarantees that T{} and every resolved payload have fully
    // initialized object representations. Do not use this with padded data.
    RawTable<T, FrameCount> table_;
    std::vector<typename RawTable<T, FrameCount>::Id> ids_;

public:
    using Id = typename RawTable<T, FrameCount>::Id;
    [[nodiscard]] uint32_t Size() const { return table_.Size(); }
    [[nodiscard]] size_t PendingRows(uint32_t flight) const {
        return table_.PendingRows(flight);
    }
    [[nodiscard]] const T* Get(uint32_t index) const {
        return index < ids_.size() ? table_.Get(ids_[index]) : nullptr;
    }
    // Resolve only an externally indexed row. No scanning of other rows.
    [[nodiscard]] bool Set(uint32_t index, const T& value) {
        if (index >= ids_.size())
            return false;
        const T* old = table_.Get(ids_[index]);
        if (!old)
            return false;
        return std::memcmp(old, &value, sizeof(T)) == 0 ||
               table_.Update(ids_[index], value);
    }
    // Grow with default rows so world slot identity remains 1:1.
    [[nodiscard]] bool EnsureSize(uint32_t count) {
        while (ids_.size() < count) {
            const auto id = table_.Insert(T{});
            if (!id)
                return false;
            ids_.push_back(id);
        }
        return true;
    }
    [[nodiscard]] bool ClearTail(uint32_t logicalCount) {
        if (logicalCount > ids_.size())
            return false;
        for (size_t i = logicalCount; i < ids_.size(); ++i)
            if (!Set(static_cast<uint32_t>(i), T{}))
                return false;
        return true;
    }
    // External indices remain stable. A scene shrinking from N rows to M
    // clears [M,N) in the GPU mirror even if another flight missed the edit.
    // Grow/reuse simply overwrites those slots with the new resolved value.
    [[nodiscard]] bool Sync(std::span<const T> resolved) {
        if (resolved.size() >= InvalidIndex)
            return false;
        if (!EnsureSize(static_cast<uint32_t>(resolved.size())))
            return false;
        for (size_t i = 0; i < resolved.size(); ++i)
            if (!Set(static_cast<uint32_t>(i), resolved[i]))
                return false;
        return ClearTail(static_cast<uint32_t>(resolved.size()));
        return true;
    }
    template <typename Writer>
    [[nodiscard]] bool Publish(uint32_t flight, bool forceFull, Writer&& writer) {
        return table_.Publish(flight, forceFull, std::forward<Writer>(writer));
    }
    void Reset() {
        table_ = {};
        ids_.clear();
    }
};

} // namespace smvk::table
