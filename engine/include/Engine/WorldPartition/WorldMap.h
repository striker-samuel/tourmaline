// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Authoring map: world-wide placement hierarchy, NOT a renderable Scene.
// A Map placement references an independently cooked local Scene (interior,
// prefab, or reusable spatial bundle). Map nodes are not World node handles.
// CompileMapTilePlacements is a deterministic cook-side first stage, not a
// substitute for spatial bounds/LOD/BVH cooking or runtime activation.

#include <Engine/WorldPartition/WorldPartition.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace tour {

using MapNodeId = uint32_t;
inline constexpr MapNodeId kMapNodeInvalid = UINT32_MAX;

// Stable authored ID survives recooks; array indices do not.
struct MapNode {
    uint64_t id = 0;
    MapNodeId parent = kMapNodeInvalid;
    glm::dmat4 localTransform{1.0};
    uint64_t sceneAssetId = 0; // 0 = organizational hierarchy node
    bool persistent = false;   // persistent placement, never tile-evicted
};

struct Map {
    engine::worldpartition::WorldId id = 0; // stable authored, never inferred from pak name
    std::vector<MapNode> nodes;
};

// A cooked tile placement refers to one local scene, not copied geometry.
// Static BVH leaves will eventually use stable per-scene instance identities.
struct CookedMapPlacement {
    uint64_t mapNodeId = 0;
    uint64_t sceneAssetId = 0;
    glm::dmat4 worldTransform{1.0};
};

struct MapCellLess {
    bool operator()(engine::worldpartition::WorldCellKey a,
                    engine::worldpartition::WorldCellKey b) const {
        return a.x < b.x || (a.x == b.x && a.z < b.z);
    }
};

struct CompiledMapTiles {
    engine::worldpartition::WorldId world = 0;
    std::map<engine::worldpartition::WorldCellKey,
             std::vector<CookedMapPlacement>, MapCellLess> tiles;
    std::vector<CookedMapPlacement> persistent;
};

// Returns false for duplicate/zero IDs, invalid parents, cycles, nonfinite
// matrices, or tile index overflow; leaves output unchanged on failure.
// Node order does not affect the output tile/placement ordering.
inline bool CompileMapTilePlacements(const Map& map, CompiledMapTiles& output) {
    CompiledMapTiles next;
    if (map.id == 0) return false; // world identity is mandatory for cooked Maps
    next.world = map.id;
    const size_t n = map.nodes.size();
    if (n > uint64_t(UINT32_MAX)) return false;

    std::vector<glm::dmat4> globals(n, glm::dmat4(1.0));
    std::vector<uint8_t> visit(n, 0);
    std::vector<uint64_t> ids;
    ids.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& node = map.nodes[i];
        if (node.id == 0 || (node.parent != kMapNodeInvalid && node.parent >= n))
            return false;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(node.localTransform[c][r])) return false;
        ids.push_back(node.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        return false;

    // Iterative ancestry evaluation avoids recursion on malformed/deep maps.
    for (size_t i = 0; i < n; ++i) {
        std::vector<MapNodeId> chain;
        MapNodeId current = static_cast<MapNodeId>(i);
        while (current != kMapNodeInvalid && visit[current] != 2) {
            if (visit[current] == 1) return false; // ancestor cycle
            visit[current] = 1;
            chain.push_back(current);
            current = map.nodes[current].parent;
        }
        while (!chain.empty()) {
            const MapNodeId idx = chain.back();
            chain.pop_back();
            const MapNode& node = map.nodes[idx];
            globals[idx] = node.localTransform;
            if (node.parent != kMapNodeInvalid)
                globals[idx] = globals[node.parent] * globals[idx];
            visit[idx] = 2;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const auto& node = map.nodes[i];
        if (!node.sceneAssetId) continue;
        const auto& m = globals[i];
        bool finite = true;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                finite &= std::isfinite(m[c][r]);
        if (!finite) return false;
        CookedMapPlacement record{node.id, node.sceneAssetId, m};
        if (node.persistent) {
            next.persistent.push_back(record);
            continue;
        }
        // The root position selects a canonical owner cell. Spatial bounds
        // are NOT used here: later cooking must add conservative visibility
        // references for scenes overlapping other cells without duplicating
        // their logical owner or gameplay state.
        const double x = std::floor(m[3][0] / engine::worldpartition::kRootCellSize);
        const double z = std::floor(m[3][2] / engine::worldpartition::kRootCellSize);
        const double lo = double(std::numeric_limits<int64_t>::min());
        const double hi = std::nextafter(double(std::numeric_limits<int64_t>::max()),
                                          -std::numeric_limits<double>::infinity());
        if (!std::isfinite(x) || !std::isfinite(z)
            || x < lo || x >= hi || z < lo || z >= hi)
            return false;
        next.tiles[{static_cast<int64_t>(x), static_cast<int64_t>(z)}]
            .push_back(record);
    }
    auto stable = [](const CookedMapPlacement& a, const CookedMapPlacement& b) {
        return a.mapNodeId < b.mapNodeId;
    };
    for (auto& [key, placements] : next.tiles)
        std::sort(placements.begin(), placements.end(), stable);
    std::sort(next.persistent.begin(), next.persistent.end(), stable);
    output = std::move(next);
    return true;
}

} // namespace tour
