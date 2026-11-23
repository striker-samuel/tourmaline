// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Baked placement asset: an AUTHORED point cloud (a USD PointInstancer —
// vegetation/clutter) serialized as explicit points + a render-prototype
// table. Produced by the gltf/usd importer, baked into the pak, and consumed
// by the BakedPlacementRuntime (the existing PR03 cull -> prefix -> scatter ->
// drawbuild instanced-draw chain, without the procedural tile-generation leg).
//
// The format is explicit about the authored-vs-render prototype distinction,
// which is the correctness-critical part of the PointInstancer import:
//
//   A USD PointInstancer references AUTHored prototypes via protoIndices. A
//   single authored prototype is a SUBTREE, and CollectProtoLeaves walks it to
//   produce ZERO OR MORE leaf meshes (ProtoLeaf{mesh, local}). There is NO 1:1
//   authored-prototype -> mesh mapping.
//
//   This asset therefore expands during import into RENDER prototypes — one
//   per (leaf mesh, material, surface) — and one POINT per (instance, leaf).
//   The leaf's transform inside the prototype root (leaf.local) is folded into
//   the point's transform, so the render prototype itself is pure geometry +
//   material identity.
//
// Identity: points carry a 128-bit StableID derived from (instancer path,
// instance ordinal, leaf ordinal) so overrides/debug survive renames and
// reordering the same way the procedural placement's StableIDs do.

#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_ids.h>

namespace tour::placement {

// ---------------------------------------------------------------------------
// A render prototype: the smallest drawable unit — ONE (mesh, material, surface).
// A leaf mesh with several primitives expands to several render prototypes.
// ---------------------------------------------------------------------------
struct BakedRenderPrototype {
    // The owning .geometry ASSET, as the project's MeshKey (the content-as-code
    // catalog id, a uint64 — resolved by the project pipeline, not the raw
    // import). 0 at import (the raw import runs before the catalog). Combined
    // with surfaceIndex below this addresses ONE flat surface.
    uint64_t meshKey = 0;
    uint32_t materialId = 0;   // world material (resolved cook-side)
    uint32_t surfaceIndex = 0; // the flat surface (the .geometry blob index) within the asset
    uint32_t flags = 0;        // reserved (e.g. future doubleSided/masked hints)
};
static_assert(sizeof(BakedRenderPrototype) == 24, "keep the packed layout");

// ---------------------------------------------------------------------------
// A baked point: one (instance, leaf) pair with the COMPOSED transform
// (instanceXf * leaf.local) decomposed to translation + full quaternion +
// uniform scale. The quaternion preserves the terrain-aligned tilt the
// yaw-only RenderInstance could not represent.
// ---------------------------------------------------------------------------
struct BakedPlacementPoint {
    glm::vec3 position{0.0f};                     // 12
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // 16 quaternion (xyzw)
    glm::vec3 scale{1.0f};                        // 12 (UNIFORM — validated at import; see flags)
    uint32_t renderPrototypeId = 0;               // 4  index into the asset's renderPrototypes
    StableID stableId{};                          // 16 (4x32) — authored identity
    uint32_t flags = 0;                           // 4  see PointFlag below
    uint32_t reserved = 0;                        // 4
};
static_assert(sizeof(BakedPlacementPoint) == 68, "keep the packed layout");

namespace BakedPointFlag {
// The composed transform (instanceXf * leaf.local) was NOT decomposable to
// (translation, yaw, uniform scale) — the leaf carried pitch/roll/mirroring or
// non-uniform scale. The point still records the best-effort yaw/uniform-scale
// decomposition, but the consumer MUST NOT submit it until the leaf's local
// transform is baked into dedicated prototype geometry (a later phase). An
// import that would silently discard orientation sets this flag instead.
inline constexpr uint32_t NeedsGeometryBake = 1u << 0;
// The source scale was non-uniform. UniformScale-equivalent points do NOT set
// this; the consumer treats scale as uniform.
inline constexpr uint32_t NonUniformScale = 1u << 1;
} // namespace BakedPointFlag

// ---------------------------------------------------------------------------
// The asset. Points are FLAT at import (one per instance x leaf); the spatial
// tile/chunk bucketing into capacity-bounded buffers happens at cook.
// ---------------------------------------------------------------------------
struct BakedPlacementAsset {
    std::vector<BakedRenderPrototype> renderPrototypes;
    std::vector<BakedPlacementPoint> points;

    // Spatial envelope (world-space AABB of all points) + the intended tile
    // size. Filled at import; the cook uses it to bucket points into tiles
    // with a LOCAL prototype palette per tile (stable global prototype id kept
    // for the asset identity).
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    float tileSize = 0.0f; // 0 = not yet bucketed (import-time flat form)

    // Name of the source instancer (diagnostics + stable-id input).
    std::string instancerPath;

    void clear() {
        renderPrototypes.clear();
        points.clear();
        boundsMin = boundsMax = glm::vec3(0.0f);
        tileSize = 0.0f;
        instancerPath.clear();
    }
    [[nodiscard]] bool empty() const {
        return points.empty();
    }
};

// A world-level declaration of a baked placement cloud: the .geometry family
// it was cooked beside + the residency knobs. The world loader populates these
// (the "prespecified instancers"); the renderer auto-loads them — no per-app
// runtime wiring. Distinct from BakedPlacementAsset (the flat import form).
struct BakedPlacementRef {
    std::string familyPath;       // "<stem>.geometry" the cloud was cooked beside
    float interestRadius = 64.0f; // camera radius that keeps tiles resident
    uint32_t slotBudget = 32;     // concurrent resident GPU tiles
};

// ---------------------------------------------------------------------------
// Binary serialization (Phase 1 round-trip). Little-endian, versioned, and
// lossless: the round-trip test asserts serialize(deserialize(x)) == x, i.e.
// source transforms and counts survive exactly.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kBakedPlacementMagic = 0x424B504C; // 'BKPL'
inline constexpr uint32_t kBakedPlacementVersion =
    2; // v2: BakedRenderPrototype.meshKey (uint64) + surfaceIndex

[[nodiscard]] inline bool SerializeBakedPlacement(const BakedPlacementAsset& asset,
                                                  std::vector<uint8_t>& out) {
    out.clear();
    const uint32_t protoCount = static_cast<uint32_t>(asset.renderPrototypes.size());
    const uint32_t pointCount = static_cast<uint32_t>(asset.points.size());
    if (protoCount != asset.renderPrototypes.size() || pointCount != asset.points.size())
        return false; // count overflow — refuse rather than truncate
    const uint32_t pathLen = static_cast<uint32_t>(asset.instancerPath.size());
    if (pathLen != asset.instancerPath.size())
        return false;
    const size_t body = sizeof(uint32_t) * 5 + // magic,ver,proto,point,pathLen
                        sizeof(BakedRenderPrototype) * protoCount +
                        sizeof(BakedPlacementPoint) * pointCount + sizeof(glm::vec3) * 2 +
                        sizeof(float) + // boundsMin/Max, tileSize
                        pathLen;
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
    u = kBakedPlacementMagic;
    if (!put(&u, 4))
        return false;
    u = kBakedPlacementVersion;
    if (!put(&u, 4))
        return false;
    u = protoCount;
    if (!put(&u, 4))
        return false;
    u = pointCount;
    if (!put(&u, 4))
        return false;
    u = pathLen;
    if (!put(&u, 4))
        return false;
    if (protoCount &&
        !put(asset.renderPrototypes.data(), sizeof(BakedRenderPrototype) * protoCount))
        return false;
    if (pointCount && !put(asset.points.data(), sizeof(BakedPlacementPoint) * pointCount))
        return false;
    if (!put(&asset.boundsMin, sizeof(glm::vec3)))
        return false;
    if (!put(&asset.boundsMax, sizeof(glm::vec3)))
        return false;
    if (!put(&asset.tileSize, sizeof(float)))
        return false;
    if (pathLen && !put(asset.instancerPath.data(), pathLen))
        return false;
    return off == out.size();
}

[[nodiscard]] inline bool DeserializeBakedPlacement(const uint8_t* data,
                                                    size_t size,
                                                    BakedPlacementAsset& out) {
    out.clear();
    size_t off = 0;
    auto get = [&](void* p, size_t n) {
        if (off + n > size)
            return false;
        std::memcpy(p, data + off, n);
        off += n;
        return true;
    };
    uint32_t magic = 0, version = 0, protoCount = 0, pointCount = 0, pathLen = 0;
    if (!get(&magic, 4) || magic != kBakedPlacementMagic)
        return false;
    if (!get(&version, 4) || version != kBakedPlacementVersion)
        return false;
    if (!get(&protoCount, 4))
        return false;
    if (!get(&pointCount, 4))
        return false;
    if (!get(&pathLen, 4))
        return false;
    if (protoCount) {
        out.renderPrototypes.resize(protoCount);
        if (!get(out.renderPrototypes.data(), sizeof(BakedRenderPrototype) * protoCount))
            return false;
    }
    if (pointCount) {
        out.points.resize(pointCount);
        if (!get(out.points.data(), sizeof(BakedPlacementPoint) * pointCount))
            return false;
    }
    if (!get(&out.boundsMin, sizeof(glm::vec3)))
        return false;
    if (!get(&out.boundsMax, sizeof(glm::vec3)))
        return false;
    if (!get(&out.tileSize, sizeof(float)))
        return false;
    if (pathLen) {
        out.instancerPath.resize(pathLen);
        if (!get(out.instancerPath.data(), pathLen))
            return false;
    }
    return off == size;
}

} // namespace tour::placement

// COMPAT: the placement namespace alias, matching the sibling placement headers.
namespace smvk::placement {
using ::tour::placement::BakedPlacementAsset;
using ::tour::placement::BakedPlacementPoint;
using ::tour::placement::BakedPlacementRef;
using ::tour::placement::BakedRenderPrototype;
using ::tour::placement::DeserializeBakedPlacement;
using ::tour::placement::SerializeBakedPlacement;
} // namespace smvk::placement
