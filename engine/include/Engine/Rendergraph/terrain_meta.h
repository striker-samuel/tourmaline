// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Tiled-terrain metadata (cook-boundary, the engine TEXT convention — no
// JSON). terrain.meta is a flat key=value file the cooker validates strictly
// and the runtime never sees (the cooked TerrainMeta blob is the runtime
// form). Only this header + the cooker know the text shape; everything past
// the cooker speaks TerrainMeta.
//
// Schema (all required, one key=value per line, '#' comments):
//   baseY range worldSize hTileSize gBlock : number
//   originX originZ                        : number
//   hRes                                   : 1024
//   packing                                : rg16

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Engine/Core/Limits.h>

namespace smvk::terrain {

struct TerrainMeta {
    float baseY = -40.0f;
    float range = 420.0f;
    float worldSize = 4096.0f;
    float originX = -2048.0f;
    float originZ = -2048.0f;
    float hTileSize = 1024.0f;
    uint32_t hRes = 1024;
    float gBlock = 256.0f;
    // packing is always "rg16" in v1 (R=MSB, G=LSB); kept as a bool for
    // the fail-closed cooker check rather than a stringly runtime knob.
    bool packingRg16 = true;
};

// World-size-driven grid dimensions (the old fixed 4x4 H / 16x16 S grid is
// gone): H-tiles cover worldSize/hTileSize, G-blocks worldSize/gBlock.
[[nodiscard]] inline uint32_t TilesPerAxis(const TerrainMeta& m) noexcept {
    return static_cast<uint32_t>(
        std::lround(static_cast<double>(m.worldSize) / static_cast<double>(m.hTileSize)));
}
[[nodiscard]] inline uint32_t BlocksPerAxis(const TerrainMeta& m) noexcept {
    return static_cast<uint32_t>(
        std::lround(static_cast<double>(m.worldSize) / static_cast<double>(m.gBlock)));
}

// ---- tile file/blob naming (source PNGs and cooked blob paths share it) --
inline std::string HTileName(uint32_t tx, uint32_t tz) {
    char b[32];
    snprintf(b, sizeof(b), "h_%02u_%02u", tx, tz);
    return std::string(b);
}
inline std::string STileName(uint32_t tx, uint32_t tz) {
    char b[32];
    snprintf(b, sizeof(b), "m_%02u_%02u", tx, tz);
    return std::string(b);
}
inline std::string HTileBlobPath(uint32_t tx, uint32_t tz) {
    return "terrain/" + HTileName(tx, tz);
}
inline std::string STileBlobPath(uint32_t tx, uint32_t tz) {
    return "terrain/" + STileName(tx, tz);
}
inline constexpr const char* kTerrainMetaBlobPath = "terrain/meta";

// Parses the flat terrain meta (the engine TEXT convention: key=value lines
// like .material/.meta, '#' comments) into meta. Strict: unknown keys fail
// (typos must not silently cook), missing keys fail, hRes must be 1024,
// packing must be "rg16".
[[nodiscard]] inline bool ParseTerrainMetaText(std::string_view text,
                                               TerrainMeta& meta,
                                               std::string& err) {
    bool seenBaseY = false, seenRange = false, seenWorld = false, seenOx = false, seenOz = false,
         seenHSize = false, seenHRes = false, seenGBlock = false, seenPacking = false;
    double worldSize = 0, hTileSize = 0, gBlock = 0, baseY = 0, range = 0, hRes = 0;
    double ox = 0, oz = 0;

    size_t pos = 0;
    size_t lineNo = 0;
    auto parseNum = [](std::string_view val, double& out) {
        std::string s(val);
        char* end = nullptr;
        out = std::strtod(s.c_str(), &end);
        return end != s.c_str() && *end == '\0';
    };
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
        pos = (nl == std::string_view::npos ? text.size() : nl + 1);
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        // Trim the whole line.
        size_t b = 0, e = line.size();
        while (b < e && (line[b] == ' ' || line[b] == '\t'))
            ++b;
        while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t'))
            --e;
        line = line.substr(b, e - b);
        if (line.empty() || line[0] == '#')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            err = "terrain meta: expected 'key=value' on line " + std::to_string(lineNo);
            return false;
        }
        std::string_view key = line.substr(0, eq);
        std::string_view val = line.substr(eq + 1);
        auto trim = [](std::string_view v) {
            size_t b = 0, e = v.size();
            while (b < e && (v[b] == ' ' || v[b] == '\t'))
                ++b;
            while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t'))
                --e;
            return v.substr(b, e - b);
        };
        key = trim(key);
        val = trim(val);

        if (key == "baseY") {
            if (!parseNum(val, baseY)) {
                err = "terrain meta: bad baseY";
                return false;
            }
            seenBaseY = true;
        } else if (key == "range") {
            if (!parseNum(val, range)) {
                err = "terrain meta: bad range";
                return false;
            }
            seenRange = true;
        } else if (key == "worldSize") {
            if (!parseNum(val, worldSize)) {
                err = "terrain meta: bad worldSize";
                return false;
            }
            seenWorld = true;
        } else if (key == "originX") {
            if (!parseNum(val, ox)) {
                err = "terrain meta: bad originX";
                return false;
            }
            seenOx = true;
        } else if (key == "originZ") {
            if (!parseNum(val, oz)) {
                err = "terrain meta: bad originZ";
                return false;
            }
            seenOz = true;
        } else if (key == "hTileSize") {
            if (!parseNum(val, hTileSize)) {
                err = "terrain meta: bad hTileSize";
                return false;
            }
            seenHSize = true;
        } else if (key == "hRes") {
            if (!parseNum(val, hRes) || hRes != 1024.0) {
                err = "terrain meta: hRes must be 1024";
                return false;
            }
            seenHRes = true;
        } else if (key == "gBlock") {
            if (!parseNum(val, gBlock)) {
                err = "terrain meta: bad gBlock";
                return false;
            }
            seenGBlock = true;
        } else if (key == "packing") {
            if (val != "rg16") {
                err = "terrain meta: packing must be rg16";
                return false;
            }
            seenPacking = true;
        } else {
            err = "terrain meta: unknown key '" + std::string(key) + "'";
            return false;
        }
    }
    if (!seenBaseY || !seenRange || !seenWorld || !seenOx || !seenOz || !seenHSize || !seenHRes ||
        !seenGBlock || !seenPacking) {
        err = "terrain meta: missing required key";
        return false;
    }
    if (!std::isfinite(baseY) || !std::isfinite(range) || !std::isfinite(ox) ||
        !std::isfinite(oz) || !(range > 0.0) || !(worldSize > 0.0) || !(hTileSize > 0.0) ||
        !(gBlock > 0.0)) {
        err = "terrain meta: requires finite heights, positive range and positive "
              "world/hTile/gBlock sizes";
        return false;
    }
    meta.baseY = static_cast<float>(baseY);
    meta.range = static_cast<float>(range);
    meta.worldSize = static_cast<float>(worldSize);
    meta.originX = static_cast<float>(ox);
    meta.originZ = static_cast<float>(oz);
    meta.hTileSize = static_cast<float>(hTileSize);
    meta.hRes = 1024;
    meta.gBlock = static_cast<float>(gBlock);
    meta.packingRg16 = true;
    // Grid layout: worldSize must divide into whole H-tiles (hTileSize) and
    // G-blocks (gBlock); TilesPerAxis/BlocksPerAxis are the dimensions the
    // cooker writes and the runtime derives. The world is bounded by the
    // engine limit (40 km per axis) and the block-grid safety bound.
    const uint32_t nH = TilesPerAxis(meta);
    const uint32_t nS = BlocksPerAxis(meta);
    if (nH < 1 || nH > engine::limits::MaxTerrainBlocksPerAxis || nS < 1 ||
        nS > engine::limits::MaxTerrainBlocksPerAxis ||
        meta.worldSize > engine::limits::MaxTerrainWorldSizeMeters ||
        std::abs(static_cast<double>(nH) * meta.hTileSize - meta.worldSize) > 0.5 ||
        std::abs(static_cast<double>(nS) * meta.gBlock - meta.worldSize) > 0.5) {
        err = "terrain meta: worldSize must divide into whole H-tiles and G-blocks "
              "within the engine limit";
        return false;
    }
    return true;
}

// ---- cooked runtime blob (terrain/meta): the ONLY form the runtime reads.
// Little-endian, fail-closed (magic + derived size):
//   u32 magic "TRM2" | u32 hRes | f32 baseY range worldSize originX originZ
//   hTileSize gBlock | (tilesPerAxis^2) x { f32 minY, f32 maxY } (row-major
//   tz*tilesPerAxis+tx), tilesPerAxis = worldSize/hTileSize.
inline constexpr uint32_t kTerrainMetaMagic = 0x324D5254u; // "TRM2" LE
[[nodiscard]] inline size_t TerrainMetaBlobSize(uint32_t tilesPerAxis) {
    return 4 + 4 + 7 * 4 + size_t(tilesPerAxis) * tilesPerAxis * 8;
}

inline void SerializeTerrainMeta(const TerrainMeta& meta,
                                 const float* tileMinMax, // tilesPerAxis^2 * 2
                                 std::vector<uint8_t>& out) {
    const uint32_t n = TilesPerAxis(meta);
    out.resize(TerrainMetaBlobSize(n));
    auto put32 = [&](size_t o, uint32_t v) { memcpy(out.data() + o, &v, 4); };
    auto putF = [&](size_t o, float v) { memcpy(out.data() + o, &v, 4); };
    put32(0, kTerrainMetaMagic);
    put32(4, meta.hRes);
    putF(8, meta.baseY);
    putF(12, meta.range);
    putF(16, meta.worldSize);
    putF(20, meta.originX);
    putF(24, meta.originZ);
    putF(28, meta.hTileSize);
    putF(32, meta.gBlock);
    for (uint32_t i = 0; i < n * n; ++i) {
        putF(36 + size_t(i) * 8, tileMinMax[i * 2]);
        putF(40 + size_t(i) * 8, tileMinMax[i * 2 + 1]);
    }
}

[[nodiscard]] inline bool ParseTerrainMetaBlob(std::span<const uint8_t> blob,
                                               TerrainMeta& meta,
                                               std::vector<float>& tileMinMax) {
    if (blob.size() < 4 + 4 + 7 * 4)
        return false;
    uint32_t magic = 0, hRes = 0;
    memcpy(&magic, blob.data() + 0, 4);
    memcpy(&hRes, blob.data() + 4, 4);
    if (magic != kTerrainMetaMagic || hRes != 1024)
        return false;
    auto getF = [&](size_t o) {
        float v = 0;
        memcpy(&v, blob.data() + o, 4);
        return v;
    };
    meta.baseY = getF(8);
    meta.range = getF(12);
    meta.worldSize = getF(16);
    meta.originX = getF(20);
    meta.originZ = getF(24);
    meta.hTileSize = getF(28);
    meta.gBlock = getF(32);
    meta.hRes = 1024;
    meta.packingRg16 = true;
    if (!std::isfinite(meta.baseY) || !std::isfinite(meta.range) || !std::isfinite(meta.originX) ||
        !std::isfinite(meta.originZ) || !(meta.range > 0.0f) || !(meta.worldSize > 0.0f) ||
        !(meta.hTileSize > 0.0f) || !(meta.gBlock > 0.0f))
        return false;
    const uint32_t n = TilesPerAxis(meta);
    if (n < 1 || n > engine::limits::MaxTerrainBlocksPerAxis ||
        meta.worldSize > engine::limits::MaxTerrainWorldSizeMeters ||
        blob.size() != TerrainMetaBlobSize(n))
        return false;
    if (std::abs(static_cast<double>(n) * meta.hTileSize - meta.worldSize) > 0.5)
        return false;
    tileMinMax.resize(size_t(n) * n * 2);
    for (uint32_t i = 0; i < n * n; ++i) {
        tileMinMax[i * 2] = getF(36 + size_t(i) * 8);
        tileMinMax[i * 2 + 1] = getF(40 + size_t(i) * 8);
        if (!std::isfinite(tileMinMax[i * 2]) || !std::isfinite(tileMinMax[i * 2 + 1]) ||
            !(tileMinMax[i * 2 + 1] >= tileMinMax[i * 2]))
            return false;
    }
    return true;
}

} // namespace smvk::terrain