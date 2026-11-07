// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Strict authored .map text reader. This is deliberately independent of
// local .tour Scene playback: one Map owns world-wide placements, not meshes.
#include <Engine/WorldPartition/WorldMap.h>
#include <glm/gtc/matrix_transform.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tour::mapcook {

// Format: map 1 WORLD_ID
//         node ID PARENT_ID SCENE_ASSET_ID PERSISTENT TX TY TZ
// PARENT_ID = 0 for roots; SCENE_ASSET_ID = 0 for grouping anchors.
// IDs are unsigned decimal; transforms are local translations in metres.
// Full authored rotation/scale is stored as a 4x4 matrix via "matrix":
//         matrix ID PARENT_ID SCENE_ASSET_ID PERSISTENT 16_DOUBLES_COLUMN_MAJOR
// Whole document must be valid; output remains unchanged on errors.
inline bool ParseMap(std::string_view input, Map& output, std::string& error) {
    error.clear();
    std::istringstream lines{std::string(input)};
    std::string line;
    Map candidate{};
    struct Pending {
        uint64_t parentId = 0;
        MapNode node;
    };
    std::vector<Pending> pending;
    size_t lineNumber = 0;
    bool sawHeader = false;
    auto fail = [&](const char* reason) {
        error = "map line " + std::to_string(lineNumber) + ": " + reason;
        return false;
    };
    while (std::getline(lines, line)) {
        ++lineNumber;
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#')
            continue;
        std::istringstream in(line.substr(start));
        std::string command, trailing;
        in >> command;
        if (!sawHeader) {
            uint32_t version = 0;
            if (command != "map" || !(in >> version >> candidate.id) || version != 1 ||
                candidate.id == 0 || (in >> trailing))
                return fail("expected 'map 1 WORLD_ID'");
            sawHeader = true;
            continue;
        }
        if (command != "node" && command != "matrix")
            return fail("expected node or matrix");
        if (pending.size() >= (1u << 20))
            return fail("map node count exceeds limit");
        Pending rec{};
        unsigned persistent = 0;
        if (!(in >> rec.node.id >> rec.parentId >> rec.node.sceneAssetId >> persistent) ||
            persistent > 1 || !rec.node.id)
            return fail("invalid node identifiers/flags");
        rec.node.persistent = persistent != 0;
        if (command == "node") {
            double x, y, z;
            if (!(in >> x >> y >> z))
                return fail("node requires local translation XYZ");
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                return fail("nonfinite translation");
            rec.node.localTransform = glm::translate(glm::dmat4(1), glm::dvec3(x, y, z));
        } else {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (!(in >> rec.node.localTransform[c][r]) ||
                        !std::isfinite(rec.node.localTransform[c][r]))
                        return fail("matrix requires 16 finite column-major doubles");
        }
        if (in >> trailing)
            return fail("unexpected trailing tokens");
        pending.push_back(std::move(rec));
    }
    if (!sawHeader) {
        error = "missing map header";
        return false;
    }
    std::unordered_map<uint64_t, uint32_t> slots;
    for (uint32_t i = 0; i < pending.size(); ++i)
        if (!slots.emplace(pending[i].node.id, i).second) {
            error = "duplicate map node ID: " + std::to_string(pending[i].node.id);
            return false;
        }
    candidate.nodes.reserve(pending.size());
    for (auto& rec : pending) {
        if (rec.parentId != 0) {
            const auto it = slots.find(rec.parentId);
            if (it == slots.end()) {
                error = "missing map parent ID: " + std::to_string(rec.parentId);
                return false;
            }
            rec.node.parent = it->second;
        }
        candidate.nodes.push_back(std::move(rec.node));
    }
    CompiledMapTiles verified;
    if (!CompileMapTilePlacements(candidate, verified)) {
        error = "map has invalid hierarchy, transforms or ownership";
        return false;
    }
    output = std::move(candidate);
    return true;
}
} // namespace tour::mapcook
