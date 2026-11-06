// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Map hierarchy is authored world placement, NOT an in-memory Scene clone.
#include <Engine/WorldPartition/WorldMap.h>
#include <Engine/WorldPartition/map_tile_codec.h>
#include <Engine/WorldPartition/map_tile_pak.h>
#include <Engine/WorldPartition/map_text.h>
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
    map.id = 0x123456789ULL;
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
        const engine::worldpartition::WorldTileKey key{map.id, {-2,2}};
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
        Check(!tour::mapcook::DecodeTile(bytes, {map.id, {1,2}}, decoded),
            "cross-cell page substitution rejected");
        Check(!tour::mapcook::DecodeTile(bytes, {map.id+1, {-2,2}}, decoded),
            "other world with same tile coordinates rejected");
        bytes.pop_back();
        Check(!tour::mapcook::DecodeTile(bytes, key, decoded)
            && decoded.size() == 1, "truncated page rejected transactionally");
    }
    {
        smvk::PakWriter writer;
        Check(tour::mapcook::AddMapTiles(writer, out),
            "map tiles packed via SMP8 writer");
        std::vector<engine::worldpartition::SceneTileDescriptor> discovery;
        // Also include another world at the very same cell addresses:
        // the pak is a container, not the source of tile identity.
        tour::CompiledMapTiles other = out;
        other.world = map.id + 1;
        Check(tour::mapcook::AddMapTiles(writer, other),
            "one pak carries multiple authored Maps");
        const std::vector<uint8_t> image = writer.Finalize();
        std::string err;
        auto reader = smvk::PakReader::Open(image, err);
        Check(reader.has_value(), "SMP8 map pages parse");
        if (reader) {
            std::vector<tour::CookedMapPlacement> decoded;
            Check(tour::mapcook::ReadMapTile(*reader, {map.id, {-2,2}}, decoded)
                && decoded.size() == 1
                && decoded[0].mapNodeId == 20,
                "tile fetched by logical cell without Cluster");
            Check(!tour::mapcook::ReadMapTile(*reader, {map.id, {9,9}}, decoded)
                && decoded.size() == 1,
                "missing tile leaves output unchanged");
            Check(tour::mapcook::ReadMapTile(
                *reader, {map.id + 1, {-2,2}}, decoded)
                && decoded.size() == 1,
                "world ID namespaces pak tile lookup");
            std::vector<tour::CookedMapPlacement> persistent;
            Check(tour::mapcook::ReadPersistentPlacements(*reader, map.id, persistent)
                && persistent.size() == 1
                && persistent.front().mapNodeId == 40
                && persistent.front().sceneAssetId == 404,
                "persistent placements roundtrip without joining tile residency");
            Check(tour::mapcook::ReadPersistentPlacements(
                *reader, map.id + 1, persistent)
                && persistent.size() == 1,
                "separate world has its own persistent page");
            Check(tour::mapcook::DiscoverMapTiles(*reader, map.id, discovery)
                && discovery.size() == 2
                && discovery.front().tile.world == map.id,
                "discover only active world's tile metadata without reading pages");
            Check(tour::mapcook::DiscoverMapTiles(*reader, map.id + 1, discovery)
                && discovery.size() == 2
                && discovery.front().tile.world == map.id + 1,
                "overlapping cells in second Map remain independent");
            Check(reader->BlobsOfKind(smvk::PakAssetKind::MapTile).size() == 6,
                "four streamed pages and two persistent Map pages");
        }
    }
    Check(out.world == map.id, "compiled map preserves authored world identity");
    tour::Map missingWorld = map;
    missingWorld.id = 0;
    Check(!tour::CompileMapTilePlacements(missingWorld, out),
        "zero world ID rejected");
    {
        const std::string source =
            "map 1 4886718345\\n"
            "node 30 0 0 0 -256 0 512\\n"
            "node 20 30 202 0 -1 0 0\\n"
            "node 10 30 101 0 512 0 0\\n"
            "node 40 30 404 1 0 0 0\\n";
        tour::Map parsed;
        std::string error;
        Check(tour::mapcook::ParseMap(source, parsed, error)
            && parsed.id == map.id && parsed.nodes.size() == 4,
            "strict authored map file parses");
        tour::CompiledMapTiles compiled;
        Check(tour::CompileMapTilePlacements(parsed, compiled)
            && compiled.tiles.size() == 2
            && compiled.persistent.size() == 1,
            "authored map file feeds cooked tiles");
        Check(!tour::mapcook::ParseMap(
            "map 1 99\\nnode 1 2 300 0 0 0 0\\n", parsed, error),
            "unknown authored parent fails closed");
        Check(!tour::mapcook::ParseMap(
            "map 1 99\\nnode 1 0 300 0 0 0 0 trailing\\n",
            parsed, error), "trailing tokens fail closed");
        Check(!tour::mapcook::ParseMap(
            "map 1 99\\nnode 1 2 300 0 0 0 0\\n"
            "node 2 1 0 0 0 0 0\\n",
            parsed, error), "authored map ancestry cycle fails");
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
