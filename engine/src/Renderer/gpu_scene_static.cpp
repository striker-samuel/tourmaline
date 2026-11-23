// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/App/world.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Renderer/gpu_scene_static.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/static_vertex_heap.h>
#include <Engine/debug_log.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace tour::gfx {

// (migration compat) this TU spells the smvk RHI names unqualified, the way
// the old enclosing-smvk lookup made them resolve; the TU-LOCAL
// using-directive stands in for it (never in the header — see R6).
using namespace ::smvk;

// ---------------------------------------------------------------------------
// Static index-region allocation (the static vertex heap's arena).
// ---------------------------------------------------------------------------

// Allocates a region of the static index monoblock for the cluster-ordered
// filterIndices stream. False when the reservation is exhausted.
static bool AllocIndexRegion(uint32_t indexCount,
                             uint32_t& outBaseIndices,
                             uint64_t& outAllocBits) {
    outBaseIndices = 0;
    outAllocBits = 0;
    if (indexCount == 0)
        return true;
    StaticHeapArena& block = staticVertexFmtHeap.m_staticIndexBlock;
    uint64_t byteOffset = 0;
    const StaticHeapArena::AllocId alloc =
        block.Allocate(uint64_t(indexCount) * sizeof(uint32_t), alignof(uint32_t), byteOffset);
    if (alloc == 0)
        return false;
    outBaseIndices = (uint32_t)(byteOffset / sizeof(uint32_t));
    outAllocBits = (uint64_t)alloc;
    return true;
}

// Retire-ordered index-region free: the arena free rides DeferDestroyTask so
// the range returns to the free list only after every in-flight frame's reads
// have retired. Load-time UploadStaticData has nothing in flight (a harmless
// delay); the geometry-streaming promote path rebuilds between frames with
// live flights, and an in-place free would hand the old region to the new
// upload while an in-flight backend still reads the old base.
static void DeferFreeIndexRegion(ResourceManager& ctx, uint64_t& allocBits) {
    if (allocBits == 0)
        return;
    const uint64_t bits = allocBits;
    allocBits = 0;
    StaticHeapArena* block = &staticVertexFmtHeap.m_staticIndexBlock;
    ctx.DeferDestroyTask(std::packaged_task<void()>([block, bits] {
        if (bits)
            block->Free((StaticHeapArena::AllocId)bits);
    }));
}

// ---------------------------------------------------------------------------
// Registration (load-time)
// ---------------------------------------------------------------------------

static void AppendCluster(StaticGeometry& s,
                          uint32_t mesh,
                          const uint32_t* globalIds,
                          uint32_t triCount,
                          const uint32_t* window,
                          uint32_t vertexCount,
                          const uint8_t* localTris,
                          const glm::vec3& center,
                          float radius,
                          const glm::vec3& apex,
                          const glm::vec3& axis,
                          float cutoff) {
    Cluster cl{};
    cl.mesh = mesh;
    cl.indexOffset = (uint32_t)s.filterIndices.size();
    cl.triCount = triCount;
    cl.vertexOffset = (uint32_t)s.meshletVertices.size();
    cl.vertexCount = vertexCount;
    cl.triangleOffset = (uint32_t)s.meshletTriangles.size();
    cl.boundCenter = center;
    cl.boundRadius = radius;
    cl.coneApex = apex;
    cl.coneAxis = axis;
    cl.coneCutoff = cutoff;
    s.filterIndices.insert(s.filterIndices.end(), globalIds, globalIds + triCount * 3);
    // Indexed-meshlet streams (task/mesh path): the lifted unique vertex
    // window + local triangle ids (3 packed u8 ids per uint). Null window
    // marks a non-indexed cluster (the 128-tri fallback chunks), which the
    // mesh shader never admits.
    if (vertexCount != 0u && window != nullptr && localTris != nullptr) {
        s.meshletVertices.insert(s.meshletVertices.end(), window, window + vertexCount);
        for (uint32_t t = 0; t < triCount; ++t) {
            const uint32_t a = localTris[t * 3u + 0u];
            const uint32_t b = localTris[t * 3u + 1u];
            const uint32_t c = localTris[t * 3u + 2u];
            s.meshletTriangles.push_back(a | (b << 8u) | (c << 16u));
        }
    }
    s.clusters.push_back(cl);
}

// Expands one meshlet (local-id triangles + vertex window of upload-local
// ids) into heap-global ids. window = cooked.vertices + vertexOffset,
// baseVertex lifts upload-local ids to heap-global ids.
static void AppendMeshlet(StaticGeometry& s,
                          uint32_t mesh,
                          const Meshlet& m,
                          const uint8_t* localTris,
                          const uint32_t* window,
                          uint32_t baseVertex) {
    if (m.triangleCount == 0 || m.triangleCount > kMaxClusterTris)
        return;
    std::vector<uint32_t> ids;
    ids.reserve(m.triangleCount * 3);
    std::vector<uint32_t> liftedWindow;
    liftedWindow.reserve(m.vertexCount);
    for (uint32_t v = 0; v < m.vertexCount; ++v)
        liftedWindow.push_back(baseVertex + window[v]);
    for (uint32_t t = 0; t < m.triangleCount; ++t) {
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t local = localTris[t * 3 + k];
            if (local >= m.vertexCount)
                return; // corrupt: drop the whole meshlet
            ids.push_back(baseVertex + window[local]);
        }
    }
    AppendCluster(s,
                  mesh,
                  ids.data(),
                  m.triangleCount,
                  liftedWindow.data(),
                  m.vertexCount,
                  localTris,
                  m.boundCenter,
                  m.boundRadius,
                  m.coneApex,
                  m.coneAxis,
                  m.coneCutoff);
}

static uint32_t FinishMesh(StaticGeometry& s, uint32_t clusterStart) {
    Mesh entry{};
    entry.clusterStart = clusterStart;
    entry.clusterCount = (uint32_t)s.clusters.size() - clusterStart;
    glm::vec3 mn(std::numeric_limits<float>::infinity());
    glm::vec3 mx(-std::numeric_limits<float>::infinity());
    for (uint32_t ci = clusterStart; ci < (uint32_t)s.clusters.size(); ++ci) {
        const Cluster& c = s.clusters[ci];
        mn = glm::min(mn, c.boundCenter - glm::vec3(c.boundRadius));
        mx = glm::max(mx, c.boundCenter + glm::vec3(c.boundRadius));
    }
    glm::vec4 bound(0.0f, 0.0f, 0.0f, 0.0f);
    if (!(mn.x <= mx.x))
        bound = glm::vec4(0.0f, 0.0f, 0.0f, 1e30f); // NaN bounds: force untestable
    else
        bound = glm::vec4((mn + mx) * 0.5f, glm::length((mx - mn) * 0.5f));
    s.meshBounds.push_back(bound);
    // Mesh clusters append into the same packed index stream, so the
    // physical range is known at registration time rather than rediscovered
    // by walking a separate renderer-side mesh/cluster table.
    IndexedRange range{};
    if (entry.clusterCount != 0u) {
        const Cluster& first = s.clusters[clusterStart];
        const Cluster& last = s.clusters.back();
        const uint64_t end = uint64_t(last.indexOffset) + uint64_t(last.triCount) * 3u;
        if (end <= std::numeric_limits<uint32_t>::max() && end > first.indexOffset &&
            end <= s.filterIndices.size()) {
            range.firstIndex = first.indexOffset;
            range.indexCount = uint32_t(end - first.indexOffset);
        }
    }
    s.indexedRanges.push_back(range);
    // Max vertex id referenced by this mesh's index stream. Walked here, at
    // registration (once), so the renderer's per-frame capacity check is a
    // single compare per mesh rather than an O(indices) scan.
    uint32_t maxVertexIndex = 0u;
    if (range.indexCount != 0u) {
        const uint32_t* idx = s.filterIndices.data() + range.firstIndex;
        for (uint32_t i = 0; i < range.indexCount; ++i)
            maxVertexIndex = std::max(maxVertexIndex, idx[i]);
    }
    s.meshMaxVertexIndex.push_back(maxVertexIndex);
    s.meshes.push_back(entry);
    return (uint32_t)s.meshes.size() - 1;
}

uint32_t RegisterMeshFromUpload(StaticGeometry& s, const MeshData& upload, uint32_t baseVertex) {
    if (upload.vertices.empty() || upload.indices.empty() || (upload.indices.size() % 3) != 0)
        return UINT32_MAX;
    if (s.meshes.size() >= kMaxMeshes)
        return UINT32_MAX;
    const uint32_t clusterStart = (uint32_t)s.clusters.size();
    const uint32_t meshIndex = (uint32_t)s.meshes.size();

    MeshletConfig cfg{};
    cfg.enabled = true;
    CookedMeshlets cooked{};
    const bool cookedOk =
        BuildMeshletsForUpload(upload, cfg, &cooked, nullptr, nullptr) && !cooked.empty();

    bool useCooked = cookedOk;
    if (useCooked) {
        for (const Meshlet& m : cooked.meshlets) {
            if (m.triangleCount == 0 || m.triangleCount > kMaxClusterTris || m.vertexCount == 0 ||
                m.vertexOffset + m.vertexCount > (uint32_t)cooked.vertices.size() ||
                m.triangleOffset + m.triangleCount * 3 > (uint32_t)cooked.triangles.size()) {
                useCooked = false;
                break;
            }
        }
    }

    if (useCooked) {
        for (const Meshlet& m : cooked.meshlets) {
            AppendMeshlet(s,
                          meshIndex,
                          m,
                          cooked.triangles.data() + m.triangleOffset,
                          cooked.vertices.data() + m.vertexOffset,
                          baseVertex);
        }
    } else {
        // Fallback: <=128-tri chunks with AABB-sphere bounds; cone cutoff
        // 1.0 disables the cone test (matches meshopt degenerate bounds).
        s.clusters.reserve(s.clusters.size() + upload.indices.size() / (kMaxClusterTris * 3) + 1);
        const uint32_t triTotal = (uint32_t)upload.indices.size() / 3;
        for (uint32_t first = 0; first < triTotal; first += kMaxClusterTris) {
            const uint32_t count = std::min(kMaxClusterTris, triTotal - first);
            glm::vec3 mn(std::numeric_limits<float>::infinity());
            glm::vec3 mx(-std::numeric_limits<float>::infinity());
            std::vector<uint32_t> ids;
            ids.reserve(count * 3);
            for (uint32_t t = 0; t < count; ++t) {
                for (uint32_t k = 0; k < 3; ++k) {
                    const uint32_t local = upload.indices[(first + t) * 3 + k];
                    ids.push_back(baseVertex + local);
                    if (local < upload.vertices.size()) {
                        const glm::vec3& p = upload.vertices[local].pos;
                        mn = glm::min(mn, p);
                        mx = glm::max(mx, p);
                    }
                }
            }
            const glm::vec3 center = (mn + mx) * 0.5f;
            AppendCluster(s,
                          meshIndex,
                          ids.data(),
                          count,
                          nullptr,
                          0u,
                          nullptr,
                          center,
                          glm::length(mx - center),
                          center,
                          glm::vec3(0.0f, 1.0f, 0.0f),
                          1.0f);
        }
    }

    if (s.clusters.size() == clusterStart)
        return UINT32_MAX;
    return FinishMesh(s, clusterStart);
}

uint32_t RegisterMeshFromCooked(StaticGeometry& s,
                                const CookedMeshlets& cooked,
                                uint32_t baseVertex) {
    if (cooked.empty() || s.meshes.size() >= kMaxMeshes)
        return UINT32_MAX;
    // Whole-mesh meshlet validation (fail-closed). A mesh is meshlet-ready
    // only when EVERY meshlet is structurally valid and within the
    // mesh-shader workgroup limits; any violation rejects the whole mesh to
    // the direct (indexed) path, never a partial meshlet draw. The renderer
    // consumes this registration as a trusted, validated result - no
    // per-frame re-walk of clusters or indices.
    for (const Meshlet& m : cooked.meshlets) {
        if (m.triangleCount == 0 || m.triangleCount > kMeshletMaxTriangles || m.vertexCount == 0 ||
            m.vertexCount > kMeshletMaxVertices)
            return UINT32_MAX;
        if (m.vertexOffset + m.vertexCount > (uint32_t)cooked.vertices.size())
            return UINT32_MAX;
        if (m.triangleOffset + m.triangleCount * 3 > (uint32_t)cooked.triangles.size())
            return UINT32_MAX;
        const uint8_t* localTris = cooked.triangles.data() + m.triangleOffset;
        for (uint32_t i = 0; i < m.triangleCount * 3; ++i)
            if (localTris[i] >= m.vertexCount)
                return UINT32_MAX;
    }
    const uint32_t clusterStart = (uint32_t)s.clusters.size();
    const uint32_t meshIndex = (uint32_t)s.meshes.size();
    for (const Meshlet& m : cooked.meshlets) {
        const uint32_t* window = cooked.vertices.data() + m.vertexOffset;
        const uint8_t* localTris = cooked.triangles.data() + m.triangleOffset;
        std::vector<uint32_t> globalIds;
        globalIds.reserve(m.triangleCount * 3);
        for (uint32_t i = 0; i < m.triangleCount * 3; ++i)
            globalIds.push_back(baseVertex + window[localTris[i]]);
        std::vector<uint32_t> liftedWindow;
        liftedWindow.reserve(m.vertexCount);
        for (uint32_t v = 0; v < m.vertexCount; ++v)
            liftedWindow.push_back(baseVertex + window[v]);
        AppendCluster(s,
                      meshIndex,
                      globalIds.data(),
                      m.triangleCount,
                      liftedWindow.data(),
                      m.vertexCount,
                      localTris,
                      m.boundCenter,
                      m.boundRadius,
                      m.coneApex,
                      m.coneAxis,
                      m.coneCutoff);
    }
    if (s.clusters.size() == clusterStart)
        return UINT32_MAX;
    return FinishMesh(s, clusterStart);
}

void ResetStaticRegistrations(StaticGeometry& s) {
    s.filterIndices.clear();
    s.meshletVertices.clear();
    s.meshletTriangles.clear();
    s.clusters.clear();
    s.meshes.clear();
    s.meshMaxVertexIndex.clear();
    s.meshBounds.clear();
    s.indexedRanges.clear();
    s.indexedRangesResident = false;
    // clusterIndexBase/AllocBits + the table buffers stay — they are
    // UploadStaticData's to retire/replace (alloc-new-first, the old region
    // frees deferred).
}

void UploadStaticData(StaticGeometry& s, ResourceManager& ctx, RenderGraph& rg) {
    // The current upload must complete before any indexed-range descriptor
    // can become visible to GPU Scene. Failure leaves all ranges unusable.
    s.indexedRangesResident = false;
    if (!s.clusterLogical.empty())
        rg.FreeBuffer(s.clusterLogical);
    s.clusterLogical = {};
    s.clusterBuffer = {};
    if (!s.meshBoundsLogical.empty())
        rg.FreeBuffer(s.meshBoundsLogical);
    s.meshBoundsLogical = {};
    s.meshBoundsBuffer = {};
    if (s.indexHeapLogical.empty())
        s.indexHeapLogical =
            rg.AddExternalBuffer(staticVertexFmtHeap.m_staticIndexBuffer, "static index monoblock");
    uint32_t newIndexBase = 0;
    uint64_t newIndexBits = 0;
    if (!s.filterIndices.empty() && !s.clusters.empty() &&
        AllocIndexRegion((uint32_t)s.filterIndices.size(), newIndexBase, newIndexBits)) {
        DeferFreeIndexRegion(ctx, s.clusterIndexAllocBits);
        s.clusterIndexBase = newIndexBase;
        s.clusterIndexAllocBits = newIndexBits;
    } else if (s.filterIndices.empty() || s.clusters.empty()) {
        DeferFreeIndexRegion(ctx, s.clusterIndexAllocBits);
        s.clusterIndexBase = 0;
    } else {
        std::fprintf(stderr,
                     "[GpuSceneStatic] cluster index region exhausted the static index "
                     "monoblock (%u indices): increase the reservation.\n",
                     (unsigned)s.filterIndices.size());
        return;
    }
    if (s.filterIndices.empty() || s.clusters.empty())
        return;
    std::vector<ClusterConstants> table(s.clusters.size());
    for (size_t i = 0; i < s.clusters.size(); ++i) {
        table[i].indexOffset = s.clusters[i].indexOffset;
        table[i].triCount = s.clusters[i].triCount;
        table[i].vertexOffset = s.clusters[i].vertexOffset;
        table[i].vertexCount = s.clusters[i].vertexCount;
        table[i].triangleOffset = s.clusters[i].triangleOffset;
        table[i].boundCenter = s.clusters[i].boundCenter;
        table[i].boundRadius = s.clusters[i].boundRadius;
        table[i].coneApex = s.clusters[i].coneApex;
        table[i].coneAxis = s.clusters[i].coneAxis;
        table[i].coneCutoff = s.clusters[i].coneCutoff;
    }
    s.clusterBuffer = ctx.CreateBuffer(
        table.size() * sizeof(ClusterConstants), BufferType_Storage, "gpu-scene clusters");
    if (s.clusterBuffer.empty())
        return;
    ctx.UploadDataToBuffer(staticVertexFmtHeap.m_staticIndexBuffer,
                           s.filterIndices.data(),
                           s.filterIndices.size() * sizeof(uint32_t),
                           PipelineStage(PipelineStageBit_Vertex),
                           AccessFlags::IndexRead,
                           0,
                           (size_t)s.clusterIndexBase * sizeof(uint32_t));
    ctx.UploadDataToBuffer(s.clusterBuffer,
                           table.data(),
                           table.size() * sizeof(ClusterConstants),
                           PipelineStage(PipelineStageBit_Compute),
                           AccessFlags::ShaderRead);
    s.clusterLogical = rg.AddExternalBuffer(s.clusterBuffer, "gpu-scene clusters");
    if (s.clusterLogical.empty())
        return;
    s.indexedRangesResident = true;
    if (!s.meshletVertexLogical.empty())
        rg.FreeBuffer(s.meshletVertexLogical);
    s.meshletVertexLogical = {};
    s.meshletVertexBuffer = {};
    if (!s.meshletTriangleLogical.empty())
        rg.FreeBuffer(s.meshletTriangleLogical);
    s.meshletTriangleLogical = {};
    s.meshletTriangleBuffer = {};
    if (!s.meshletVertices.empty() && !s.meshletTriangles.empty()) {
        s.meshletVertexBuffer = ctx.CreateBuffer(s.meshletVertices.size() * sizeof(uint32_t),
                                                 BufferType_Storage,
                                                 "gpu-scene meshlet vertices");
        s.meshletTriangleBuffer = ctx.CreateBuffer(s.meshletTriangles.size() * sizeof(uint32_t),
                                                   BufferType_Storage,
                                                   "gpu-scene meshlet triangles");
        if (!s.meshletVertexBuffer.empty()) {
            ctx.UploadDataToBuffer(s.meshletVertexBuffer,
                                   s.meshletVertices.data(),
                                   s.meshletVertices.size() * sizeof(uint32_t),
                                   PipelineStage(PipelineStageBit_Mesh),
                                   AccessFlags::ShaderRead);
            s.meshletVertexLogical =
                rg.AddExternalBuffer(s.meshletVertexBuffer, "gpu-scene meshlet vertices");
        }
        if (!s.meshletTriangleBuffer.empty()) {
            ctx.UploadDataToBuffer(s.meshletTriangleBuffer,
                                   s.meshletTriangles.data(),
                                   s.meshletTriangles.size() * sizeof(uint32_t),
                                   PipelineStage(PipelineStageBit_Mesh),
                                   AccessFlags::ShaderRead);
            s.meshletTriangleLogical =
                rg.AddExternalBuffer(s.meshletTriangleBuffer, "gpu-scene meshlet triangles");
        }
    }
    if (!s.meshBounds.empty()) {
        s.meshBoundsBuffer = ctx.CreateBuffer(
            s.meshBounds.size() * sizeof(glm::vec4), BufferType_Storage, "gpu-scene mesh bounds");
        if (!s.meshBoundsBuffer.empty()) {
            ctx.UploadDataToBuffer(s.meshBoundsBuffer,
                                   s.meshBounds.data(),
                                   s.meshBounds.size() * sizeof(glm::vec4),
                                   PipelineStage(PipelineStageBit_Compute),
                                   AccessFlags::ShaderRead);
            s.meshBoundsLogical = rg.AddExternalBuffer(s.meshBoundsBuffer, "gpu-scene mesh bounds");
        }
    }
    if (!s.meshesLogical.empty())
        rg.FreeBuffer(s.meshesLogical);
    s.meshesLogical = {};
    s.meshesBuffer = {};
    if (!s.meshes.empty()) {
        s.meshesBuffer = ctx.CreateBuffer(
            s.meshes.size() * 2 * sizeof(uint32_t), BufferType_Storage, "gpu-scene mesh ranges");
        if (!s.meshesBuffer.empty()) {
            std::vector<uint32_t> meshRanges(s.meshes.size() * 2);
            for (size_t i = 0; i < s.meshes.size(); ++i) {
                meshRanges[i * 2 + 0] = s.meshes[i].clusterStart;
                meshRanges[i * 2 + 1] = s.meshes[i].clusterCount;
            }
            ctx.UploadDataToBuffer(s.meshesBuffer,
                                   meshRanges.data(),
                                   meshRanges.size() * sizeof(uint32_t),
                                   PipelineStage(PipelineStageBit_Compute),
                                   AccessFlags::ShaderRead);
            s.meshesLogical = rg.AddExternalBuffer(s.meshesBuffer, "gpu-scene mesh ranges");
        }
    }
}

void RegisterWorldMeshes(StaticGeometry& s, const tour::World& world) {
    s.worldMeshMap.assign(world.meshLocations.size(), UINT32_MAX);
    s.worldLodMeshMap.clear();
    s.worldLodMeshMap.resize(world.meshLocations.size());
    uint32_t fromCooked = 0, fromUpload = 0, skipped = 0;
    uint64_t skippedTris = 0;
    // Dedupe: distinct base meshes can share one LOD sibling URI, and the
    // same base mesh is never registered twice. Key = owning cluster +
    // pool index (stable for the register pass; all clusters are alive).
    struct LocKey {
        const void* cluster = nullptr;
        uint32_t index = 0;
        bool operator==(const LocKey& o) const {
            return cluster == o.cluster && index == o.index;
        }
    };
    struct LocHash {
        size_t operator()(const LocKey& k) const noexcept {
            return std::hash<const void*>{}(k.cluster) * 31u + k.index;
        }
    };
    std::unordered_map<LocKey, uint32_t, LocHash> done;
    auto registerOne = [&](const tour::World::MeshLocation& loc) -> uint32_t {
        if (!loc.cluster || !loc.handle.valid())
            return UINT32_MAX;
        const LocKey key{loc.cluster, loc.handle.index()};
        if (auto it = done.find(key); it != done.end())
            return it->second;
        const smvk::StaticMesh* mesh = loc.cluster->staticMeshes.get(loc.handle);
        if (!mesh || mesh->geometry.surfaces.empty())
            return UINT32_MAX;
        const uint32_t baseVertex =
            (uint32_t)std::max<int32_t>(mesh->geometry.surfaces[0].baseVertex, 0);
        uint32_t gfxMesh = UINT32_MAX;
        if (mesh->meshlets && !mesh->meshlets->empty()) {
            gfxMesh = RegisterMeshFromCooked(s, *mesh->meshlets, baseVertex);
            if (gfxMesh != UINT32_MAX)
                ++fromCooked;
        } else if (const smvk::MeshData* retained = loc.cluster->GetRetainedUpload(loc.handle)) {
            gfxMesh = RegisterMeshFromUpload(s, *retained, baseVertex);
            if (gfxMesh != UINT32_MAX)
                ++fromUpload;
        }
        if (gfxMesh == UINT32_MAX)
            skippedTris += mesh->geometry.indexCount / 3;
        done.emplace(key, gfxMesh);
        return gfxMesh;
    };
    for (size_t i = 0; i < world.meshLocations.size(); ++i) {
        const uint32_t chainLen = world.LodChainLength((uint32_t)i);
        if (chainLen <= 1) {
            const uint32_t g = registerOne(world.meshLocations[i]);
            if (g == UINT32_MAX)
                ++skipped;
            s.worldMeshMap[i] = g;
            s.worldLodMeshMap[i] = {g};
            continue;
        }
        std::vector<uint32_t> perLod;
        perLod.reserve(chainLen);
        for (uint32_t l = 0; l < chainLen; ++l) {
            auto hit = world.ResolveLodMesh((uint32_t)i, l);
            const uint32_t g = hit ? registerOne(*hit) : UINT32_MAX;
            if (g == UINT32_MAX)
                ++skipped;
            perLod.push_back(g);
        }
        s.worldMeshMap[i] = perLod.empty() ? UINT32_MAX : perLod[0];
        s.worldLodMeshMap[i] = std::move(perLod);
    }
    SMVK_DEBUG_LOG(
        "[GfxFilter] meshes: %u cooked + %u runtime-cooked = %u filtered, %u direct (%llu tris)\n",
        fromCooked,
        fromUpload,
        fromCooked + fromUpload,
        skipped,
        (unsigned long long)skippedTris);
    SMVK_DEBUG_LOG("[GfxFilter] clusters: %u, filter indices: %llu\n",
                   (uint32_t)s.clusters.size(),
                   (unsigned long long)s.filterIndices.size());
}

} // namespace tour::gfx
