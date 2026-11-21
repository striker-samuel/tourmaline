// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// EXPERIMENTAL: per-flight raw-table mirror for descriptor-bound storage buffers.
// No Vulkan dependency, BDA request, texture binding, or descriptor-set API.
#include <Engine/RHI/raw_table.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace smvk::table {

// Buffer is an owning, move-only token (e.g. Holder<BufferHandle>).
// Create: std::optional<Buffer>(size_t capacityBytes).
// Write: bool(const Buffer&, size_t offsetBytes, const void*, size_t bytes).
// The caller binds the returned buffer as a storage-buffer descriptor;
// this helper does not alter shaders or silently use a device address.
template <typename T, uint32_t FrameCount, typename Buffer>
class DescriptorTableMirror {
    static_assert(FrameCount > 0);
    static constexpr size_t kPageBytes = 64u * 1024u;
    static constexpr size_t kRowsPerPage =
        sizeof(T) > kPageBytes ? size_t(1) : kPageBytes / sizeof(T);

public:
    struct Binding {
        const Buffer* buffer = nullptr; // valid while mirror remains alive
        size_t capacityBytes = 0;
        uint32_t rowCount = 0;          // includes tombstones
        uint64_t allocationRevision = 0;
        explicit operator bool() const { return buffer != nullptr && rowCount != 0; }
    };

    template <typename Create, typename Write>
    [[nodiscard]] bool Publish(RawTable<T, FrameCount>& table, uint32_t flight,
                               Create&& create, Write&& write) {
        if (flight >= FrameCount)
            return false;
        const size_t rows = table.Size();
        if (rows == 0) {
            // No descriptor is exposed for an empty table.
            ready_[flight] = false;
            return true;
        }
        if (rows > std::numeric_limits<size_t>::max() / sizeof(T))
            return false;
        if (rows > std::numeric_limits<size_t>::max() - (kRowsPerPage - 1))
            return false;
        const size_t pageCount = 1 + (rows - 1) / kRowsPerPage;
        if (pageCount > std::numeric_limits<size_t>::max() / kRowsPerPage)
            return false;
        const size_t capacityRows = pageCount * kRowsPerPage;
        if (capacityRows > std::numeric_limits<size_t>::max() / sizeof(T))
            return false;
        const size_t neededBytes = capacityRows * sizeof(T);
        if (!buffers_[flight] || capacityBytes_[flight] < rows * sizeof(T)) {
            // A previous mirror is undersized for the new logical table.
            // Never expose it as a valid descriptor if allocation fails.
            ready_[flight] = false;
            std::optional<Buffer> fresh = create(neededBytes);
            if (!fresh)
                return false;
            buffers_[flight] = std::move(*fresh);
            capacityBytes_[flight] = neededBytes;
            ++revisions_[flight];
            forceFull_[flight] = true;
            ready_[flight] = false;
        }
        const bool ok = table.Publish(
            flight, forceFull_[flight],
            [&](size_t offset, const void* src, size_t bytes) {
                return write(*buffers_[flight], offset, src, bytes);
            });
        if (!ok) {
            ready_[flight] = false;
            return false; // must replay full snapshot after a failed copy
        }
        forceFull_[flight] = false;
        ready_[flight] = true;
        return true;
    }

    [[nodiscard]] Binding GetBinding(const RawTable<T, FrameCount>& table,
                                     uint32_t flight) const {
        if (flight >= FrameCount || !ready_[flight] || !buffers_[flight] ||
            table.Size() == 0 ||
            size_t(table.Size()) > capacityBytes_[flight] / sizeof(T))
            return {};
        return {&*buffers_[flight], capacityBytes_[flight], table.Size(),
                revisions_[flight]};
    }

private:
    std::array<std::optional<Buffer>, FrameCount> buffers_{};
    std::array<size_t, FrameCount> capacityBytes_{};
    std::array<uint64_t, FrameCount> revisions_{};
    std::array<bool, FrameCount> forceFull_{};
    std::array<bool, FrameCount> ready_{};
};

} // namespace smvk::table
