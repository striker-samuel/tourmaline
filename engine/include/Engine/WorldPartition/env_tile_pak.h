// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Env tile pages in SMP8: independently indexed, CRC-checked pak blobs.
// The manifest stores immutable exact authored OBBs and looks once;
// individual 64m cell pages contain only sorted record indices.
// This is a cook/reader seam, not a dependency on renderer Vulkan state.
#include <Engine/Asset/pak.h>
#include <Engine/WorldPartition/WorldEnvTiles.h>
#include <Engine/WorldPartition/WorldPartition.h>
#include <bit>
#include <cmath>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tour::envcook {
using engine::worldpartition::WorldEnvTiles;
using engine::worldpartition::WorldId;
using engine::worldpartition::WorldSubcellKey;
using engine::worldpartition::EnvTileRecord;
using smvk::envzones::EnvZoneGpu;

inline constexpr uint32_t kManifestMagic = 0x314D5645u; // EVM1
inline constexpr uint32_t kPageMagic = 0x31505645u; // EVP1
inline constexpr uint32_t kFormat = 1u;
inline constexpr size_t kManifestHeaderBytes = 4 + 4 + 8 + 4 + 4 + 4 + 4 + 4;
inline constexpr size_t kRecordBytes = 48 + 144;
inline constexpr size_t kPageHeaderBytes = 4 + 4 + 8 + 8 + 8 + 4;

inline void Write32(std::vector<uint8_t>& b, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(uint8_t(value >> (i * 8)));
}
inline void Write64(std::vector<uint8_t>& b, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) b.push_back(uint8_t(value >> (i * 8)));
}
inline bool Read32(std::span<const uint8_t> b, size_t& p, uint32_t& value) {
    if (p > b.size() || b.size() - p < 4) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(b[p++]) << (8*i);
    return true;
}
inline bool Read64(std::span<const uint8_t> b, size_t& p, uint64_t& value) {
    if (p > b.size() || b.size() - p < 8) return false;
    value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(b[p++]) << (8*i);
    return true;
}
inline void WriteFloat(std::vector<uint8_t>& b, float f) {
    Write32(b, std::bit_cast<uint32_t>(f));
}
inline bool ReadFloat(std::span<const uint8_t> b, size_t& p, float& f) {
    uint32_t value = 0;
    if (!Read32(b, p, value)) return false;
    f = std::bit_cast<float>(value);
    return true;
}
inline void WriteVec(std::vector<uint8_t>& b, const glm::vec4& v) {
    for (int i = 0; i < 4; ++i) WriteFloat(b, v[i]);
}
inline bool ReadVec(std::span<const uint8_t> b, size_t& p, glm::vec4& v) {
    for (int i = 0; i < 4; ++i)
        if (!ReadFloat(b, p, v[i])) return false;
    return true;
}
inline void WriteUVec(std::vector<uint8_t>& b, const glm::uvec4& v) {
    for (int i = 0; i < 4; ++i) Write32(b, v[i]);
}
inline bool ReadUVec(std::span<const uint8_t> b, size_t& p, glm::uvec4& v) {
    for (int i = 0; i < 4; ++i)
        if (!Read32(b, p, v[i])) return false;
    return true;
}

// Reserved scene-scoped ID for the baked env content in a scene pak.
// Scene name is the same identity used by SerializeClusters/ListPakScenes.
inline WorldId SceneWorldId(std::string_view sceneName) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : sceneName) {
        hash ^= uint64_t(c);
        hash *= 1099511628211ull;
    }
    return hash | (uint64_t(1) << 63);
}
inline std::string ManifestPath(WorldId world) {
    return "worlds/" + std::to_string(world) + "/env/manifest.evm";
}
inline std::string PagePath(WorldId world, WorldSubcellKey tile) {
    return "worlds/" + std::to_string(world) + "/env/" +
        std::to_string(tile.x) + "_" + std::to_string(tile.z) + ".evp";
}

struct Manifest {
    WorldId world = 0;
    glm::vec2 gridMin{};
    uint32_t dimX = 0, dimZ = 0;
    std::vector<EnvTileRecord> records;
    std::vector<EnvZoneGpu> shapes;
    std::vector<uint8_t> pagePresent; // one byte per grid cell, 0 or 1
};

inline bool EncodeManifest(const Manifest& m, std::vector<uint8_t>& out) {
    const size_t n = m.records.size();
    if (!m.world || !m.dimX || !m.dimZ ||
        m.dimX > engine::worldpartition::kEnvTileMaxDim ||
        m.dimZ > engine::worldpartition::kEnvTileMaxDim ||
        !n || n > engine::worldpartition::kEnvTileMaxRecords ||
        m.shapes.size() != n ||
        m.pagePresent.size() != size_t(m.dimX) * m.dimZ ||
        !std::isfinite(m.gridMin.x) ||
        !std::isfinite(m.gridMin.y)) return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(kManifestHeaderBytes + n*kRecordBytes + m.pagePresent.size());
    Write32(bytes, kManifestMagic);
    Write32(bytes, kFormat);
    Write64(bytes, m.world);
    WriteFloat(bytes, m.gridMin.x);
    WriteFloat(bytes, m.gridMin.y);
    Write32(bytes, m.dimX);
    Write32(bytes, m.dimZ);
    Write32(bytes, uint32_t(n));
    for (size_t i = 0; i < n; ++i) {
        WriteVec(bytes, m.records[i].sky);
        WriteVec(bytes, m.records[i].fog);
        WriteVec(bytes, m.records[i].vol);
        const auto& s = m.shapes[i];
        for (const auto& row : s.worldToLocal) WriteVec(bytes, row);
        WriteUVec(bytes, s.halfAndMask);
        WriteVec(bytes, s.shape);
        WriteVec(bytes, s.sky);
        WriteVec(bytes, s.fog);
        WriteVec(bytes, s.vol);
    }
    for (uint8_t present : m.pagePresent) {
        if (present > 1) return false;
        bytes.push_back(present);
    }
    out = std::move(bytes);
    return true;
}

inline bool DecodeManifest(std::span<const uint8_t> b, WorldId expected, Manifest& out) {
    size_t p = 0;
    uint32_t magic = 0, version = 0, n = 0, dx = 0, dz = 0;
    uint64_t world = 0;
    glm::vec2 origin{};
    if (b.size() < kManifestHeaderBytes ||
        !Read32(b,p,magic) || magic != kManifestMagic ||
        !Read32(b,p,version) || version != kFormat ||
        !Read64(b,p,world) || !world || world != expected ||
        !ReadFloat(b,p,origin.x) || !ReadFloat(b,p,origin.y) ||
        !Read32(b,p,dx) || !Read32(b,p,dz) ||
        !Read32(b,p,n) || !n || n > engine::worldpartition::kEnvTileMaxRecords ||
        !dx || !dz || dx > engine::worldpartition::kEnvTileMaxDim ||
        dz > engine::worldpartition::kEnvTileMaxDim ||
        !std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        b.size() != kManifestHeaderBytes + size_t(n)*kRecordBytes +
                    size_t(dx)*dz) return false;
    Manifest next{};
    next.world = world;
    next.gridMin = origin;
    next.dimX = dx;
    next.dimZ = dz;
    next.records.resize(n);
    next.shapes.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto& look = next.records[i];
        auto& shape = next.shapes[i];
        if (!ReadVec(b,p,look.sky) || !ReadVec(b,p,look.fog) || !ReadVec(b,p,look.vol))
            return false;
        for (auto& row : shape.worldToLocal)
            if (!ReadVec(b,p,row)) return false;
        if (!ReadUVec(b,p,shape.halfAndMask) ||
            !ReadVec(b,p,shape.shape) || !ReadVec(b,p,shape.sky) ||
            !ReadVec(b,p,shape.fog) || !ReadVec(b,p,shape.vol)) return false;
    }
    next.pagePresent.resize(size_t(dx)*dz);
    for (uint8_t& present : next.pagePresent) {
        present = b[p++];
        if (present > 1) return false;
    }
    out = std::move(next);
    return true;
}

inline bool EncodePage(WorldId world, WorldSubcellKey tile,
                       std::span<const uint32_t> ids, std::vector<uint8_t>& out) {
    if (!world || ids.size() > engine::worldpartition::kEnvTileMaxRecords) return false;
    for (uint32_t id : ids)
        if (id >= engine::worldpartition::kEnvTileMaxRecords) return false;
    std::vector<uint8_t> b;
    b.reserve(kPageHeaderBytes + ids.size()*4);
    Write32(b, kPageMagic); Write32(b, kFormat);
    Write64(b, world);
    Write64(b, std::bit_cast<uint64_t>(tile.x));
    Write64(b, std::bit_cast<uint64_t>(tile.z));
    Write32(b, uint32_t(ids.size()));
    for (uint32_t id : ids) Write32(b, id);
    out = std::move(b);
    return true;
}
inline bool DecodePage(std::span<const uint8_t> b, WorldId expectedWorld,
                       WorldSubcellKey expectedTile, uint32_t recordCount,
                       std::vector<uint32_t>& out) {
    size_t p = 0;
    uint32_t magic=0, version=0, n=0;
    uint64_t world=0, x=0, z=0;
    if (b.size() < kPageHeaderBytes || !expectedWorld || !recordCount ||
        !Read32(b,p,magic) || magic != kPageMagic ||
        !Read32(b,p,version) || version != kFormat ||
        !Read64(b,p,world) || world != expectedWorld ||
        !Read64(b,p,x) || std::bit_cast<int64_t>(x) != expectedTile.x ||
        !Read64(b,p,z) || std::bit_cast<int64_t>(z) != expectedTile.z ||
        !Read32(b,p,n) || n > engine::worldpartition::kEnvTileMaxRecords ||
        b.size() != kPageHeaderBytes + size_t(n)*4) return false;
    std::vector<uint32_t> ids;
    ids.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = 0;
        if (!Read32(b,p,id) || id >= recordCount) return false;
        ids.push_back(id);
    }
    out = std::move(ids);
    return true;
}

inline bool AddEnvTiles(smvk::PakWriter& writer, WorldId world, const WorldEnvTiles& tiles) {
    if (!world || !tiles.Valid()) return false;
    Manifest manifest{};
    manifest.world = world;
    manifest.gridMin = tiles.GridMin();
    manifest.dimX = tiles.DimX();
    manifest.dimZ = tiles.DimZ();
    manifest.records.assign(tiles.Records(), tiles.Records() + tiles.RecordCount());
    manifest.shapes.assign(tiles.Shapes(), tiles.Shapes() + tiles.RecordCount());
    manifest.pagePresent.assign(size_t(tiles.DimX())*tiles.DimZ(), uint8_t(0));
    for (size_t i = 0; i < manifest.pagePresent.size(); ++i) {
        const auto& c = tiles.Cells()[i];
        manifest.pagePresent[i] = uint8_t(c.candidateCount &&
            !(c.candidateCount == 1 && tiles.CandidateIndices()[c.firstCandidate] == 0));
    }
    std::vector<uint8_t> blob;
    if (!EncodeManifest(manifest, blob) ||
        !writer.AddBlob(smvk::PakAssetKind::EnvTile, ManifestPath(world), std::move(blob)))
        return false;
    for (uint32_t z = 0; z < tiles.DimZ(); ++z)
        for (uint32_t x = 0; x < tiles.DimX(); ++x) {
            const auto& c = tiles.Cells()[size_t(z)*tiles.DimX() + x];
            // Base-only pages need no pak payload; missing page means base.
            if (!c.candidateCount ||
                (c.candidateCount == 1 && tiles.CandidateIndices()[c.firstCandidate] == 0))
                continue;
            const glm::vec2 origin = tiles.GridMin();
            const WorldSubcellKey key = engine::worldpartition::GetWorldSubcell(
                glm::dvec3(double(origin.x)+(double(x)+0.5)*engine::worldpartition::kEnvTileSize,
                           0.0, double(origin.y)+(double(z)+0.5)*engine::worldpartition::kEnvTileSize));
            const auto* first = tiles.CandidateIndices() + c.firstCandidate;
            blob.clear();
            if (!EncodePage(world, key, std::span<const uint32_t>(first, c.candidateCount), blob) ||
                !writer.AddBlob(smvk::PakAssetKind::EnvTile, PagePath(world, key), std::move(blob)))
                return false;
        }
    return true;
}
inline bool ReadEnvManifest(const smvk::PakReader& reader, WorldId world, Manifest& out) {
    const auto* entry = reader.Find(smvk::PakAssetKind::EnvTile, ManifestPath(world));
    if (!entry) return false;
    try { return DecodeManifest(reader.Payload(*entry), world, out); }
    catch (...) { return false; }
}
// Missing page is an intentionally omitted base-only tile. Corrupted
// pages still fail explicitly (never silently turn into base).
inline bool ReadEnvPage(const smvk::PakReader& reader, WorldId world,
                        WorldSubcellKey tile, uint32_t recordCount,
                        bool expectedPage, std::vector<uint32_t>& out) {
    const auto* entry = reader.Find(smvk::PakAssetKind::EnvTile, PagePath(world,tile));
    if (!entry) {
        if (expectedPage) return false; // missing authored data is corruption
        out = {0};
        return true;
    }
    if (!expectedPage) return false; // unexpected page: manifest mismatch
    try {
        const std::vector<uint8_t> bytes = reader.PayloadCopyUncached(*entry);
        return DecodePage(bytes, world, tile, recordCount, out);
    }
    catch (...) { return false; }
}
} // namespace tour::envcook
