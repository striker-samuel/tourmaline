// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Experimental scene-memory input-assembler vocabulary.
//
// Views are borrowed; the authoritative arena-backed streams can also use
// caller-owned contiguous storage. Exported views expire on reallocations.
// Use the existing Scene setters for mutations: writing directly into the
// exported memory bypasses dirty propagation and GPU publication.

#include <Engine/Scene/scene.h>

#include <array>
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
    Float32,
    Float32x4,
    Mat4x4Float32
};

enum class Access : uint8_t { ReadOnly, ReadWrite };
enum class Ownership : uint8_t { Borrowed, External };

struct Field {
    const char* semantic = nullptr;
    uint32_t offset = 0;
    ScalarFormat format = ScalarFormat::Opaque;
    uint8_t components = 1;
};

// Describes a strided memory stream. Not all streams are POD: table rows
// carrying strings/vectors are intentionally opaque and must NOT be
// serialized, relocated, or interpreted across ABI boundaries.
struct View {
    Semantic semantic{};
    const void* data = nullptr;
    size_t count = 0;
    size_t capacity = 0;
    size_t stride = 0;
    size_t alignment = 0;
    ScalarFormat format = ScalarFormat::Opaque;
    Access access = Access::ReadOnly;
    Ownership ownership = Ownership::Borrowed;

    [[nodiscard]] size_t ByteSize() const noexcept {
        return count * stride;
    }
    [[nodiscard]] bool Empty() const noexcept {
        return count == 0;
    }
};

template <class Container>
[[nodiscard]] inline View MakeView(Semantic semantic,
                                   const Container& values,
                                   ScalarFormat format = ScalarFormat::Opaque) noexcept {
    using T = typename Container::value_type;
    Ownership backing = Ownership::Borrowed;
    if constexpr (requires { values.IsExternal(); })
        backing = values.IsExternal() ? Ownership::External : Ownership::Borrowed;
    return {semantic,
            values.empty() ? nullptr : values.data(),
            values.size(),
            values.capacity(),
            sizeof(T),
            alignof(T),
            format,
            Access::ReadOnly,
            backing};
}

// The five hierarchy fields are signed int32 on Tourmaline's supported
// platforms. Indices refer to stable scene slots; -1 means no relative.
// The nodeGeneration stream identifies retired/reused slots.
static_assert(sizeof(int) == 4, "Hierarchy requires 32-bit int fields");
static_assert(std::is_standard_layout_v<Hierarchy>);
inline constexpr Field HierarchyFields[] = {
    {"PARENT", uint32_t(offsetof(Hierarchy, parent)), ScalarFormat::Int32},
    {"FIRST_CHILD", uint32_t(offsetof(Hierarchy, firstChild)), ScalarFormat::Int32},
    {"NEXT_SIBLING", uint32_t(offsetof(Hierarchy, nextSibling)), ScalarFormat::Int32},
    {"LAST_SIBLING", uint32_t(offsetof(Hierarchy, lastSibling)), ScalarFormat::Int32},
    {"LEVEL", uint32_t(offsetof(Hierarchy, level)), ScalarFormat::Int32}};

inline constexpr uint32_t InvalidHierarchyLink = 0xFFFFFFFFu;

// Descriptors remain useful even when a stream is empty. The caller must
// re-query after array growth, scene merging, node retirement, or reload.
[[nodiscard]] inline View GetView(const Scene& scene, Semantic semantic) noexcept {
    switch (semantic) {
    case Semantic::LocalTransform:
        return MakeView(semantic, scene.localTransform, ScalarFormat::Mat4x4Float32);
    case Semantic::GlobalTransform:
        return MakeView(semantic, scene.globalTransform, ScalarFormat::Mat4x4Float32);
    case Semantic::Hierarchy:
        return MakeView(semantic, scene.hierarchy);
    case Semantic::NodeGeneration:
        return MakeView(semantic, scene.nodeGeneration, ScalarFormat::UInt32);
    case Semantic::NodeFreeList:
        return MakeView(semantic, scene.nodeFreeList, ScalarFormat::UInt32);
    case Semantic::NodeMetadata:
        return MakeView(semantic, scene.metadata);
    case Semantic::RowsForNode:
        return MakeView(semantic, scene.rowsForNode);
    case Semantic::StaticRoot:
        return MakeView(semantic, scene.staticRoot, ScalarFormat::UInt8);
    case Semantic::StaticNode:
        return MakeView(semantic, scene.staticNode, ScalarFormat::UInt8);
    case Semantic::Hidden:
        return MakeView(semantic, scene.hidden, ScalarFormat::UInt8);
    case Semantic::NoLightmap:
        return MakeView(semantic, scene.noLightmap, ScalarFormat::UInt8);
    case Semantic::Lightmap:
        return MakeView(semantic, scene.lightmap, ScalarFormat::UInt8);
    case Semantic::DitherCloseup:
        return MakeView(semantic, scene.ditherCloseup, ScalarFormat::UInt8);
    case Semantic::CastsVolumetricShadow:
        return MakeView(semantic, scene.castsVolumetricShadow, ScalarFormat::UInt8);
    case Semantic::GpuSceneGlobalChanges:
        return MakeView(semantic, scene.gpuSceneGlobalChanges, ScalarFormat::UInt32);
    case Semantic::GpuSceneGlobalQueued:
        return MakeView(semantic, scene.gpuSceneGlobalQueued, ScalarFormat::UInt8);
    case Semantic::MeshRows:
        return MakeView(semantic, scene.tables.meshes);
    case Semantic::MaterialRows:
        return MakeView(semantic, scene.tables.materials);
    case Semantic::LightRows:
        return MakeView(semantic, scene.tables.lights);
    case Semantic::CameraRows:
        return MakeView(semantic, scene.tables.cameras);
    case Semantic::DecalRows:
        return MakeView(semantic, scene.tables.decals);
    case Semantic::GiVolumeRows:
        return MakeView(semantic, scene.tables.giVolumes);
    case Semantic::CubemapRows:
        return MakeView(semantic, scene.tables.cubemaps);
    case Semantic::ExportRows:
        return MakeView(semantic, scene.tables.exports);
    case Semantic::EnvRows:
        return MakeView(semantic, scene.tables.envs);
    case Semantic::EnvVolumeRows:
        return MakeView(semantic, scene.tables.envVolumes);
    case Semantic::AudioSourceRows:
        return MakeView(semantic, scene.tables.audioSources);
    case Semantic::AudioRoomRows:
        return MakeView(semantic, scene.tables.audioRooms);
    case Semantic::TriggerVolumeRows:
        return MakeView(semantic, scene.tables.triggerVolumes);
    case Semantic::PhysicsBodyRows:
        return MakeView(semantic, scene.tables.physicsBodies);
    case Semantic::SchemaAssocRows:
        return MakeView(semantic, scene.tables.schemaAssocs);
    }
    return {};
}

// Input assembler field layouts for the ABI of this build. Parent/child
// references are signed node slots (-1 is invalid). GLM matrices are
// column-major; each column is four float32 values.
static_assert(sizeof(glm::mat4) == 64);
inline constexpr Field TransformFields[] = {{"COLUMN0", 0, ScalarFormat::Float32x4, 4},
                                            {"COLUMN1", 16, ScalarFormat::Float32x4, 4},
                                            {"COLUMN2", 32, ScalarFormat::Float32x4, 4},
                                            {"COLUMN3", 48, ScalarFormat::Float32x4, 4}};

struct Layout {
    Semantic semantic{};
    size_t stride = 0;
    size_t alignment = 0;
    ScalarFormat format = ScalarFormat::Opaque;
    const Field* fields = nullptr;
    size_t fieldCount = 0;
    bool nodeIndexed = false;
    bool externalBindingSupported = false;
};

[[nodiscard]] inline Layout Describe(const Scene& scene, Semantic semantic) noexcept {
    const View view = GetView(scene, semantic);
    Layout info{semantic, view.stride, view.alignment, view.format};
    switch (semantic) {
    case Semantic::Hierarchy:
        info.fields = HierarchyFields;
        info.fieldCount = std::size(HierarchyFields);
        info.nodeIndexed = true;
        info.externalBindingSupported = true;
        break;
    case Semantic::LocalTransform:
    case Semantic::GlobalTransform:
        info.fields = TransformFields;
        info.fieldCount = std::size(TransformFields);
        info.nodeIndexed = true;
        info.externalBindingSupported = true;
        break;
    case Semantic::NodeGeneration:
    case Semantic::NodeMetadata:
    case Semantic::RowsForNode:
    case Semantic::StaticRoot:
    case Semantic::StaticNode:
    case Semantic::Hidden:
    case Semantic::NoLightmap:
    case Semantic::Lightmap:
    case Semantic::DitherCloseup:
    case Semantic::CastsVolumetricShadow:
    case Semantic::GpuSceneGlobalQueued:
        info.nodeIndexed = true;
        break;
    default:
        break;
    }
    return info;
}

inline constexpr auto AllSemantics = std::to_array<Semantic>({Semantic::LocalTransform,
                                                              Semantic::GlobalTransform,
                                                              Semantic::Hierarchy,
                                                              Semantic::NodeGeneration,
                                                              Semantic::NodeFreeList,
                                                              Semantic::NodeMetadata,
                                                              Semantic::RowsForNode,
                                                              Semantic::StaticRoot,
                                                              Semantic::StaticNode,
                                                              Semantic::Hidden,
                                                              Semantic::NoLightmap,
                                                              Semantic::Lightmap,
                                                              Semantic::DitherCloseup,
                                                              Semantic::CastsVolumetricShadow,
                                                              Semantic::GpuSceneGlobalChanges,
                                                              Semantic::GpuSceneGlobalQueued,
                                                              Semantic::MeshRows,
                                                              Semantic::MaterialRows,
                                                              Semantic::LightRows,
                                                              Semantic::CameraRows,
                                                              Semantic::DecalRows,
                                                              Semantic::GiVolumeRows,
                                                              Semantic::CubemapRows,
                                                              Semantic::ExportRows,
                                                              Semantic::EnvRows,
                                                              Semantic::EnvVolumeRows,
                                                              Semantic::AudioSourceRows,
                                                              Semantic::AudioRoomRows,
                                                              Semantic::TriggerVolumeRows,
                                                              Semantic::PhysicsBodyRows,
                                                              Semantic::SchemaAssocRows});

template <typename Visitor> inline void ForEachView(const Scene& scene, Visitor&& visit) {
    for (Semantic semantic : AllSemantics)
        visit(GetView(scene, semantic), Describe(scene, semantic));
}

// Caller-owned backing is supported for three authoritative scene streams.
// The buffer must be contiguous (stride == sizeof element).
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

// Bind exact-layout, writable caller storage. Existing scene values are
// copied into the supplied memory. The caller retains ownership and MUST
// detach before freeing it. Growth beyond its capacity aborts (OOM policy).
[[nodiscard]] inline bool BindExternal(Scene& scene, const ExternalBinding& b) {
    if (!Validate(b) || b.access != Access::ReadWrite || !b.capacity ||
        b.count != GetView(scene, b.semantic).count)
        return false;
    switch (b.semantic) {
    case Semantic::LocalTransform:
        return b.stride == sizeof(glm::mat4) && b.alignment >= alignof(glm::mat4) &&
               scene.localTransform.BindExternal(static_cast<glm::mat4*>(b.data), b.capacity);
    case Semantic::GlobalTransform:
        return b.stride == sizeof(glm::mat4) && b.alignment >= alignof(glm::mat4) &&
               scene.globalTransform.BindExternal(static_cast<glm::mat4*>(b.data), b.capacity);
    case Semantic::Hierarchy:
        return b.stride == sizeof(Hierarchy) && b.alignment >= alignof(Hierarchy) &&
               scene.hierarchy.BindExternal(static_cast<Hierarchy*>(b.data), b.capacity);
    default:
        return false;
    }
}

[[nodiscard]] inline bool DetachExternal(Scene& scene, Semantic semantic) {
    switch (semantic) {
    case Semantic::LocalTransform:
        scene.localTransform.DetachExternal();
        return true;
    case Semantic::GlobalTransform:
        scene.globalTransform.DetachExternal();
        return true;
    case Semantic::Hierarchy:
        scene.hierarchy.DetachExternal();
        return true;
    default:
        return false;
    }
}

} // namespace tour::scene::memory
