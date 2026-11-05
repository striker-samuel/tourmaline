// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Experimental scene-memory input-assembler vocabulary.
//
// This is a BORROWED VIEW of the current Scene vectors, not a transfer of
// ownership. Views expire on scene mutation/reallocation or destruction.
// Use the existing Scene setters for mutations: writing directly into the
// exported memory bypasses dirty propagation and GPU publication.

#include <Engine/Scene/scene.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace tour::scene::memory {

enum class Semantic : uint32_t {
    LocalTransform,
    GlobalTransform,
    Hierarchy,
    NodeGeneration,
    NodeFreeList,
    NodeMetadata,
    RowsForNode,
    StaticRoot,
    StaticNode,
    Hidden,
    NoLightmap,
    Lightmap,
    DitherCloseup,
    CastsVolumetricShadow,
    GpuSceneGlobalChanges,
    GpuSceneGlobalQueued,
    MeshRows,
    MaterialRows,
    LightRows,
    CameraRows,
    DecalRows,
    GiVolumeRows,
    CubemapRows,
    ExportRows,
    EnvRows,
    EnvVolumeRows,
    AudioSourceRows,
    AudioRoomRows,
    TriggerVolumeRows,
    PhysicsBodyRows,
    SchemaAssocRows
};

enum class ScalarFormat : uint8_t {
    Opaque,
    UInt8,
    UInt32,
    Int32,
    Float32
};

enum class Access : uint8_t { ReadOnly, ReadWrite };
enum class Ownership : uint8_t { Borrowed, External };

struct Field {
    const char* semantic = nullptr;
    uint32_t offset = 0;
    ScalarFormat format = ScalarFormat::Opaque;
};

// Describes a strided memory stream. Not all streams are POD: table rows
// carrying strings/vectors are intentionally opaque and must NOT be
// serialized, relocated, or interpreted across ABI boundaries.
struct View {
    Semantic semantic{};
    const void* data = nullptr;
    size_t count = 0;
    size_t stride = 0;
    size_t alignment = 0;
    ScalarFormat format = ScalarFormat::Opaque;
    Access access = Access::ReadOnly;
    Ownership ownership = Ownership::Borrowed;

    [[nodiscard]] size_t ByteSize() const noexcept { return count * stride; }
    [[nodiscard]] bool Empty() const noexcept { return count == 0; }
};

template <class T>
[[nodiscard]] inline View MakeView(
    Semantic semantic, const std::vector<T>& values,
    ScalarFormat format = ScalarFormat::Opaque) noexcept {
    return { semantic, values.empty() ? nullptr : values.data(),
             values.size(), sizeof(T), alignof(T), format,
             Access::ReadOnly, Ownership::Borrowed };
}

// The five hierarchy fields are signed int32 on Tourmaline's supported
// platforms. Indices refer to stable scene slots; -1 means no relative.
// The nodeGeneration vector identifies retired/reused slots.
static_assert(sizeof(int) == 4, "Hierarchy requires 32-bit int fields");
static_assert(std::is_standard_layout_v<Hierarchy>);
inline constexpr Field HierarchyFields[] = {
    { "PARENT",       uint32_t(offsetof(Hierarchy, parent)),       ScalarFormat::Int32 },
    { "FIRST_CHILD",  uint32_t(offsetof(Hierarchy, firstChild)),   ScalarFormat::Int32 },
    { "NEXT_SIBLING", uint32_t(offsetof(Hierarchy, nextSibling)),  ScalarFormat::Int32 },
    { "LAST_SIBLING", uint32_t(offsetof(Hierarchy, lastSibling)),  ScalarFormat::Int32 },
    { "LEVEL",        uint32_t(offsetof(Hierarchy, level)),        ScalarFormat::Int32 }
};

inline constexpr uint32_t InvalidHierarchyLink = 0xFFFFFFFFu;

// Descriptors remain useful even when a stream is empty. The caller must
// re-query after vector growth, scene merging, node retirement, or reload.
[[nodiscard]] inline View GetView(const Scene& scene, Semantic semantic) noexcept {
    switch (semantic) {
    case Semantic::LocalTransform: return MakeView(semantic, scene.localTransform);
    case Semantic::GlobalTransform: return MakeView(semantic, scene.globalTransform);
    case Semantic::Hierarchy: return MakeView(semantic, scene.hierarchy);
    case Semantic::NodeGeneration: return MakeView(semantic, scene.nodeGeneration, ScalarFormat::UInt32);
    case Semantic::NodeFreeList: return MakeView(semantic, scene.nodeFreeList, ScalarFormat::UInt32);
    case Semantic::NodeMetadata: return MakeView(semantic, scene.metadata);
    case Semantic::RowsForNode: return MakeView(semantic, scene.rowsForNode);
    case Semantic::StaticRoot: return MakeView(semantic, scene.staticRoot, ScalarFormat::UInt8);
    case Semantic::StaticNode: return MakeView(semantic, scene.staticNode, ScalarFormat::UInt8);
    case Semantic::Hidden: return MakeView(semantic, scene.hidden, ScalarFormat::UInt8);
    case Semantic::NoLightmap: return MakeView(semantic, scene.noLightmap, ScalarFormat::UInt8);
    case Semantic::Lightmap: return MakeView(semantic, scene.lightmap, ScalarFormat::UInt8);
    case Semantic::DitherCloseup: return MakeView(semantic, scene.ditherCloseup, ScalarFormat::UInt8);
    case Semantic::CastsVolumetricShadow: return MakeView(semantic, scene.castsVolumetricShadow, ScalarFormat::UInt8);
    case Semantic::GpuSceneGlobalChanges: return MakeView(semantic, scene.gpuSceneGlobalChanges, ScalarFormat::UInt32);
    case Semantic::GpuSceneGlobalQueued: return MakeView(semantic, scene.gpuSceneGlobalQueued, ScalarFormat::UInt8);
    case Semantic::MeshRows: return MakeView(semantic, scene.tables.meshes);
    case Semantic::MaterialRows: return MakeView(semantic, scene.tables.materials);
    case Semantic::LightRows: return MakeView(semantic, scene.tables.lights);
    case Semantic::CameraRows: return MakeView(semantic, scene.tables.cameras);
    case Semantic::DecalRows: return MakeView(semantic, scene.tables.decals);
    case Semantic::GiVolumeRows: return MakeView(semantic, scene.tables.giVolumes);
    case Semantic::CubemapRows: return MakeView(semantic, scene.tables.cubemaps);
    case Semantic::ExportRows: return MakeView(semantic, scene.tables.exports);
    case Semantic::EnvRows: return MakeView(semantic, scene.tables.envs);
    case Semantic::EnvVolumeRows: return MakeView(semantic, scene.tables.envVolumes);
    case Semantic::AudioSourceRows: return MakeView(semantic, scene.tables.audioSources);
    case Semantic::AudioRoomRows: return MakeView(semantic, scene.tables.audioRooms);
    case Semantic::TriggerVolumeRows: return MakeView(semantic, scene.tables.triggerVolumes);
    case Semantic::PhysicsBodyRows: return MakeView(semantic, scene.tables.physicsBodies);
    case Semantic::SchemaAssocRows: return MakeView(semantic, scene.tables.schemaAssocs);
    }
    return {};
}

// Input binding describes caller-owned storage for a future zero-copy scene
// backend. It does NOT attach the pointer to Scene or make Scene's vectors
// external: the current build deliberately exposes only query-only views.
struct ExternalBinding {
    Semantic semantic{};
    void* data = nullptr;
    size_t count = 0;
    size_t capacity = 0;
    size_t stride = 0;
    size_t alignment = 0;
    Access access = Access::ReadWrite;
};

// Validate only the generic byte/stride contract. Semantic-specific layouts,
// index constraints, pointer lifetime and synchronization remain the caller's
// responsibility until a storage-backend implementation is introduced.
[[nodiscard]] inline bool Validate(const ExternalBinding& b) noexcept {
    if (b.count > b.capacity || b.stride == 0 || b.alignment == 0)
        return false;
    if ((b.alignment & (b.alignment - 1)) != 0)
        return false;
    if (b.capacity && !b.data)
        return false;
    if (b.capacity > SIZE_MAX / b.stride)
        return false;
    if (b.data && reinterpret_cast<uintptr_t>(b.data) % b.alignment != 0)
        return false;
    return true;
}

} // namespace tour::scene::memory
