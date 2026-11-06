// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Map hierarchy is authored world placement, NOT an in-memory Scene clone.
#include <Engine/WorldPartition/WorldMap.h>
#include <Engine/WorldPartition/map_tile_codec.h>
#include <Engine/WorldPartition/map_tile_pak.h>
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
    {
        const engine::worldpartition::WorldCellKey key{-2,2};
        std::vector<uint8_t> bytes, bytes2;
        const auto& placements = out.tiles.at(key);
        Check(tour::mapcook::EncodeTile(key, placements, bytes)
            && tour::mapcook::EncodeTile(key, placements, bytes2)
            && bytes == bytes2, "tile encoding deterministic");
        std::vector<tour::CookedMapPlacement> decoded;
        Check(tour::mapcook::DecodeTile(bytes, key, decoded)
            && decoded.size() == 1
            && decoded[0].mapNodeId == 20
            && decoded[0].sceneAssetId == 202,
            "tile page roundtrip");
        Check(!tour::mapcook::DecodeTile(bytes, {1,2}, decoded),
            "cross-cell page substitution rejected");
        bytes.pop_back();
        Check(!tour::mapcook::DecodeTile(bytes, key, decoded)
            && decoded.size() == 1, "truncated page rejected transactionally");
    }
    {
        smvk::PakWriter writer;
        Check(tour::mapcook::AddMapTiles(writer, out),
            "map tiles packed via SMP8 writer");
        const std::vector<uint8_t> image = writer.Finalize();
        std::string err;
        auto reader = smvk::PakReader::Open(image, err);
        Check(reader.has_value(), "SMP8 map pages parse");
        if (reader) {
            std::vector<tour::CookedMapPlacement> decoded;
            Check(tour::mapcook::ReadMapTile(*reader, {-2,2}, decoded)
                && decoded.size() == 1
                && decoded[0].mapNodeId == 20,
                "tile fetched by logical cell without Cluster");
            Check(!tour::mapcook::ReadMapTile(*reader, {9,9}, decoded)
                && decoded.size() == 1,
                "missing tile leaves output unchanged");
            Check(reader->BlobsOfKind(smvk::PakAssetKind::MapTile).size() == 2,
                "two independent MapTile pages");
        }
    }
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
