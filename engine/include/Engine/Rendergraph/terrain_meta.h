// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Tiled-terrain metadata (cook-boundary schema, no JSON dependency).
//
// terrain.json is a FLAT object the cooker validates strictly and the
// runtime never sees (the cooked TerrainMeta blob is the runtime form).
// Only this header + the cooker know the .json shape; everything past
// the cooker speaks TerrainMeta. Replacing .json later touches only the
// Parse function below.
//
// Schema (all required):
//   baseY, range, worldSize, hTileSize, gBlock : number
//   origin : [x, z]                             : number pair
//   hRes                                        : 1024
//   packing : "rg16"

#include <cmath>
#include <cstdint>
#include <cstdio>
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

// ---- minimal flat-JSON parser (strict, cook-side) -------------------------
namespace detail {
inline void SkipWs(std::string_view s, size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n'))
        ++p;
}
// Parses one JSON number at p (no hex/inf/nan); advances p past it.
inline bool ParseNum(std::string_view s, size_t& p, double& out) {
    SkipWs(s, p);
    const size_t start = p;
    if (p < s.size() && (s[p] == '-' || s[p] == '+'))
        ++p;
    bool any = false;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        ++p;
        any = true;
    }
    if (p < s.size() && s[p] == '.') {
        ++p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
            ++p;
            any = true;
        }
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
        ++p;
        if (p < s.size() && (s[p] == '-' || s[p] == '+'))
            ++p;
        bool eany = false;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
            ++p;
            eany = true;
        }
        if (!eany)
            return false;
    }
    if (!any || p == start)
        return false;
    try {
        out = std::stod(std::string(s.substr(start, p - start)));
    } catch (...) {
        return false;
    }
    return true;
}
inline bool ParseLit(std::string_view s, size_t& p, const char* lit) {
    SkipWs(s, p);
    for (const char* q = lit; *q; ++q, ++p) {
        if (p >= s.size() || s[p] != *q)
            return false;
    }
    return true;
}
inline bool ParseString(std::string_view s, size_t& p, std::string& out) {
    SkipWs(s, p);
    if (p >= s.size() || s[p] != '"')
        return false;
    ++p;
    out.clear();
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            ++p;
            out.push_back(s[p++]);
        } else {
            out.push_back(s[p++]);
        }
    }
    if (p >= s.size())
        return false;
    ++p;
    return true;
}
} // namespace detail

// Parses the flat terrain.json schema into meta. Strict: unknown keys fail
// (typos must not silently cook), missing keys fail, hRes must be 1024,
// packing must be "rg16".
[[nodiscard]] inline bool ParseTerrainJson(std::string_view text,
                                           TerrainMeta& meta,
                                           std::string& err) {
    using namespace detail;
    size_t p = 0;
    if (!ParseLit(text, p, "{")) {
        err = "terrain.json: expected '{'";
        return false;
    }
    bool seenBaseY = false, seenRange = false, seenWorld = false, seenOx = false, seenHSize = false,
         seenHRes = false, seenGBlock = false, seenPacking = false;
    double worldSize = 0, hTileSize = 0, gBlock = 0, baseY = 0, range = 0, hRes = 0;
    double ox = 0, oz = 0;
    bool first = true;
    while (true) {
        SkipWs(text, p);
        if (p < text.size() && text[p] == '}') {
            ++p;
            break;
        }
        if (!first) {
            if (!ParseLit(text, p, ",")) {
                err = "terrain.json: expected ',' between keys";
                return false;
            }
        }
        first = false;
        std::string key;
        if (!ParseString(text, p, key)) {
            err = "terrain.json: expected string key";
            return false;
        }
        if (!ParseLit(text, p, ":")) {
            err = "terrain.json: expected ':' after key '" + key + "'";
            return false;
        }
        double num = 0;
        if (key == "origin") {
            if (!ParseLit(text, p, "[")) {
                err = "terrain.json: origin must be [x, z]";
                return false;
            }
            if (!ParseNum(text, p, ox) || !ParseLit(text, p, ",") || !ParseNum(text, p, oz) ||
                !ParseLit(text, p, "]")) {
                err = "terrain.json: origin must be [x, z] numbers";
                return false;
            }
            seenOx = true;
        } else if (key == "packing") {
            std::string v;
            if (!ParseString(text, p, v) || v != "rg16") {
                err = "terrain.json: packing must be \"rg16\"";
                return false;
            }
            seenPacking = true;
        } else if (key == "baseY") {
            if (!ParseNum(text, p, baseY)) {
                err = "terrain.json: baseY must be a number";
                return false;
            }
            seenBaseY = true;
        } else if (key == "range") {
            if (!ParseNum(text, p, range)) {
                err = "terrain.json: range must be a number";
                return false;
            }
            seenRange = true;
        } else if (key == "worldSize") {
            if (!ParseNum(text, p, worldSize)) {
                err = "terrain.json: worldSize must be a number";
                return false;
            }
            seenWorld = true;
        } else if (key == "hTileSize") {
            if (!ParseNum(text, p, hTileSize)) {
                err = "terrain.json: hTileSize must be a number";
                return false;
            }
            seenHSize = true;
        } else if (key == "hRes") {
            if (!ParseNum(text, p, hRes) || hRes != 1024.0) {
                err = "terrain.json: hRes must be 1024";
                return false;
            }
            seenHRes = true;
        } else if (key == "gBlock") {
            if (!ParseNum(text, p, gBlock)) {
                err = "terrain.json: gBlock must be a number";
                return false;
            }
            seenGBlock = true;
        } else {
            err = "terrain.json: unknown key '" + key + "'";
            return false;
        }
        (void)num;
    }
    SkipWs(text, p);
    if (p != text.size()) {
        err = "terrain.json: trailing bytes after '}'";
        return false;
    }
    if (!seenBaseY || !seenRange || !seenWorld || !seenOx || !seenHSize || !seenHRes ||
        !seenGBlock || !seenPacking) {
        err = "terrain.json: missing required key";
        return false;
    }
    if (!std::isfinite(baseY) || !std::isfinite(range) || !std::isfinite(ox) ||
        !std::isfinite(oz) || !(range > 0.0) || !(worldSize > 0.0) || !(hTileSize > 0.0) ||
        !(gBlock > 0.0)) {
        err = "terrain.json: requires finite heights, positive range and positive "
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
        err = "terrain.json: worldSize must divide into whole H-tiles and G-blocks "
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