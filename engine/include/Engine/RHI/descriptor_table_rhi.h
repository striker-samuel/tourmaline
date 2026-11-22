// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// EXPERIMENTAL: descriptor-addressable storage buffer publication.
// The caller still creates and binds the descriptor set and must ensure
// the acquired flight is fenced before writing. This does NOT enable a
// no-BDA shader path by itself and does not alter bindless textures.
#include <Engine/Memory/pool.h>
#include <Engine/RHI/descriptor_table_mirror.h>
#include <Engine/RHI/resource_manager.h>
#include <optional>

namespace smvk::table {

template <typename T, uint32_t FrameCount> class DescriptorStoragePublisher {
    using Owner = smvk::Holder<smvk::BufferHandle>;
    DescriptorTableMirror<T, FrameCount, Owner> mirror_;

  public:
    // Publication state. Publish() STAGES the copies (queued) — the buffer is
    // not GPU-visible until the caller Flush()es. A bare bool made it easy to
    // confuse "queued" with "usable"; the enum makes the two explicit.
    enum class State {
        Queued, // copies staged for the flight; flush before binding to shaders
        Failed, // buffer creation or a validation gate refused; no binding
    };

    struct Binding {
        smvk::BufferHandle buffer{};
        size_t capacityBytes = 0;
        uint32_t rowCount = 0;
        uint64_t allocationRevision = 0;
        explicit operator bool() const {
            return buffer.valid() && rowCount != 0;
        }
    };

    // dstStages/dstAccess name the stages that CONSUME the buffer. The default
    // covers every shader consumer (compute + vertex + fragment): Forward+
    // lighting, decals and materials all read these tables from the FRAGMENT
    // stage, so an upload that omitted Fragment would leave the transfer
    // unsynchronized against fragment reads (a transfer-to-fragment hazard).
    [[nodiscard]] State Publish(smvk::ResourceManager& ctx,
                                RawTable<T, FrameCount>& table,
                                uint32_t flight,
                                smvk::PipelineStage dstStages = smvk::PipelineStage(
                                    smvk::PipelineStageBit_Compute | smvk::PipelineStageBit_Vertex |
                                    smvk::PipelineStageBit_Fragment),
                                smvk::AccessFlags dstAccess = smvk::AccessFlags::ShaderRead) {
        const bool ok = mirror_.Publish(
            table,
            flight,
            [&](size_t bytes) -> std::optional<Owner> {
                Owner created = ctx.CreateBuffer(
                    bytes, smvk::BufferType_Storage, "descriptor table (experimental)");
                if (created.empty())
                    return std::nullopt;
                return std::optional<Owner>(std::move(created));
            },
            [&](const Owner& buffer, size_t offset, const void* data, size_t bytes) -> bool {
                ctx.UploadDataToBuffer(buffer, data, bytes, dstStages, dstAccess, 0, offset);
                return true; // the staging API queues; Flush() submits
            });
        return ok ? State::Queued : State::Failed;
    }

    // Submits the staged uploads (the caller's flush). Until this runs, the
    // published bindings are a QUEUED snapshot, not the GPU-visible one.
    void Flush(smvk::ResourceManager& ctx) {
        ctx.FlushUploads();
    }

    [[nodiscard]] Binding GetBinding(const RawTable<T, FrameCount>& table, uint32_t flight) const {
        const auto b = mirror_.GetBinding(table, flight);
        if (!b)
            return {};
        return {static_cast<smvk::BufferHandle>(*b.buffer),
                b.capacityBytes,
                b.rowCount,
                b.allocationRevision};
    }
};
} // namespace smvk::table
