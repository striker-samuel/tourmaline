// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The shared GPU geometry upload service's implementation (see
// geometry_upload.h for the design record). Extracted VERBATIM from
// Cluster::UploadStaticMeshesDirect's carve/copy core (the cluster
// keeps a compatibility wrapper for its pool bookkeeping) — the
// behavior contract is bit-identical: same carve order, same
// all-or-nothing rollback, same {srcOffset, dstOffset, size} copy
// tables, same pipeline stages per stream.

#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/geometry_upload.h>
#include <Engine/Renderer/vertex_pack.h>

namespace smvk {

// ---- the retirement helpers (moved from cluster.cpp) -----------------------

void FreeStaticMeshRangesNow(StaticHeapArena::AllocId vertexAllocation,
                             StaticHeapArena::AllocId indexAllocation,
                             StaticHeapArena::AllocId lightmapAllocation) {
    if (vertexAllocation)
        staticVertexFmtHeap.m_staticVertexBlock.Free(vertexAllocation);
    if (indexAllocation)
        staticVertexFmtHeap.m_staticIndexBlock.Free(indexAllocation);
    if (lightmapAllocation)
        staticVertexFmtHeap.m_staticLightmapBlock.Free(lightmapAllocation);
}

void DeferFreeStaticMeshRanges(ResourceManager* ctx,
                               StaticHeapArena::AllocId vertexAllocation,
                               StaticHeapArena::AllocId indexAllocation,
                               StaticHeapArena::AllocId lightmapAllocation) {
    if (!ctx) {
        // Defensive (allocations imply a live ctx_): nothing was ever
        // staged through this path, free in place.
        FreeStaticMeshRangesNow(vertexAllocation, indexAllocation, lightmapAllocation);
        return;
    }
    StaticHeapArena* vertexBlock = &staticVertexFmtHeap.m_staticVertexBlock;
    StaticHeapArena* indexBlock = &staticVertexFmtHeap.m_staticIndexBlock;
    StaticHeapArena* lightmapBlock = &staticVertexFmtHeap.m_staticLightmapBlock;
    ctx->DeferDestroyTask(std::packaged_task<void()>([vertexBlock,
                                                      vertexAllocation,
                                                      indexBlock,
                                                      indexAllocation,
                                                      lightmapBlock,
                                                      lightmapAllocation] {
        if (vertexAllocation)
            vertexBlock->Free(vertexAllocation);
        if (indexAllocation)
            indexBlock->Free(indexAllocation);
        if (lightmapAllocation)
            lightmapBlock->Free(lightmapAllocation);
    }));
}

// ---- the shared upload core -------------------------------------------------

std::vector<StaticGeometryUpload> UploadStaticGeometry(
    ResourceManager& ctx,
    const void* content,
    std::span<const StaticGeometrySource> sources) {
    std::vector<StaticGeometryUpload> uploaded;
    if (!content || sources.empty())
        return uploaded;

    // =========================================================================
    // 1. Allocate heap ranges (all-or-nothing — nothing staged on failure)
    // =========================================================================
    struct MeshRange {
        uint64_t vertexBase = 0; // VERTEX UNITS
        uint64_t indexByteOffset = 0;
        StaticMeshAllocation alloc{};
    };
    std::vector<MeshRange> ranges(sources.size());
    // Same all-or-nothing rollback as the carve loops always had (in
    // place is safe — nothing is staged before every range is carved).
    auto failCarved = [&ranges](size_t upto) {
        for (size_t j = 0; j < upto; ++j)
            FreeStaticMeshRangesNow(ranges[j].alloc.vertexAllocation,
                                    ranges[j].alloc.indexAllocation,
                                    ranges[j].alloc.lightmapAllocation);
    };
    for (size_t i = 0; i < sources.size(); ++i) {
        const StaticGeometrySource& src = sources[i];
        if (src.vertexCount == 0) {
            failCarved(i); // degenerate entry fails the whole batch
            return {};
        }
        MeshRange& r = ranges[i];
        {
            r.alloc.vertexAllocation =
                staticVertexFmtHeap.m_staticVertexBlock.Allocate(src.vertexCount, 1, r.vertexBase);
            if (r.alloc.vertexAllocation == 0) {
                failCarved(i);
                return {};
            }
        }
        if (src.indexCount > 0) {
            const uint64_t indexBytes = sizeof(uint32_t) * static_cast<uint64_t>(src.indexCount);
            r.alloc.indexAllocation = staticVertexFmtHeap.m_staticIndexBlock.Allocate(
                indexBytes, alignof(uint32_t), r.indexByteOffset);
            if (r.alloc.indexAllocation == 0) {
                failCarved(i + 1);
                return {};
            }
        }
        // The OPTIONAL lightmap range: the source blob carries the
        // stream only for charted meshes (lmSrc != 0).
        if (src.lmSrc != 0) {
            uint64_t lmBase = 0;
            r.alloc.lightmapAllocation =
                staticVertexFmtHeap.m_staticLightmapBlock.Allocate(src.vertexCount, 1, lmBase);
            if (r.alloc.lightmapAllocation == 0) {
                failCarved(i + 1);
                return {};
            }
            r.alloc.lightmapBase = lmBase;
        }
    }

    // =========================================================================
    // 2. Assemble the mesh VALUES (surfaces resolved to the absolute
    //    monoblock ranges — pool-ready for whichever owner stores them)
    // =========================================================================
    uploaded.resize(sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
        const StaticGeometrySource& src = sources[i];
        MeshRange& r = ranges[i];
        StaticGeometryUpload& u = uploaded[i];
        u.mesh.geometry.topology = PrimitiveTopology::TriangleList;
        u.mesh.geometry.vertexCount = src.vertexCount;
        u.mesh.geometry.indexCount = src.indexCount;
        u.mesh.geometry.aabbMin = src.aabbMin;
        u.mesh.geometry.aabbMax = src.aabbMax;
        Surface surface{};
        surface.firstIndex = static_cast<uint32_t>(r.indexByteOffset / sizeof(uint32_t));
        surface.indexCount = src.indexCount;
        surface.baseVertex = static_cast<int32_t>(r.vertexBase);
        u.mesh.geometry.surfaces.push_back(surface);
        // The optional lightmap window's published base (0xFFFFFFFF =
        // none) — the draw-ring fill reads it for the per-draw delta.
        u.mesh.lightmapBase =
            r.alloc.lightmapAllocation ? static_cast<uint32_t>(r.alloc.lightmapBase) : 0xFFFFFFFFu;
        r.alloc.vertexBase = r.vertexBase;
        r.alloc.indexByteOffset = r.indexByteOffset;
        u.alloc = r.alloc;
    }

    // =========================================================================
    // 3. THREE table-driven bulk uploads straight from `content` — the
    //    sources are packed-SoA subranges of one blob (the loaded pak
    //    image), so this is pure offset math: no AoS parse, no
    //    conversion, no intermediate vectors.
    // =========================================================================
    std::vector<BufferBulkCopy> posCopies;
    std::vector<BufferBulkCopy> shadeCopies;
    std::vector<BufferBulkCopy> idxCopies;
    std::vector<BufferBulkCopy> lmCopies;
    posCopies.reserve(sources.size());
    shadeCopies.reserve(sources.size());
    lmCopies.reserve(sources.size());
    idxCopies.reserve(sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
        const StaticGeometrySource& src = sources[i];
        const MeshRange& r = ranges[i];
        posCopies.push_back(BufferBulkCopy{size_t(src.posSrc),
                                           size_t(r.vertexBase) * sizeof(PackedVertexPosition),
                                           size_t(src.vertexCount) * sizeof(PackedVertexPosition)});
        shadeCopies.push_back(
            BufferBulkCopy{size_t(src.shadeSrc),
                           size_t(r.vertexBase) * sizeof(PackedVertexShading),
                           size_t(src.vertexCount) * sizeof(PackedVertexShading)});
        if (src.lmSrc != 0) {
            lmCopies.push_back(
                BufferBulkCopy{size_t(src.lmSrc),
                               size_t(r.alloc.lightmapBase) * sizeof(PackedVertexLightmap),
                               size_t(src.vertexCount) * sizeof(PackedVertexLightmap)});
        }
        if (src.indexCount > 0) {
            idxCopies.push_back(BufferBulkCopy{size_t(src.idxSrc),
                                               size_t(r.indexByteOffset),
                                               size_t(src.indexCount) * sizeof(uint32_t)});
        }
    }
    ctx.BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexPosBuffer,
                       content,
                       posCopies,
                       PipelineStageBit_Vertex,
                       AccessFlags::VertexAttributeRead);
    ctx.BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexShadingBuffer,
                       content,
                       shadeCopies,
                       PipelineStageBit_Vertex,
                       AccessFlags::VertexAttributeRead);
    if (!lmCopies.empty()) {
        ctx.BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexLightmapBuffer,
                           content,
                           lmCopies,
                           PipelineStageBit_All,
                           AccessFlags::ShaderRead);
    }
    if (!idxCopies.empty()) {
        ctx.BulkCopyBuffer(staticVertexFmtHeap.m_staticIndexBuffer,
                           content,
                           idxCopies,
                           PipelineStageBit_Vertex,
                           AccessFlags::IndexRead);
    }
    return uploaded;
}

} // namespace smvk
