// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/gpu_scene_upload.h>
#include <Engine/RHI/resource_manager.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace tour::gpu_scene {
namespace {
constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;

template <typename T>
struct Mirror {
    std::array<smvk::Holder<smvk::BufferHandle>, Flights> buffers{};
    std::array<size_t, Flights> capacityRows{};
};

template <typename Payload>
bool PublishTable(smvk::ResourceManager& ctx, Table<Payload, Flights>& table,
                  Mirror<Payload>& mirror, uint32_t flight, const char* name) {
    if (table.Size() == 0)
        return true; // empty table has a null GPU address

    bool fullCopy = false;
    if (mirror.capacityRows[flight] < table.Size() ||
        mirror.buffers[flight].empty()) {
        size_t cap = std::max<size_t>(16, mirror.capacityRows[flight]);
        while (cap < table.Size()) {
            if (cap > std::numeric_limits<size_t>::max() / 2)
                return false;
            cap *= 2;
        }
        if (cap > std::numeric_limits<size_t>::max() /
                  sizeof(typename Table<Payload, Flights>::Entry))
            return false;

        auto next = ctx.CreateBuffer(
            cap * sizeof(typename Table<Payload, Flights>::Entry),
            smvk::BufferType_Storage, name);
        if (next.empty())
            return false;
        // Holder replacement goes through ResourceManager's deferred
        // destruction, protecting older in-flight references.
        mirror.buffers[flight] = std::move(next);
        mirror.capacityRows[flight] = cap;
        fullCopy = true;
    }
    return table.Publish(flight, fullCopy, [&](size_t destination,
                                               const void* bytes,
                                               size_t count) {
        ctx.UploadDataToBuffer(
            mirror.buffers[flight], bytes, count,
            smvk::PipelineStage(smvk::PipelineStageBit_Compute),
            smvk::AccessFlags::ShaderRead, 0, destination);
        return true;
    });
}

template <typename Payload>
uint64_t Address(const smvk::ResourceManager& ctx,
                 const Mirror<Payload>& mirror, uint32_t flight) {
    if (flight >= Flights || mirror.buffers[flight].empty())
        return 0;
    return ctx.GetBufferDeviceAddress(mirror.buffers[flight]);
}
} // namespace

struct Uploader::Impl {
    Mirror<Mesh> meshes;
    Mirror<Lod> lods;
    Mirror<Submesh> submeshes;
    Mirror<Instance> instances;
    Mirror<Leaf> leaves;
    Mirror<LeafMember> leafMembers;
};

Uploader::Uploader() : impl_(std::make_unique<Impl>()) {}
Uploader::~Uploader() = default;

bool Uploader::Publish(smvk::ResourceManager& ctx, RenderDatabase& scene,
                       uint32_t flight) {
    if (flight >= Flights)
        return false;
    // The tables are independent publications. A failure retains pending
    // rows for the failing table; callers must NOT consume a partial frame
    // snapshot, and must retry Publish before recording GPU work.
    return PublishTable(ctx, scene.meshes, impl_->meshes, flight, "GPU scene meshes")
        && PublishTable(ctx, scene.lods, impl_->lods, flight, "GPU scene LODs")
        && PublishTable(ctx, scene.submeshes, impl_->submeshes, flight, "GPU scene submeshes")
        && PublishTable(ctx, scene.instances, impl_->instances, flight, "GPU scene instances")
        && PublishTable(ctx, scene.leaves, impl_->leaves, flight, "GPU scene leaves")
        && PublishTable(ctx, scene.leafMembers, impl_->leafMembers, flight,
                        "GPU scene leaf members");
}

Addresses Uploader::DeviceAddresses(const smvk::ResourceManager& ctx,
                                    uint32_t flight) const {
    return {
        Address(ctx, impl_->meshes, flight),
        Address(ctx, impl_->lods, flight),
        Address(ctx, impl_->submeshes, flight),
        Address(ctx, impl_->instances, flight),
        Address(ctx, impl_->leaves, flight),
        Address(ctx, impl_->leafMembers, flight)
    };
}

} // namespace tour::gpu_scene
