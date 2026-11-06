// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Map tile page adapter for the existing SMP8 archive. Tile identity is
// logical; one pak can store any number of tiles. Pages are individually
// checksummed/seekable by the PakReader, not loaded as one Cluster.
#include <Engine/WorldPartition/map_tile_codec.h>
#include <Engine/Asset/pak.h>
#include <Engine/WorldPartition/SceneTileProvider.h>

#include <charconv>
#include <map>
#include <string_view>

#include <string>
#include <vector>

namespace tour::mapcook {

inline std::string TilePath(engine::worldpartition::WorldTileKey tile) {
    return "worlds/" + std::to_string(tile.world) + "/tiles/" +
        std::to_string(tile.cell.x) + "_" +
        std::to_string(tile.cell.z) + ".mtile";
}

// The pak TOC is already a bounded, indexed metadata manifest. Discover
// logical tiles without fetching/allocating any tile payloads. The provider
// reads one active Map at a time; other Maps may coexist in the same pak.
inline bool DiscoverMapTiles(const smvk::PakReader& reader,
                             engine::worldpartition::WorldId world,
                             std::vector<engine::worldpartition::SceneTileDescriptor>& out) {
    if (!world) return false;
    uint32_t expectedTiles = 0, expectedPersistent = 0;
    if (!ReadMapManifest(reader, world, expectedTiles, expectedPersistent))
        return false;
    std::map<engine::worldpartition::WorldTileKey, uint64_t,
             engine::worldpartition::WorldTileKeyLess> found;
    const std::string prefix = "worlds/" + std::to_string(world) + "/tiles/";
    constexpr std::string_view suffix = ".mtile";
    for (const auto* entry : reader.BlobsOfKind(smvk::PakAssetKind::MapTile)) {
        const std::string_view path(entry->path);
        if (!path.starts_with(prefix)) continue;
        if (!path.ends_with(suffix)) return false;
        const std::string_view base =
            path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
        const size_t sep = base.find('_');
        if (sep == std::string_view::npos || base.find('_', sep + 1) != std::string_view::npos)
            return false;
        int64_t x = 0, z = 0;
        const auto left = base.substr(0, sep);
        const auto right = base.substr(sep + 1);
        const auto px = std::from_chars(left.data(), left.data() + left.size(), x);
        const auto pz = std::from_chars(right.data(), right.data() + right.size(), z);
        if (px.ec != std::errc{} || pz.ec != std::errc{}
            || px.ptr != left.data() + left.size()
            || pz.ptr != right.data() + right.size())
            return false;
        const engine::worldpartition::WorldTileKey key{world, {x,z}};
        // The request payload identity is an archive-local slot ticket.
        // Stable spatial identity is the WorldTileKey, never this slot.
        if (!found.emplace(key, uint64_t(entry->slot) + 1).second) return false;
    }
    if (found.size() != expectedTiles) return false;
    std::vector<engine::worldpartition::SceneTileDescriptor> next;
    next.reserve(found.size());
    for (const auto& [key, slot] : found)
        next.push_back({key, slot});
    out = std::move(next);
    return true;
}

inline std::string ManifestPath(engine::worldpartition::WorldId world) {
    return "worlds/" + std::to_string(world) + "/manifest.mmap";
}

// Fixed-width metadata page. Required even for empty Maps so a world ID
// exists in the archive without any resident scene placement or tile.
inline constexpr uint32_t kMapManifestMagic = 0x31504D4D; // "MMP1" LE
inline bool ReadMapManifest(const smvk::PakReader& reader,
                            engine::worldpartition::WorldId world,
                            uint32_t& tileCount, uint32_t& persistentCount) {
    if (!world) return false;
    const auto* entry = reader.Find(smvk::PakAssetKind::MapManifest,
                                     ManifestPath(world));
    if (!entry) return false;
    try {
        const auto bytes = reader.Payload(*entry);
        if (bytes.size() != 24) return false;
        size_t pos = 0;
        uint32_t magic = 0, version = 0, tiles = 0, persistent = 0;
        uint64_t id = 0;
        if (!ReadU32(bytes, pos, magic) || magic != kMapManifestMagic
            || !ReadU32(bytes, pos, version) || version != 1
            || !ReadU64(bytes, pos, id) || id != world
            || !ReadU32(bytes, pos, tiles)
            || !ReadU32(bytes, pos, persistent))
            return false;
        tileCount = tiles;
        persistentCount = persistent;
        return true;
    } catch (...) { return false; }
}

inline std::string PersistentPath(engine::worldpartition::WorldId world) {
    return "worlds/" + std::to_string(world) + "/persistent.mtile";
}

// A non-spatial record is a distinct pak path: never included in resident
// cell enumeration. Its page uses MTL2's record layout with reserved cell
// coordinate (0,0), but cannot collide with an actual (0,0) tile path.
inline bool ReadPersistentPlacements(
    const smvk::PakReader& reader, engine::worldpartition::WorldId world,
    std::vector<CookedMapPlacement>& out) {
    if (!world) return false;
    const auto* entry = reader.Find(smvk::PakAssetKind::MapTile,
                                     PersistentPath(world));
    if (!entry) { out.clear(); return true; }
    try {
        return DecodeTile(reader.Payload(*entry), {world, {0,0}}, out);
    } catch (...) { return false; }
}

// Does not finalize the pak. Caller may add other assets and choose regional
// packaging. Fail-fast on invalid placement page or repeated logical key.
inline bool AddMapTiles(smvk::PakWriter& writer, const CompiledMapTiles& tiles) {
    if (!tiles.world || tiles.tiles.size() > uint64_t(UINT32_MAX)
        || tiles.persistent.size() > uint64_t(UINT32_MAX))
        return false;
    std::vector<uint8_t> header;
    WriteU32(header, kMapManifestMagic);
    WriteU32(header, 1);
    WriteU64(header, tiles.world);
    WriteU32(header, uint32_t(tiles.tiles.size()));
    WriteU32(header, uint32_t(tiles.persistent.size()));
    if (!writer.AddBlob(smvk::PakAssetKind::MapManifest,
                        ManifestPath(tiles.world), std::move(header)))
        return false;
    if (!tiles.persistent.empty()) {
        std::vector<uint8_t> bytes;
        if (!EncodeTile({tiles.world, {0,0}}, tiles.persistent, bytes)
            || !writer.AddBlob(smvk::PakAssetKind::MapTile,
                               PersistentPath(tiles.world), std::move(bytes)))
            return false;
    }
    for (const auto& [cell, placements] : tiles.tiles) {
        const engine::worldpartition::WorldTileKey tile{tiles.world, cell};
        std::vector<uint8_t> bytes;
        if (!EncodeTile(tile, placements, bytes) ||
            !writer.AddBlob(smvk::PakAssetKind::MapTile, TilePath(tile),
                            std::move(bytes)))
            return false;
    }
    return true;
}

// Returns false for no page, corrupt page, wrong cell or invalid payload.
// Output remains unchanged on validation failure.
inline bool ReadMapTile(const smvk::PakReader& reader,
                        engine::worldpartition::WorldTileKey tile,
                        std::vector<CookedMapPlacement>& out) {
    if (!tile.world) return false;
    const auto* entry = reader.Find(smvk::PakAssetKind::MapTile, TilePath(tile));
    if (!entry) return false;
    try {
        return DecodeTile(reader.Payload(*entry), tile, out);
    } catch (...) {
        // CRC/decompress failures must not leak half-initialized placement
        // records to the runtime. The host reports the failing tile/key.
        return false;
    }
}
} // namespace tour::mapcook
