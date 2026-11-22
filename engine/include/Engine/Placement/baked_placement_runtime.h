// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// BakedPlacementRuntime: renders the authored PointInstancer cloud (the
// BakedPlacementAsset) through the PR03 cull -> prefix -> scatter -> drawbuild
// instanced-draw chain — the SAME GPU path as the procedural placement, but
// with NO procedural generation leg: the points are baked at import, bucketed
// into world-XZ tiles at cook, and uploaded as RenderInstance buffers on tile
// activation (no scatter dispatch, no constraint, no heightfield/masks).
//
// The correctness-critical part is the TILE-LOCAL PROTOTYPE PALETTE. The
// authored asset has up to 695 render prototypes, but any ONE tile references
// only a handful. Each tile carries its own proto table (bounds + draw
// distance + index range per LOCAL proto) and its instances' prototypeId is
// REMAPPED to the local palette index, so the 1024-proto bin slab (Phase 2)
// never has to hold the full global set per tile.
//
// Layering:
//   BucketBakedPlacement(asset, tileSize)  — pure-CPU tiling + palette (the
//     "spatial tiling of the 1.54M points"), headless-testable.
//   BakedPlacementRuntime                  — the GPU residency + PR03 chain,
//     fed the bucketed tiles + the RESOLVED per-proto geometry (bounds +
//     index range, derived from the world mesh registry at cook/load — NOT
//     part of the asset, which carries only mesh/material/surface identity).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/baked_placement.h>
#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_point.h>
#include <Engine/Rendergraph/placement_pass.h>

namespace tour::placement {

// ---------------------------------------------------------------------------
// A bucketed tile: the world-XZ tile key + the tile's LOCAL prototype palette
// (the subset of global render prototypes it references) + the remapped
// instances ready for the GPU chain (prototypeId is a LOCAL palette index).
// ---------------------------------------------------------------------------
struct BakedPlacementTile {
    TileID key{}; // grid coordinate (y unused = 0)
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    std::vector<uint32_t> localProtos;     // global renderPrototypeIds, sorted + deduped
    std::vector<RenderInstance> instances; // prototypeId remapped to localProtos index
};

// The resolved per-proto geometry the runtime needs to build proto tables:
// the conservative cull sphere (bounds) + the drawbuild index range. Resolved
// at cook/load from the world mesh registry (World::meshLocations + the static
// mesh bounds) — keyed by the asset's GLOBAL renderPrototypeId.
struct BakedProtoGeometry {
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    float drawDistance = 0.0f; // 0 = unlimited
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
};
static_assert(sizeof(BakedProtoGeometry) == 40,
              "BakedProtoGeometry moved — update the cooked serialization");

// A resolved surface view: the fields the cook/load resolution reads from a
// static mesh's surface (indexed range) + the mesh bounds. Decouples the
// resolution from the concrete StaticMesh type so it stays headless-testable.
struct BakedSurfaceRef {
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// Resolves the asset's render prototypes to GPU geometry via a mesh-registry
// callback: resolve(meshKey, surfaceIndex, out) fills the indexed range + bounds
// for one render prototype. meshKey is the project's MeshKey (the .geometry
// asset id, a uint64) and surfaceIndex is the FLAT surface (the .geometry blob
// index) within that asset — together they address one non-hierarchical
// surface. Returning false for an out-of-range mesh/surface refuses the WHOLE
// asset (never a silent mis-reference). The output is keyed by the asset's
// GLOBAL renderPrototypeId, so the tile-local palettes built by
// BucketBakedPlacement resolve through it directly.
[[nodiscard]] inline bool ResolveBakedPrototypes(
    const BakedPlacementAsset& asset,
    const std::function<bool(uint64_t meshKey, uint32_t surfaceIndex, BakedSurfaceRef& out)>&
        resolve,
    std::vector<BakedProtoGeometry>& out) {
    out.clear();
    out.resize(asset.renderPrototypes.size());
    for (size_t i = 0; i < asset.renderPrototypes.size(); ++i) {
        const BakedRenderPrototype& rp = asset.renderPrototypes[i];
        BakedSurfaceRef ref;
        if (!resolve(rp.meshKey, rp.surfaceIndex, ref))
            return false;
        BakedProtoGeometry& g = out[i];
        g.boundsMin = ref.boundsMin;
        g.boundsMax = ref.boundsMax;
        g.drawDistance = 0.0f;
        g.indexCount = ref.indexCount;
        g.firstIndex = ref.firstIndex;
        g.vertexOffset = ref.vertexOffset;
    }
    return true;
}

// Buckets a flat baked asset into world-XZ tiles with local proto palettes.
// Returns tiles sorted by key (x then z). Points whose renderPrototypeId is
// out of range are refused (never a silent mis-reference). Points flagged
// NeedsGeometryBake (mirroring / undecomposable — the geometry-bake phase's
// job) are SKIPPED and counted into *skippedNeedsBake when non-null, so the
// caller can report the deferral instead of silently dropping them.
// NonUniformScale points are INCLUDED (RenderInstance.scale is a vec3; the
// cull's conservative bound uses the max component).
[[nodiscard]] inline std::vector<BakedPlacementTile> BucketBakedPlacement(
    const BakedPlacementAsset& asset,
    float tileSize,
    size_t* skippedNeedsBake = nullptr) {
    if (tileSize <= 0.0f)
        tileSize = asset.tileSize > 0.0f ? asset.tileSize : 64.0f;
    std::map<std::pair<int32_t, int32_t>, BakedPlacementTile> buckets;
    std::map<std::pair<int32_t, int32_t>, std::set<uint32_t>> protoSets;
    for (const BakedPlacementPoint& pt : asset.points) {
        if (pt.renderPrototypeId >= asset.renderPrototypes.size())
            continue; // refuse the out-of-range reference
        if (pt.flags & BakedPointFlag::NeedsGeometryBake) {
            if (skippedNeedsBake)
                ++*skippedNeedsBake;
            continue;
        }
        const int32_t tx = int32_t(std::floor(double(pt.position.x) / double(tileSize)));
        const int32_t tz = int32_t(std::floor(double(pt.position.z) / double(tileSize)));
        const auto key = std::make_pair(tx, tz);
        BakedPlacementTile& tile = buckets[key];
        if (tile.instances.empty()) {
            tile.key = TileID{tx, 0, tz};
            tile.boundsMin = tile.boundsMax = pt.position;
        } else {
            tile.boundsMin = glm::min(tile.boundsMin, pt.position);
            tile.boundsMax = glm::max(tile.boundsMax, pt.position);
        }
        RenderInstance ri;
        ri.position = pt.position;
        ri.padRotAlign = 0.0f;
        ri.rotation = glm::vec4(pt.rotation[0], pt.rotation[1], pt.rotation[2], pt.rotation[3]);
        ri.scale = pt.scale;
        ri.prototypeId = pt.renderPrototypeId; // global for now; remapped below
        ri.stableLow = pt.stableId.w[0];
        ri.customB = 0;
        tile.instances.push_back(ri);
        protoSets[key].insert(pt.renderPrototypeId);
    }
    std::vector<BakedPlacementTile> out;
    out.reserve(buckets.size());
    for (auto& [key, tile] : buckets) {
        const std::set<uint32_t>& ids = protoSets[key];
        tile.localProtos.assign(ids.begin(), ids.end());
        std::map<uint32_t, uint32_t> remap;
        for (size_t i = 0; i < tile.localProtos.size(); ++i)
            remap[tile.localProtos[i]] = uint32_t(i);
        for (RenderInstance& ri : tile.instances)
            ri.prototypeId = remap[ri.prototypeId];
        out.push_back(std::move(tile));
    }
    std::sort(out.begin(), out.end(), [](const BakedPlacementTile& a, const BakedPlacementTile& b) {
        if (a.key.x != b.key.x)
            return a.key.x < b.key.x;
        return a.key.z < b.key.z;
    });
    return out;
}

// ---------------------------------------------------------------------------
// The cooked form: the runtime's direct inputs (resolved proto geometry + the
// bucketed tiles), produced at cook and serialized into the pak. Loaded at
// startup by the host and fed to BakedPlacementRuntime (tiles + protoGeometry).
// ---------------------------------------------------------------------------
struct BakedPlacementCooked {
    std::vector<BakedProtoGeometry> protoGeometry; // by GLOBAL renderPrototypeId
    std::vector<BakedPlacementTile> tiles;         // bucketed, sorted by key
    float tileSize = 0.0f;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    std::string instancerPath;

    void clear() {
        protoGeometry.clear();
        tiles.clear();
        tileSize = 0.0f;
        boundsMin = boundsMax = glm::vec3(0.0f);
        instancerPath.clear();
    }
    [[nodiscard]] bool empty() const {
        return tiles.empty();
    }
};

// Cooks a flat imported asset: resolves the render prototypes (via the
// mesh-registry callback) + buckets the points into tiles with local palettes.
// The whole cloud's bounds are the union of the bucketed tiles (valid even when
// the import left the flat envelope unset). Fails only when a mesh/surface is
// out of range (never a partial cook).
[[nodiscard]] inline bool CookBakedPlacement(
    const BakedPlacementAsset& asset,
    float tileSize,
    const std::function<bool(uint32_t meshId, uint32_t surfaceIndex, BakedSurfaceRef& out)>&
        resolve,
    BakedPlacementCooked& out,
    size_t* skippedNeedsBake = nullptr) {
    out.clear();
    if (tileSize <= 0.0f)
        tileSize = asset.tileSize > 0.0f ? asset.tileSize : 64.0f;
    if (!ResolveBakedPrototypes(asset, resolve, out.protoGeometry))
        return false;
    out.tiles = BucketBakedPlacement(asset, tileSize, skippedNeedsBake);
    out.tileSize = tileSize;
    if (!out.tiles.empty()) {
        out.boundsMin = out.tiles.front().boundsMin;
        out.boundsMax = out.tiles.front().boundsMax;
        for (const BakedPlacementTile& t : out.tiles) {
            out.boundsMin = glm::min(out.boundsMin, t.boundsMin);
            out.boundsMax = glm::max(out.boundsMax, t.boundsMax);
        }
    }
    out.instancerPath = asset.instancerPath;
    return true;
}

// ---------------------------------------------------------------------------
// Cooked binary serialization (little-endian, versioned, lossless): the
// round-trip test asserts serialize(deserialize(x)) == x, i.e. the resolved
// index ranges, tile palettes and quaternion instances survive exactly.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kBakedPlacementCookedMagic = 0x424B434B; // 'BKCK'
inline constexpr uint32_t kBakedPlacementCookedVersion = 1;

[[nodiscard]] inline bool SerializeBakedPlacementCooked(const BakedPlacementCooked& cooked,
                                                        std::vector<uint8_t>& out) {
    out.clear();
    const uint32_t protoCount = uint32_t(cooked.protoGeometry.size());
    const uint32_t tileCount = uint32_t(cooked.tiles.size());
    const uint32_t pathLen = uint32_t(cooked.instancerPath.size());
    if (protoCount != cooked.protoGeometry.size() || tileCount != cooked.tiles.size() ||
        pathLen != cooked.instancerPath.size())
        return false;
    size_t body = 5 * sizeof(uint32_t) +  // magic,ver,protoCount,tileCount,pathLen
                  sizeof(float) +         // tileSize
                  2 * sizeof(glm::vec3) + // boundsMin/Max
                  sizeof(BakedProtoGeometry) * protoCount + pathLen;
    for (const BakedPlacementTile& t : cooked.tiles) {
        body += 3 * sizeof(int32_t) + 2 * sizeof(glm::vec3); // key + tile bounds
        body += sizeof(uint32_t) + sizeof(uint32_t) * t.localProtos.size();
        body += sizeof(uint32_t) + sizeof(RenderInstance) * t.instances.size();
    }
    out.resize(body);
    size_t off = 0;
    auto put = [&](const void* p, size_t n) {
        if (off + n > out.size())
            return false;
        std::memcpy(out.data() + off, p, n);
        off += n;
        return true;
    };
    uint32_t u;
    u = kBakedPlacementCookedMagic;
    if (!put(&u, 4))
        return false;
    u = kBakedPlacementCookedVersion;
    if (!put(&u, 4))
        return false;
    u = protoCount;
    if (!put(&u, 4))
        return false;
    u = tileCount;
    if (!put(&u, 4))
        return false;
    u = pathLen;
    if (!put(&u, 4))
        return false;
    if (!put(&cooked.tileSize, 4))
        return false;
    if (!put(&cooked.boundsMin, sizeof(glm::vec3)))
        return false;
    if (!put(&cooked.boundsMax, sizeof(glm::vec3)))
        return false;
    if (protoCount && !put(cooked.protoGeometry.data(), sizeof(BakedProtoGeometry) * protoCount))
        return false;
    for (const BakedPlacementTile& t : cooked.tiles) {
        const int32_t kx = t.key.x, ky = t.key.y, kz = t.key.z;
        if (!put(&kx, 4) || !put(&ky, 4) || !put(&kz, 4))
            return false;
        if (!put(&t.boundsMin, sizeof(glm::vec3)) || !put(&t.boundsMax, sizeof(glm::vec3)))
            return false;
        u = uint32_t(t.localProtos.size());
        if (!put(&u, 4))
            return false;
        if (u && !put(t.localProtos.data(), sizeof(uint32_t) * u))
            return false;
        u = uint32_t(t.instances.size());
        if (!put(&u, 4))
            return false;
        if (u && !put(t.instances.data(), sizeof(RenderInstance) * u))
            return false;
    }
    if (pathLen && !put(cooked.instancerPath.data(), pathLen))
        return false;
    return off == out.size();
}

[[nodiscard]] inline bool DeserializeBakedPlacementCooked(const uint8_t* data,
                                                          size_t size,
                                                          BakedPlacementCooked& out) {
    out.clear();
    size_t off = 0;
    auto get = [&](void* p, size_t n) {
        if (off + n > size)
            return false;
        std::memcpy(p, data + off, n);
        off += n;
        return true;
    };
    uint32_t magic = 0, version = 0, protoCount = 0, tileCount = 0, pathLen = 0;
    if (!get(&magic, 4) || magic != kBakedPlacementCookedMagic)
        return false;
    if (!get(&version, 4) || version != kBakedPlacementCookedVersion)
        return false;
    if (!get(&protoCount, 4))
        return false;
    if (!get(&tileCount, 4))
        return false;
    if (!get(&pathLen, 4))
        return false;
    if (!get(&out.tileSize, 4))
        return false;
    if (!get(&out.boundsMin, sizeof(glm::vec3)))
        return false;
    if (!get(&out.boundsMax, sizeof(glm::vec3)))
        return false;
    if (protoCount) {
        out.protoGeometry.resize(protoCount);
        if (!get(out.protoGeometry.data(), sizeof(BakedProtoGeometry) * protoCount))
            return false;
    }
    out.tiles.resize(tileCount);
    for (BakedPlacementTile& t : out.tiles) {
        int32_t kx = 0, ky = 0, kz = 0;
        if (!get(&kx, 4) || !get(&ky, 4) || !get(&kz, 4))
            return false;
        t.key = TileID{kx, ky, kz};
        if (!get(&t.boundsMin, sizeof(glm::vec3)) || !get(&t.boundsMax, sizeof(glm::vec3)))
            return false;
        uint32_t n = 0;
        if (!get(&n, 4))
            return false;
        if (n) {
            t.localProtos.resize(n);
            if (!get(t.localProtos.data(), sizeof(uint32_t) * n))
                return false;
        }
        if (!get(&n, 4))
            return false;
        if (n) {
            t.instances.resize(n);
            if (!get(t.instances.data(), sizeof(RenderInstance) * n))
                return false;
        }
    }
    if (pathLen) {
        out.instancerPath.resize(pathLen);
        if (!get(out.instancerPath.data(), pathLen))
            return false;
    }
    return off == size;
}

// ---------------------------------------------------------------------------
// The runtime: a bounded set of resident GPU tiles (LRU) fed the bucketed
// points + the resolved proto geometry, driving the PR03 chain per tile.
// ---------------------------------------------------------------------------
class BakedPlacementRuntime {
  public:
    // ---- inputs (caller-owned, must outlive the runtime) ----
    const std::vector<BakedPlacementTile>* tiles = nullptr;         // sorted by key
    const std::vector<BakedProtoGeometry>* protoGeometry = nullptr; // by GLOBAL renderPrototypeId
    float tileSize = 64.0f;
    uint32_t slotBudget = 8; // concurrent resident GPU tiles
    uint32_t viewBudget = 1; // per-view visible sets per tile
    uint32_t maxInstancesPerTile = 65536;

    // ---- the owned stack ----
    PlacementPass pass;                   // per-slot instance + count buffers
    PlacementCullPrograms cullPrograms;   // the PR03 chain programs
    std::vector<PlacementVisibleSet> vis; // [slot * viewBudget + view]

    struct ActiveTile {
        TileID key{};
        int32_t srcIndex = -1;  // index into *tiles
        bool published = false; // uploaded + drawable
        uint32_t protoCount = 0;
        uint64_t lastUse = 0; // LRU stamp
        // The tile's LOCAL proto table (bounds + distance + index range).
        smvk::Holder<smvk::BufferHandle> protoTableBuffer;
        smvk::LogicalBufferHandle protoTableLogical{};
        void Destroy(smvk::RenderGraph& rg) {
            if (!protoTableLogical.empty())
                rg.FreeBuffer(protoTableLogical);
            protoTableLogical = {};
            protoTableBuffer = {};
            protoCount = 0;
            key = TileID{};
            srcIndex = -1;
            published = false;
        }
    };
    std::vector<ActiveTile> active; // slot-indexed

    bool created = false;
    uint64_t tick = 0;

    [[nodiscard]] bool Valid() const {
        return created && pass.Valid();
    }

    bool Create(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) {
        if (!tiles || !protoGeometry)
            return false;
        if (slotBudget == 0 || viewBudget == 0)
            return false;
        Destroy(rg);
        PlacementPassDesc desc{};
        desc.maxInstancesPerTile = maxInstancesPerTile;
        desc.maxTiles = slotBudget;
        desc.name = "baked placement";
        if (!pass.Ensure(ctx, rg, desc))
            return false;
        if (!cullPrograms.Ensure(ctx))
            return false;
        vis.resize(size_t(slotBudget) * size_t(viewBudget));
        for (auto& v : vis)
            if (!v.Create(ctx, rg, maxInstancesPerTile))
                return false;
        active.resize(slotBudget);
        created = true;
        return true;
    }
    void Destroy(smvk::RenderGraph& rg) {
        for (auto& a : active)
            a.Destroy(rg);
        active.clear();
        for (auto& v : vis)
            v.Destroy(rg);
        vis.clear();
        cullPrograms.Destroy();
        pass.Destroy(rg);
        created = false;
    }

    [[nodiscard]] const PlacementVisibleSet* VisSet(int32_t slot, uint32_t view = 0) const {
        if (slot < 0 || slot >= int32_t(active.size()) || view >= viewBudget ||
            !active[size_t(slot)].published)
            return nullptr;
        return &vis[size_t(slot) * size_t(viewBudget) + size_t(view)];
    }
    // The LOCAL proto count for a resident tile (the draw loop issues one
    // DrawIndexedIndirect per local proto — NOT the global prototype count).
    [[nodiscard]] uint32_t TileProtoCount(int32_t slot) const {
        if (slot < 0 || slot >= int32_t(active.size()) || !active[size_t(slot)].published)
            return 0;
        return active[size_t(slot)].protoCount;
    }

    // ---- THE FRAME TICK -------------------------------------------------
    // Uploads the baked tiles intersecting the interests into resident GPU
    // slots (LRU eviction when the budget is full). The interests are
    // world-XZ rects (glm::vec4 minX/minZ/maxX/maxZ, half-open). Tiles stay
    // published until their slot is claimed by a different tile (the same
    // LRU-cache semantics as the procedural path); the frustum cull drops the
    // off-screen instances.
    void BeginFrame(smvk::ResourceManager& ctx,
                    smvk::RenderGraph& rg,
                    const glm::vec4* interests,
                    size_t interestCount) {
        if (!created)
            return;
        ++tick;
        std::vector<size_t> sel;
        SelectTiles(interests, interestCount, *tiles, size_t(slotBudget), sel);
        for (size_t src : sel) {
            const BakedPlacementTile& tile = (*tiles)[src];
            const int32_t slot = FindSlot(tile.key);
            if (slot >= 0 && active[size_t(slot)].published) {
                active[size_t(slot)].lastUse = tick; // already resident + published
                continue;
            }
            const int32_t claim = slot >= 0 ? slot : ClaimSlot();
            if (claim < 0)
                continue;
            UploadTile(ctx, rg, uint32_t(claim), int32_t(src), tile);
        }
    }

    // ---- THE CULL CHAIN (per view) -------------------------------------
    // Records cull -> prefix -> scatter -> drawbuild for every published
    // tile, each with its OWN proto table (the tile-local palette). The
    // renderer records one DrawIndexedIndirect per (tile, local proto).
    void RecordCullChain(smvk::ResourceManager& ctx,
                         smvk::RenderGraph& rg,
                         uint32_t view,
                         smvk::LogicalBufferHandle renderViewsLogical,
                         uint64_t renderViewsBDA,
                         uint32_t renderViewIndex,
                         const glm::vec3& camPos,
                         uint32_t cullFlags) {
        if (!created || view >= viewBudget)
            return;
        for (size_t s = 0; s < active.size(); ++s) {
            const ActiveTile& at = active[s];
            if (!at.published || at.protoCount == 0 || at.protoTableBuffer.empty())
                continue;
            PlacementVisibleSet& vs = vis[s * size_t(viewBudget) + size_t(view)];
            vs.ClearFrame(ctx); // the mask + bin atomics (per-frame zero)
            // The baked path has no constraint, so the alive test is off and
            // there is no LayerBinding to source proto bounds from — the
            // pushes are filled DIRECTLY from the tile's local proto table.
            PlacementCullPush cullPush{};
            cullPush.instances = pass.TileInstancesAddress(ctx, uint32_t(s));
            cullPush.count = pass.TileCountAddress(ctx, uint32_t(s));
            cullPush.mask = vs.MaskAddress(ctx);
            cullPush.bins = vs.BinsAddress(ctx);
            cullPush.alive = 0;
            cullPush.renderViews = renderViewsBDA;
            cullPush.renderViewIndex = renderViewIndex;
            cullPush.cullFlags = cullFlags;
            cullPush.protoTable = ctx.GetBufferDeviceAddress(at.protoTableBuffer);
            cullPush.protoCount = at.protoCount;
            cullPush.camPos = camPos;
            cullPush.maxInstanceScale = 1.0f;
            PlacementScatterVisPush scPush{};
            scPush.instances = pass.TileInstancesAddress(ctx, uint32_t(s));
            scPush.count = pass.TileCountAddress(ctx, uint32_t(s));
            scPush.mask = vs.MaskAddress(ctx);
            scPush.bins = vs.BinsAddress(ctx);
            scPush.visible = vs.VisibleAddress(ctx);
            scPush.protoCount = at.protoCount;
            PlacementDrawBuildPush dbPush{};
            dbPush.bins = vs.BinsAddress(ctx);
            dbPush.indirect = vs.IndirectAddress(ctx);
            dbPush.protoTable = ctx.GetBufferDeviceAddress(at.protoTableBuffer);
            dbPush.protoCount = at.protoCount;

            smvk::RenderPass& cull = PlacementCullPrograms::AddCullPass(
                rg, pass, uint32_t(s), vs, smvk::LogicalBufferHandle{}, renderViewsLogical);
            const uint32_t capacity = pass.desc.maxInstancesPerTile;
            cull.SetRecordingFunc([this, cullPush, capacity](smvk::CommandBuffer& cmd) {
                PlacementCullPrograms::PushAndDispatchCull(
                    cmd, cullPrograms.cullProgram, cullPush, capacity);
            });
            smvk::RenderPass& pre = PlacementCullPrograms::AddPrefixPass(rg, vs);
            pre.SetRecordingFunc([this, cullPush](smvk::CommandBuffer& cmd) {
                PlacementCullPrograms::PushAndDispatchPrefix(
                    cmd, cullPrograms.prefixProgram, cullPush);
            });
            smvk::RenderPass& sc =
                PlacementCullPrograms::AddScatterVisPass(rg, pass, uint32_t(s), vs);
            sc.SetRecordingFunc([this, scPush, capacity](smvk::CommandBuffer& cmd) {
                PlacementCullPrograms::PushAndDispatchScatterVis(
                    cmd, cullPrograms.scatterProgram, scPush, capacity);
            });
            smvk::RenderPass& db = PlacementCullPrograms::AddDrawBuildPass(rg, vs);
            db.SetRecordingFunc([this, dbPush](smvk::CommandBuffer& cmd) {
                PlacementCullPrograms::PushAndDispatchDrawBuild(
                    cmd, cullPrograms.buildProgram, dbPush);
            });
            // The read-only proto table feeds both the cull (bounds/distance)
            // and the draw-build (index range) — hint the shared read.
            if (!at.protoTableLogical.empty()) {
                smvk::BufferSyncRequest protoRead{};
                protoRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
                protoRead.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
                cull.AddBufferHint(at.protoTableLogical, protoRead);
                db.AddBufferHint(at.protoTableLogical, protoRead);
            }
        }
    }

  private:
    // Nearest-first tile selection, bounded by the slot budget, restricted to
    // tiles INTERSECTING an interest rect (the same residency semantics as the
    // procedural BudgetTileInterests — never request more tiles than slots).
    // Defined out-of-line below the class (needs the member tileSize).
    void SelectTiles(const glm::vec4* interests,
                     size_t interestCount,
                     const std::vector<BakedPlacementTile>& tiles,
                     size_t maxTiles,
                     std::vector<size_t>& outSrc);

    [[nodiscard]] int32_t FindSlot(const TileID& key) const {
        for (size_t s = 0; s < active.size(); ++s)
            if (active[s].published && active[s].key == key)
                return int32_t(s);
        return -1;
    }
    [[nodiscard]] int32_t ClaimSlot() {
        for (size_t s = 0; s < active.size(); ++s)
            if (!active[s].published)
                return int32_t(s);
        int32_t lru = -1;
        uint64_t oldest = UINT64_MAX;
        for (size_t s = 0; s < active.size(); ++s) {
            if (active[s].lastUse < oldest) {
                oldest = active[s].lastUse;
                lru = int32_t(s);
            }
        }
        return lru;
    }

    void UploadTile(smvk::ResourceManager& ctx,
                    smvk::RenderGraph& rg,
                    uint32_t slot,
                    int32_t srcIndex,
                    const BakedPlacementTile& tile) {
        active[slot].Destroy(rg); // free the previous tenant's proto table
        const size_t n = tile.instances.size();
        if (n > maxInstancesPerTile) {
            std::fprintf(stderr,
                         "[BakedPlacement] tile {%d,%d} has %zu instances > capacity %u "
                         "(reduce tileSize or raise maxInstancesPerTile)\n",
                         tile.key.x,
                         tile.key.z,
                         n,
                         maxInstancesPerTile);
            return; // loud: never a truncated silent success
        }
        // The baked points upload as a TRANSFER write read at COMPUTE scope —
        // the same read-by-compute pattern as the proto table / heightfield.
        if (n > 0) {
            ctx.UploadDataToBuffer(pass.instanceBuffers[slot],
                                   tile.instances.data(),
                                   n * sizeof(RenderInstance),
                                   smvk::PipelineStageBit_Compute,
                                   smvk::AccessFlags::ShaderRead,
                                   0,
                                   0);
        }
        PlacementCountGpu cnt{};
        cnt.count = uint32_t(n);
        cnt.capacity = maxInstancesPerTile;
        ctx.UploadDataToBuffer(pass.countBuffers[slot],
                               &cnt,
                               sizeof(cnt),
                               smvk::PipelineStageBit_Compute,
                               smvk::AccessFlags::ShaderRead,
                               0,
                               0);
        // Build + upload the tile's LOCAL proto table.
        const uint32_t protoCount = uint32_t(tile.localProtos.size());
        std::vector<PlacementProtoRow> rows(protoCount);
        for (uint32_t i = 0; i < protoCount; ++i) {
            const BakedProtoGeometry& g = (*protoGeometry)[tile.localProtos[i]];
            const glm::vec3 center = (g.boundsMin + g.boundsMax) * 0.5f;
            const glm::vec3 halfDiag = (g.boundsMax - g.boundsMin) * 0.5f;
            rows[i].bounds = glm::vec4(center, glm::length(halfDiag));
            rows[i].drawDistance = g.drawDistance;
            rows[i].indexCount = g.indexCount;
            rows[i].firstIndex = g.firstIndex;
            rows[i].vertexOffset = g.vertexOffset;
        }
        active[slot].protoTableBuffer = ctx.CreateBuffer(protoCount * sizeof(PlacementProtoRow),
                                                         smvk::BufferType_Storage,
                                                         "baked placement proto table");
        if (!active[slot].protoTableBuffer.empty()) {
            ctx.UploadDataToBuffer(active[slot].protoTableBuffer,
                                   rows.data(),
                                   protoCount * sizeof(PlacementProtoRow),
                                   smvk::PipelineStageBit_Compute,
                                   smvk::AccessFlags::ShaderRead,
                                   0,
                                   0);
            active[slot].protoTableLogical =
                rg.AddExternalBuffer(active[slot].protoTableBuffer, "baked placement proto table");
        }
        active[slot].key = tile.key;
        active[slot].srcIndex = srcIndex;
        active[slot].protoCount = protoCount;
        active[slot].lastUse = tick;
        active[slot].published = !active[slot].protoTableLogical.empty();
    }
};

// Out-of-line (needs tileSize): nearest-first, intersecting-tile selection.
inline void BakedPlacementRuntime::SelectTiles(const glm::vec4* interests,
                                               size_t interestCount,
                                               const std::vector<BakedPlacementTile>& tiles,
                                               size_t maxTiles,
                                               std::vector<size_t>& outSrc) {
    outSrc.clear();
    if (!interests || maxTiles == 0)
        return;
    struct Ranked {
        size_t idx;
        double d2;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(tiles.size());
    for (size_t i = 0; i < tiles.size(); ++i) {
        const BakedPlacementTile& t = tiles[i];
        const double cx = (double(t.key.x) + 0.5) * tileSize;
        const double cz = (double(t.key.z) + 0.5) * tileSize;
        bool intersects = false;
        double bestD2 = std::numeric_limits<double>::max();
        for (size_t r = 0; r < interestCount; ++r) {
            const glm::vec4 rect = interests[r];
            if (!(rect.z > rect.x && rect.w > rect.y))
                continue;
            const double tx0 = double(t.key.x) * tileSize;
            const double tx1 = tx0 + tileSize;
            const double tz0 = double(t.key.z) * tileSize;
            const double tz1 = tz0 + tileSize;
            if (!(tx1 > double(rect.x) && tx0 < double(rect.z) && tz1 > double(rect.y) &&
                  tz0 < double(rect.w)))
                continue;
            intersects = true;
            const double dx = cx - (double(rect.x) + double(rect.z)) * 0.5;
            const double dz = cz - (double(rect.y) + double(rect.w)) * 0.5;
            const double d2 = dx * dx + dz * dz;
            if (d2 < bestD2)
                bestD2 = d2;
        }
        if (!intersects)
            continue;
        ranked.push_back({i, bestD2});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.d2 != b.d2)
            return a.d2 < b.d2;
        return a.idx < b.idx; // stable tiebreak
    });
    outSrc.reserve(std::min(maxTiles, ranked.size()));
    for (size_t i = 0; i < ranked.size() && outSrc.size() < maxTiles; ++i)
        outSrc.push_back(ranked[i].idx);
}

} // namespace tour::placement

// COMPAT (the namespace/RHI boundary audit): smvk::placement aliases.
namespace smvk::placement {
using ::tour::placement::BakedPlacementCooked;
using ::tour::placement::BakedPlacementRuntime;
using ::tour::placement::BakedPlacementTile;
using ::tour::placement::BakedProtoGeometry;
using ::tour::placement::BakedSurfaceRef;
using ::tour::placement::BucketBakedPlacement;
using ::tour::placement::CookBakedPlacement;
using ::tour::placement::DeserializeBakedPlacementCooked;
using ::tour::placement::ResolveBakedPrototypes;
using ::tour::placement::SerializeBakedPlacementCooked;
} // namespace smvk::placement
