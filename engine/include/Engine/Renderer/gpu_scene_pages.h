// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU-scene storage keeps a single contiguous buffer device address per table.
// A "page" here is a fixed row-capacity growth quantum INSIDE that buffer,
// not an independently bound VkBuffer; existing shaders index rows directly.

#include <Engine/Renderer/gpu_scene.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace tour::gpu_scene {

inline constexpr size_t kGpuScenePageBytes = 64u * 1024u;

template <typename Payload> struct TablePageLayout {
    using Entry = Row<Payload>;
    static_assert(sizeof(Entry) <= kGpuScenePageBytes);
    static constexpr size_t EntryBytes = sizeof(Entry);
    static constexpr size_t RowsPerPage = kGpuScenePageBytes / EntryBytes;
    static constexpr size_t PageBytes = RowsPerPage * EntryBytes;
    static_assert(RowsPerPage > 0);
    static_assert(PageBytes <= kGpuScenePageBytes);

    // Deterministic capacity; no allocation until another complete logical
    // page is needed. The final partial page is always reserved in full.
    [[nodiscard]] static constexpr bool Capacity(size_t rows, size_t& capacity) noexcept {
        if (rows == 0) {
            capacity = 0;
            return true;
        }
        const size_t pages = 1 + (rows - 1) / RowsPerPage;
        if (pages > size_t(std::numeric_limits<uint32_t>::max()) / RowsPerPage ||
            pages > std::numeric_limits<size_t>::max() / RowsPerPage)
            return false;
        capacity = pages * RowsPerPage;
        return true;
    }

    [[nodiscard]] static constexpr bool Bytes(size_t rows, size_t& bytes) noexcept {
        size_t cap = 0;
        if (!Capacity(rows, cap) || cap > std::numeric_limits<size_t>::max() / EntryBytes)
            return false;
        bytes = cap * EntryBytes;
        return true;
    }
};

} // namespace tour::gpu_scene
