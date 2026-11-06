// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Map hierarchy is authored world placement, NOT an in-memory Scene clone.
#include <Engine/WorldPartition/WorldMap.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdlib>
#include <iostream>

static int failures = 0;
static void Check(bool cond, const char* what) {
    if (!cond) { ++failures; std::cerr << "FAIL " << what << "\n"; }
}
static tour::MapNode Node(uint64_t id, uint32_t parent, double x, double z,
                          uint64_t sceneId, bool persistent = false) {
    tour::MapNode n{};
    n.id = id;
    n.parent = parent;
    n.localTransform = glm::translate(glm::dmat4(1.0), glm::dvec3(x, 0.0, z));
    n.sceneAssetId = sceneId;
    n.persistent = persistent;
    return n;
}
int main() {
    tour::Map map{};
    map.nodes = {
        Node(30, tour::kMapNodeInvalid, -256.0, 512.0, 0),
        Node(20, 0, -1.0, 0.0, 202),
        Node(10, 0, 512.0, 0.0, 101),
        Node(40, 0, 0.0, 0.0, 404, true)
    };
    tour::CompiledMapTiles out{};
    Check(tour::CompileMapTilePlacements(map, out), "valid map compiles");
    Check(out.tiles.size() == 2 && out.tiles.count({-2,2}) == 1
        && out.tiles.count({1,2}) == 1, "world transforms and negative cells");
    Check(out.tiles.at({-2,2})[0].sceneAssetId == 202
        && out.tiles.at({1,2})[0].sceneAssetId == 101,
        "scene assets referenced without copying local hierarchy");
    Check(out.persistent.size() == 1 && out.persistent[0].mapNodeId == 40,
        "persistent placement separated from tiles");
    const auto oldTiles = out.tiles.size();
    tour::Map invalid = map;
    invalid.nodes[1].parent = 1;
    Check(!tour::CompileMapTilePlacements(invalid, out)
        && out.tiles.size() == oldTiles, "cycle rejected transactionally");
    invalid = map;
    invalid.nodes[2].id = 20;
    Check(!tour::CompileMapTilePlacements(invalid, out), "duplicate stable ID");
    invalid = map;
    invalid.nodes[2].parent = 99;
    Check(!tour::CompileMapTilePlacements(invalid, out), "bad parent");
    invalid = map;
    invalid.nodes[1].localTransform[0][0] =
        std::numeric_limits<double>::infinity();
    Check(!tour::CompileMapTilePlacements(invalid, out), "nonfinite local");
    std::cout << "world_map failures=" << failures << "\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
