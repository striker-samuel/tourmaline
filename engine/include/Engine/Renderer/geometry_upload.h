// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// THE SHARED GPU GEOMETRY UPLOAD SERVICE (the consolidated content
// architecture's extraction step — docs/content_as_code.md): the
// carve + all-or-nothing rollback + table-driven bulk-copy core that
// used to live inside Cluster::UploadStaticMeshesDirect, lifted out
// so the OWNERSHIP of uploaded geometry is the CALLER's, never the
// service's and never implicitly a scene cluster's.
//
// Three consumers share this one seam by design (never a duplicate
// uploader, per the architecture rule):
//   - the CLUSTER deserializer (the compatibility wrapper
//     Cluster::UploadStaticMeshesDirect — pool row + per-cluster
//     meshAllocations_ bookkeeping stay there);
//   - the future shared mesh store (app::AssetStore's permanent
//     model — mesh rows owned independently of any scene cluster);
//   - the pak-vNext pager's page-aware uploads (geometry_streamer.h
//     Stage 6 + the compiled physical plans).
//
// The GPU storage itself was ALWAYS shared: the lifetime-global
// staticVertexFmtHeap monoblocks (static_vertex_heap.h) — every
// cluster's meshes carve ranges from the same buffers. What this
// service makes shared is the BOOKKEEPING: the arena tokens come
// back as caller-owned values (StaticGeometryUpload::alloc), so a
// non-cluster owner can hold them and free them through the same
// retirement-ordered helpers.

#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/static_vertex_heap.h>
#include <Engine/smvk.h>

#include <span>
#include <vector>

namespace smvk {

// One direct-addressed upload source: packed-SoA stream subranges of
// ONE blob (the loaded pak image) addressed by byte offsets — the
// upload is pure offset math, no AoS parse, no per-vertex CPU
// conversion. (Moved out of Cluster's nested StaticMeshDirectSource —
// RENAMED here; the old nested name is gone, no compat alias.)
struct StaticGeometrySource {
    uint64_t posSrc = 0; // content-relative stream offsets
    uint64_t shadeSrc = 0;
    uint64_t lmSrc = 0; // lightmap stream (0 = none)
    uint64_t idxSrc = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    glm::vec3 aabbMin{};
    glm::vec3 aabbMax{};
};

// One uploaded mesh: the pool-ready StaticMesh VALUE (surfaces
// resolved to absolute monoblock ranges) + the CALLER-OWNED arena
// tokens. The caller stores the value wherever its ownership model
// puts mesh rows (a Cluster pool today; the shared store tomorrow)
// and frees the tokens through the helpers below — the service holds
// nothing after the call returns.
struct StaticGeometryUpload {
    StaticMesh mesh;
    StaticMeshAllocation alloc{};
};

// The shared upload core. Carves the vertex/index/(optional)
// lightmap ranges (all-or-nothing: a degenerate entry or an
// exhausted heap rolls back every carved range and stages NOTHING),
// assembles the mesh values, and issues the table-driven
// BulkCopyBuffer calls straight from `content`. Requires a real GPU
// context (the deserializer's fast path — never offline mode).
// Empty result = failure.
std::vector<StaticGeometryUpload> UploadStaticGeometry(
    ResourceManager& ctx,
    const void* content,
    std::span<const StaticGeometrySource> sources);

// -----------------------------------------------------------------------------
// Mesh heap-range retirement (the deferred-destruction contract — the
// facade's DeferDestroyTask): the vertex/index arena ranges free
// RETIREMENT-ORDERED with the last published graphics timeline
// point, never in place. An in-flight frame can still be DRAWING a
// destroyed mesh's ranges — an immediate arena free hands the range
// to the next upload, and the old frame's draws then read the NEW
// mesh's vertices/indices through the old draw args (garbage
// rendering, the classic suballoc-vs-frames-in-flight hazard).
//
// Contract (the DeferDestroy API rule): only call from the render
// thread between frames — the published point then covers every
// in-flight producer. The arenas live in the lifetime-global
// staticVertexFmtHeap (inline storage — outlives every retirement),
// so capturing them by pointer stays valid through the retirement
// window.
//
// The IN-PLACE variant is ONLY safe for ranges that were never
// staged to the GPU and never referenced by any pending streamer
// batch (the carve loops' all-or-nothing rollbacks — their bulk
// uploads happen only after every range is carved). Everything a
// draw could have touched must use the deferred form.
// -----------------------------------------------------------------------------
void FreeStaticMeshRangesNow(StaticHeapArena::AllocId vertexAllocation,
                             StaticHeapArena::AllocId indexAllocation,
                             StaticHeapArena::AllocId lightmapAllocation);
void DeferFreeStaticMeshRanges(ResourceManager* ctx,
                               StaticHeapArena::AllocId vertexAllocation,
                               StaticHeapArena::AllocId indexAllocation,
                               StaticHeapArena::AllocId lightmapAllocation);

} // namespace smvk
