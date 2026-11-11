// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Load-time STATIC geometry registration for the GPU-scene raster backends
// (task/mesh + indexed raster). This is the physical-geometry half the retired
// gfx-filter `State` used to own: the cluster/meshlet tables + the global and
// meshlet index streams, cooked once at load, uploaded once, and shared across
// frames. The GPU-scene RenderDatabase owns IDENTITY (instances/meshes/LODs/
// submeshes); this owns the cooked cluster/meshlet BYTES — one authority, no
// double bookkeeping.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/rendergraph.h>

namespace tour {
struct StaticMeshUpload;
}
namespace smvk {
using ::tour::StaticMeshUpload;
struct CookedMeshlets;
} // namespace smvk

namespace tour {
namespace gfx {

// Clusters (meshlets) cap: the mesh-shader workgroup covers a WHOLE meshlet,
// so a cluster bigger than this would drop geometry (see meshlet.h's
// kMeshletMaxTriangles, the tighter cook-time limit).
static constexpr uint32_t kMaxClusterTris = 128;
static constexpr uint32_t kMaxMeshes = 8192;

// Cluster record (CPU). The task/mesh + indexed raster backends read the GPU
// mirror (ClusterConstants below) from the cluster buffer; the filtered
// indexOffset/triCount and the meshlet vertex/triangle windows ride the SAME
// record so both backends address one cluster.
struct Cluster {
    uint32_t mesh = 0;
    uint32_t indexOffset = 0; // into StaticGeometry::filterIndices (uints)
    uint32_t triCount = 0;
    uint32_t vertexOffset = 0;   // into StaticGeometry::meshletVertices (uints)
    uint32_t vertexCount = 0;    // unique local vertices
    uint32_t triangleOffset = 0; // into StaticGeometry::meshletTriangles (uints)
    glm::vec3 boundCenter{0.0f};
    float boundRadius = 0.0f;
    glm::vec3 coneApex{0.0f};
    glm::vec3 coneAxis{0.0f, 1.0f, 0.0f};
    float coneCutoff = 1.0f;
};

struct Mesh {
    uint32_t clusterStart = 0;
    uint32_t clusterCount = 0;
};

// GPU byte-mirror of Cluster (uploaded by UploadStaticData).
struct ClusterConstants {
    uint32_t indexOffset = 0; // into the cluster-ordered index buffer (uints)
    uint32_t triCount = 0;
    uint32_t vertexOffset = 0; // into the meshlet unique-vertex buffer (uints)
    uint32_t vertexCount = 0;  // unique local vertices
    glm::vec3 boundCenter{0.0f};
    float boundRadius = 0.0f;
    glm::vec3 coneApex{0.0f};
    float coneCutoff = 1.0f;
    glm::vec3 coneAxis{0.0f, 1.0f, 0.0f};
    float conePad = 0.0f;
    uint32_t triangleOffset = 0; // into the meshlet local-triangle buffer (uints)
    uint32_t pad1[3] = {};
};
static_assert(sizeof(ClusterConstants) == 80);

// The static registration surface: plain data, one instance owned by the
// app renderer. Frozen after UploadStaticData, safe to share across frames.
struct StaticGeometry {
    std::vector<uint32_t> filterIndices;    // cluster-ordered global vertex ids
    std::vector<uint32_t> meshletVertices;  // unique vertex windows (global ids)
    std::vector<uint32_t> meshletTriangles; // 3 packed u8 local ids per uint
    std::vector<Cluster> clusters;
    std::vector<Mesh> meshes;
    std::vector<glm::vec4> meshBounds;  // per-mesh union bound (center.xyz, radius)
    uint32_t clusterIndexBase = 0;      // region base in uint32 indices (0 = none)
    uint64_t clusterIndexAllocBits = 0; // VMA virtual-allocation bits
    smvk::LogicalBufferHandle indexHeapLogical{};
    smvk::Holder<smvk::BufferHandle> clusterBuffer;
    smvk::LogicalBufferHandle clusterLogical{};
    smvk::Holder<smvk::BufferHandle> meshletVertexBuffer;
    smvk::LogicalBufferHandle meshletVertexLogical{};
    smvk::Holder<smvk::BufferHandle> meshletTriangleBuffer;
    smvk::LogicalBufferHandle meshletTriangleLogical{};
    smvk::Holder<smvk::BufferHandle> meshesBuffer;
    smvk::LogicalBufferHandle meshesLogical{};
    smvk::Holder<smvk::BufferHandle> meshBoundsBuffer;
    smvk::LogicalBufferHandle meshBoundsLogical{};
};

// Registers one mesh's retained upload as clusters/meshlets (runtime path).
// Returns the mesh index, or UINT32_MAX on failure.
uint32_t RegisterMeshFromUpload(StaticGeometry& s,
                                const StaticMeshUpload& upload,
                                uint32_t baseVertex);

// Registers a cooked meshlet set as clusters/meshlets (pak path). Returns the
// mesh index, or UINT32_MAX on failure.
uint32_t RegisterMeshFromCooked(StaticGeometry& s,
                                const smvk::CookedMeshlets& cooked,
                                uint32_t baseVertex);

// Bakes filterIndices/clusters/meshlet streams into GPU buffers (call once
// after all RegisterMeshFrom* calls).
void UploadStaticData(StaticGeometry& s, smvk::ResourceManager& ctx, smvk::RenderGraph& rg);

// Clears the load-time registration state (filterIndices/clusters/meshes/
// meshlet streams).
void ResetStaticRegistrations(StaticGeometry& s);

} // namespace gfx
} // namespace tour

// COMPAT: the static-geometry registration lives in tour::gfx; the namespace
// ALIAS keeps the existing smvk::gfx::* spellings (GfxRenderer, WorldRenderer,
// gpu_scene_raster_geometry) resolving while call sites migrate to the
// gpu_scene_static surface. Retire it as consumers move off it.
namespace smvk {
namespace gfx = ::tour::gfx;
}
