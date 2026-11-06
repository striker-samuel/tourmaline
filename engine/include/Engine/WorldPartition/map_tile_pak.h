// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Map tile page adapter for the existing SMP8 archive. Tile identity is
// logical; one pak can store any number of tiles. Pages are individually
// checksummed/seekable by the PakReader, not loaded as one Cluster.
#include <Engine/WorldPartition/map_tile_codec.h>
#include <Engine/Asset/pak.h>

#include <string>
#include <vector>

namespace tour::mapcook {

inline std::string TilePath(engine::worldpartition::WorldTileKey tile) {
    return "worlds/" + std::to_string(tile.world) + "/tiles/" +
        std::to_string(tile.cell.x) + "_" +
        std::to_string(tile.cell.z) + ".mtile";
}

// Does not finalize the pak. Caller may add other assets and choose regional
// packaging. Fail-fast on invalid placement page or repeated logical key.
inline bool AddMapTiles(smvk::PakWriter& writer, const CompiledMapTiles& tiles) {
    if (!tiles.world) return false;
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
