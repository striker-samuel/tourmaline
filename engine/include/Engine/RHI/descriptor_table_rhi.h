// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// EXPERIMENTAL: descriptor-addressable storage buffer publication.
// The caller still creates and binds the descriptor set and must ensure
// the acquired flight is fenced before writing. This does NOT enable a
// no-BDA shader path by itself and does not alter bindless textures.
#include <Engine/RHI/descriptor_table_mirror.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Memory/pool.h>
#include <optional>

namespace smvk::table {

template <typename T, uint32_t FrameCount>
class DescriptorStoragePublisher {
    using Owner = smvk::Holder<smvk::BufferHandle>;
    DescriptorTableMirror<T, FrameCount, Owner> mirror_;

public:
    struct Binding {
        smvk::BufferHandle buffer{};
        size_t capacityBytes = 0;
        uint32_t rowCount = 0;
        uint64_t allocationRevision = 0;
        explicit operator bool() const { return buffer.valid() && rowCount != 0; }
    };

    [[nodiscard]] bool Publish(smvk::ResourceManager& ctx,
                               RawTable<T, FrameCount>& table,
                               uint32_t flight) {
        return mirror_.Publish(
            table, flight,
            [&](size_t bytes) -> std::optional<Owner> {
                Owner created = ctx.CreateBuffer(bytes, smvk::BufferType_Storage,
                                                 "descriptor table (experimental)");
                if (created.empty())
                    return std::nullopt;
                return std::optional<Owner>(std::move(created));
            },
            [&](const Owner& buffer, size_t offset,
                const void* data, size_t bytes) -> bool {
                ctx.UploadDataToBuffer(
                    buffer, data, bytes,
                    smvk::PipelineStage(smvk::PipelineStageBit_Compute |
                                        smvk::PipelineStageBit_Vertex),
                    smvk::AccessFlags::ShaderRead, 0, offset);
                return true; // staging API has void return; caller flushes
            });
    }

    [[nodiscard]] Binding GetBinding(const RawTable<T, FrameCount>& table,
                                     uint32_t flight) const {
        const auto b = mirror_.GetBinding(table, flight);
        if (!b)
            return {};
        return {static_cast<smvk::BufferHandle>(*b.buffer),
                b.capacityBytes, b.rowCount, b.allocationRevision};
    }
};
} // namespace smvk::table
