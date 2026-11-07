// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldEnvTiles: exact static OBB geometry accelerated by a 64m spatial
// index. A tile contains references to all overlapping candidates, not a
// sampled winner. The shading position determines coverage and falloff.
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <Engine/Core/Limits.h>
#include <Engine/Rendergraph/envzones.h>
#include <Engine/Scene/scene.h>

namespace engine::worldpartition {

// The env-tile record: one zone's blendable LOOK (the old EnvZoneGpu's
// sky/fog/vol payload — the OBB fields are gone, the tile carries the
// shape). HLSL mirror: EnvTileRecord (common.hlsli), 48 B.
struct EnvTileRecord {
    glm::vec4 sky{}; // rgb skyTint, a ambientI
    glm::vec4 fog{}; // rgb fogColor, a fogDensity
    glm::vec4 vol{}; // x volumetrics ratio, y volumetricDensity, zw reserved
};
static_assert(sizeof(EnvTileRecord) == 48, "EnvTileRecord: HLSL mirror");

// The 32-byte tile retains the existing raw BDA addressing stride.
struct EnvTileCell {
    uint32_t firstCandidate = 0;
    uint32_t candidateCount = 0;
    uint32_t reserved[6] = {};
};
static_assert(sizeof(EnvTileCell) == 32, "EnvTileCell HLSL ABI");

inline constexpr uint32_t kEnvTileMaxRecords = 64; // [0]=base + 63 zones
inline constexpr uint32_t kEnvTileMaxDim = engine::limits::MaxWorldEnvTilesPerAxis;
inline constexpr float kEnvTileSize = float(engine::limits::WorldSubcellSizeMeters);

// The grid extent for a bake: the tile-aligned XZ window + the vertical
// search range. MESH-ONLY scene bounds miss volumes placed outside the
// geometry (a volumes-only scene fell to the fixed fallback window; a
// zone hanging past the models fell OUTSIDE the grid and its look
// silently reverted to the base record outside — the review's finding),
// so the extent UNIONS the env-volume OBB bounds in. INFINITE faces /
// INF column lengths extend the WORLD axes the axis projects on to the
// grid's design edge (16 km): an infinite zone applies wherever the
// grid can reach; beyond the clamp the base record wins (the documented
// world edge). The VERTICAL range never extends for infinite axes — an
// infinite-Y zone wins at every ladder height, and stretching the
// ladder would tank the whole scene's vertical resolution.
struct EnvGridExtent {
    glm::vec2 gridMin{0.0f};
    uint32_t dimX = 0, dimZ = 0;
    float yMin = 0.0f, yMax = 0.0f;
};
// The seed bounds (the caller's ::tour::Aabb flattened — no Utils/bvh.h
// dependency in this header).
struct EnvGridBounds {
    glm::vec3 mn{};
    glm::vec3 mx{};
};
EnvGridExtent ComputeEnvGridExtent(const Scene& scene, const EnvGridBounds& sceneBounds);

// The bake. Rebuildable; the caller drives dirtiness (the renderer hashes
// the zone tables + transforms per frame — cheap — and rebuilds on
// change). CPU queries via At() are the shader mirror's reference (the
// tests pin both sides of the contract).
class WorldEnvTiles {
  public:
    // Grid extent: gridMin = the min corner (XZ, tile-aligned); dimX/dimZ
    // the tile counts (clamped to kEnvTileMaxDim). yMin/yMax frame the
    // vertical split search (the scene bounds). Returns false for invalid
    // extents; a scene with no env entities bakes the pure-base grid.
    bool Build(const Scene& scene,
               glm::vec2 gridMin,
               uint32_t dimX,
               uint32_t dimZ,
               float yMin,
               float yMax);

    // The shader mirror's reference (tests/editor/tooling).
    struct Lookup {
        uint32_t record = 0; // into Records(); 0 = the base
        float weight = 1.0f; // blendWeight*falloff at the slot sample
        bool valid = false;  // false = the plain env path (no tiles built)
    };
    Lookup At(glm::vec3 pos) const;

    // Load the immutable metadata cooked into an SMP8 environment manifest.
    // The provider supplies individual cell pages on demand; the CPU cell
    // address table initially contains only base fallback entries.
    bool LoadCooked(glm::vec2 gridMin, uint32_t dimX, uint32_t dimZ,
                    const EnvTileRecord* looks,
                    const smvk::envzones::EnvZoneGpu* shapes,
                    uint32_t count);

    // ---- pointer + count accessors (the FrameUniforms publication) ----
    const EnvTileRecord* Records() const {
        return records_.data();
    }
    uint32_t RecordCount() const {
        return recordCount_;
    }
    // Zone-authored volumetric fog present: ANY live record (the base
    // [0] included — it carries the scene env's own volumetrics pair)
    // with a positive ABSOLUTE volumetricDensity. Only the absolute
    // density can raise extinction from a zero pass density (the
    // volOn gates' zone leg + the shader's zone early-out bit — the
    // pair must stay in sync); the volumetrics RATIO alone is
    // multiplicative over the pass intensity the gates already require
    // positive, so a ratio-only record with no density anywhere
    // contributes nothing. The base record's density is SceneEnv's
    // default 0.015, so a published bake virtually always passes this
    // scan — by design: the scene env (the base block) IS a zone
    // author, and the env table owns the look.
    bool AnyVolumetricFog() const;
    const smvk::envzones::EnvZoneGpu* Shapes() const { return shapes_.data(); }
    const uint32_t* CandidateIndices() const { return candidateIndices_.data(); }
    uint32_t CandidateIndexCount() const { return uint32_t(candidateIndices_.size()); }
    // Static far field: important large/unbounded volumes in exact winner
    // order, evaluated only when a detailed page is not resident.
    const uint32_t* FarCandidates() const { return farCandidates_.data(); }
    uint32_t FarCandidateCount() const { return uint32_t(farCandidates_.size()); }
    bool SetCookedFarCandidates(const uint32_t* ids, uint32_t count) {
        if (count > kEnvTileMaxRecords - 1 || (count && !ids))
            return false;
        for (uint32_t i = 0; i < count; ++i)
            if (ids[i] == 0 || ids[i] >= recordCount_)
                return false;
        farCandidates_.assign(ids, ids + count);
        return true;
    }
    const EnvTileCell* Cells() const {
        return cells_.data();
    }
    uint32_t CellCount() const {
        return uint32_t(cells_.size());
    }
    glm::vec2 GridMin() const {
        return gridMin_;
    }
    uint32_t DimX() const {
        return dimX_;
    }
    uint32_t DimZ() const {
        return dimZ_;
    }
    bool Valid() const {
        return built_;
    }

  private:
    struct Candidate {
        smvk::envzones::EnvZoneGpu zone{}; // the sanitized OBB + look
        uint32_t record = 0;               // the assigned record index
        glm::vec2 xzMin{0.0f}, xzMax{0.0f}; // conservative falloff AABB
        double boundedVolume = 0.0;        // finite product; +inf for any unbounded face
        uint32_t nodeId = 0;
        float priority = 0.0f;
    };
    // The winner (record + weight) among the sorted candidates at a
    // position; record 0 (the base, an all-infinite candidate) at weight
    // 1 when nothing covers.
    Lookup Evaluate(const glm::vec3& p) const;

    std::vector<smvk::envzones::EnvZoneGpu> shapes_; // record-indexed OBBs
    std::vector<uint32_t> candidateIndices_; // ordered per-cell references
    std::vector<uint32_t> farCandidates_; // sorted far-tier candidate IDs
    std::vector<EnvTileRecord> records_; // [0] = the base
    uint32_t recordCount_ = 0;
    std::vector<EnvTileCell> cells_;    // dimX * dimZ
    std::vector<Candidate> candidates_; // sorted, includes the base at [0]
    glm::vec2 gridMin_{0.0f, 0.0f};
    uint32_t dimX_ = 0;
    uint32_t dimZ_ = 0;
    bool built_ = false;
};

} // namespace engine::worldpartition
