// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Deterministic, bounded little-endian cooked Map tile placement page.
// This codec produces an independently addressable blob suitable for SMP8
// PakWriter; integration into the project cooker and PakReader is a later
// step. Scene geometry and local BVHs remain separate referenced assets.

#include <Engine/WorldPartition/WorldMap.h>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace tour::mapcook {

inline constexpr uint32_t kTileMagic = 0x324C544D; // "MTL2" LE: world-scoped
inline constexpr uint32_t kMaxPlacementsPerTile = (1u << 20);
inline constexpr size_t kPlacementBytes = 16 + (16 * 8);
inline constexpr size_t kTileHeaderBytes = 4 + 4 + 8 + 8 + 8 + 4;

inline void WriteU32(std::vector<uint8_t>& bytes, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        bytes.push_back(uint8_t(v >> (8 * i)));
}
inline void WriteU64(std::vector<uint8_t>& bytes, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        bytes.push_back(uint8_t(v >> (8 * i)));
}
inline bool ReadU32(std::span<const uint8_t> bytes, size_t& pos, uint32_t& v) {
    if (pos > bytes.size() || bytes.size() - pos < 4)
        return false;
    v = 0;
    for (int i = 0; i < 4; ++i)
        v |= uint32_t(bytes[pos++]) << (i * 8);
    return true;
}
inline bool ReadU64(std::span<const uint8_t> bytes, size_t& pos, uint64_t& v) {
    if (pos > bytes.size() || bytes.size() - pos < 8)
        return false;
    v = 0;
    for (int i = 0; i < 8; ++i)
        v |= uint64_t(bytes[pos++]) << (i * 8);
    return true;
}
inline bool EncodeTile(const engine::worldpartition::WorldTileKey& tile,
                       std::span<const CookedMapPlacement> records,
                       std::vector<uint8_t>& out) {
    if (!tile.world || records.size() > kMaxPlacementsPerTile)
        return false;
    std::vector<uint8_t> result;
    result.reserve(kTileHeaderBytes + kPlacementBytes * records.size());
    WriteU32(result, kTileMagic);
    WriteU32(result, 2); // world-scoped codec version
    WriteU64(result, tile.world);
    WriteU64(result, std::bit_cast<uint64_t>(tile.cell.x));
    WriteU64(result, std::bit_cast<uint64_t>(tile.cell.z));
    WriteU32(result, uint32_t(records.size()));
    uint64_t previousId = 0;
    for (const auto& record : records) {
        if (!record.mapNodeId || record.mapNodeId <= previousId || !record.sceneAssetId)
            return false;
        previousId = record.mapNodeId;
        WriteU64(result, record.mapNodeId);
        WriteU64(result, record.sceneAssetId);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                const double v = record.worldTransform[c][r];
                if (!std::isfinite(v))
                    return false;
                WriteU64(result, std::bit_cast<uint64_t>(v));
            }
    }
    out = std::move(result);
    return true;
}
inline bool DecodeTile(std::span<const uint8_t> bytes,
                       engine::worldpartition::WorldTileKey expected,
                       std::vector<CookedMapPlacement>& out) {
    if (bytes.size() < kTileHeaderBytes)
        return false;
    size_t pos = 0;
    uint32_t magic = 0, version = 0, count = 0;
    uint64_t world = 0, x = 0, z = 0;
    if (!ReadU32(bytes, pos, magic) || magic != kTileMagic || !ReadU32(bytes, pos, version) ||
        version != 2 || !ReadU64(bytes, pos, world) || !ReadU64(bytes, pos, x) ||
        !ReadU64(bytes, pos, z) || !ReadU32(bytes, pos, count) || count > kMaxPlacementsPerTile)
        return false;
    if (!world || world != expected.world || std::bit_cast<int64_t>(x) != expected.cell.x ||
        std::bit_cast<int64_t>(z) != expected.cell.z)
        return false;
    if (size_t(count) > (std::numeric_limits<size_t>::max() - kTileHeaderBytes) / kPlacementBytes ||
        bytes.size() != kTileHeaderBytes + size_t(count) * kPlacementBytes)
        return false;
    std::vector<CookedMapPlacement> records;
    records.reserve(count);
    uint64_t previousId = 0;
    for (uint32_t i = 0; i < count; ++i) {
        CookedMapPlacement rec{};
        if (!ReadU64(bytes, pos, rec.mapNodeId) || !ReadU64(bytes, pos, rec.sceneAssetId) ||
            !rec.mapNodeId || rec.mapNodeId <= previousId || !rec.sceneAssetId)
            return false;
        previousId = rec.mapNodeId;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                uint64_t bits = 0;
                if (!ReadU64(bytes, pos, bits))
                    return false;
                const double v = std::bit_cast<double>(bits);
                if (!std::isfinite(v))
                    return false;
                rec.worldTransform[c][r] = v;
            }
        records.push_back(rec);
    }
    out = std::move(records);
    return true;
}

} // namespace tour::mapcook
