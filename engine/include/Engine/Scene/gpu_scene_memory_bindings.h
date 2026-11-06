// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// The six published GPU scene streams, described with the same semantic
// vocabulary as CPU scene memory, but with explicitly per-flight addresses.
// These addresses belong to the Uploader/ResourceManager; this API does not
// transfer ownership or allow binding external GPU buffers.

#include <Engine/Renderer/gpu_scene_upload.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace tour::scene::memory {

enum class GpuSemantic : uint8_t {
    Mesh, Lod, Submesh, Instance, Leaf, LeafMember
};

struct GpuView {
    GpuSemantic semantic{};
    uint64_t deviceAddress = 0;
    size_t count = 0;
    size_t stride = 0;
    uint32_t flight = 0;
    size_t generationOffset = 0;
    size_t aliveOffset = 0;
    bool slotIndexed = true;

    [[nodiscard]] bool Published() const noexcept {
        return deviceAddress != 0;
    }
};

// Query after a successful uploader Publish() for a synchronized flight.
// Row slots include tombstones. GPU identity is a uvec4 following the
// payload: x=generation, y=alive. A nonzero address is NOT a lifetime
// guarantee and cannot be reused for another in-flight frame.
[[nodiscard]] inline std::array<GpuView, 6> GetGpuViews(
    const tour::gpu_scene::RenderDatabase& db,
    const tour::gpu_scene::Addresses& addresses,
    uint32_t flight) noexcept {
    using namespace tour::gpu_scene;
    constexpr size_t gMesh = offsetof(Row<Mesh>, identity);
    constexpr size_t gLod = offsetof(Row<Lod>, identity);
    constexpr size_t gSubmesh = offsetof(Row<Submesh>, identity);
    constexpr size_t gInstance = offsetof(Row<Instance>, identity);
    constexpr size_t gLeaf = offsetof(Row<Leaf>, identity);
    constexpr size_t gLeafMember = offsetof(Row<LeafMember>, identity);
    return {{
        { GpuSemantic::Mesh, addresses.meshes, db.meshes.Size(),
            sizeof(Row<Mesh>), flight, gMesh, gMesh + 4 },
        { GpuSemantic::Lod, addresses.lods, db.lods.Size(),
            sizeof(Row<Lod>), flight, gLod, gLod + 4 },
        { GpuSemantic::Submesh, addresses.submeshes, db.submeshes.Size(),
            sizeof(Row<Submesh>), flight, gSubmesh, gSubmesh + 4 },
        { GpuSemantic::Instance, addresses.instances, db.instances.Size(),
            sizeof(Row<Instance>), flight, gInstance, gInstance + 4 },
        { GpuSemantic::Leaf, addresses.leaves, db.leaves.Size(),
            sizeof(Row<Leaf>), flight, gLeaf, gLeaf + 4 },
        { GpuSemantic::LeafMember, addresses.leafMembers, db.leafMembers.Size(),
            sizeof(Row<LeafMember>), flight, gLeafMember, gLeafMember + 4 }
    }};
}

} // namespace tour::scene::memory
