// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

#include <Engine/Rendergraph/envzones.h>
#include <Engine/WorldPartition/WorldEnvTiles.h>

namespace engine::worldpartition {

EnvGridExtent ComputeEnvGridExtent(const Scene& scene, const EnvGridBounds& sceneBounds) {
    // ---- The seed: the mesh scene bounds (degenerate/no bounds — a flat
    // or mesh-less scene — falls to the 1 km default window around the
    // origin; env volumes below can still grow it out). ----
    glm::vec3 mn = sceneBounds.mn, mx = sceneBounds.mx;
    const bool valid = mx.x >= mn.x && mx.y >= mn.y && mx.z >= mn.z && (mx.x > mn.x || mx.z > mn.z);
    if (!valid) {
        mn = glm::vec3(-512.0f, -16.0f, -512.0f);
        mx = glm::vec3(512.0f, 16.0f, 512.0f);
    }
    const auto unionPoint = [&](const glm::vec3& p) {
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    };

    // ---- The env-volume OBB bounds, unioned in. ----
    // The infinite-axis stand-in: half the max grid edge — the grid then
    // spans the full 16 km on that axis after the margin + dim clamp.
    const float infExtent = 0.5f * float(kEnvTileMaxDim) * kEnvTileSize;
    // The env rows' LOOK blocks are irrelevant to the extent - only
    // each node's zone shape (transform + volume knobs) is.
    for (const EnvRow& envRow : scene.tables.envs) {
        const NodeId node = envRow.node;
        if (node >= scene.globalTransform.size())
            continue;
        uint32_t infMask = 0;
        float blendWeight = 1.0f;
        bool enabled = true;
        if (const EnvVolumeRow* vit = FindEnvVolume(scene, node)) {
            infMask = vit->volume.infMask;
            blendWeight = vit->volume.blendWeight;
            enabled = vit->volume.enabled;
        }
        if (!enabled || blendWeight <= 0.0f)
            continue; // the bake skips it: no bounds contribution
        const glm::mat4& g = scene.globalTransform[node];
        // Per local axis: the column length is the half extent; INF
        // length (the legacy INF scale) or an inf-mask face on the axis
        // makes it unbounded (the bake's own rule).
        bool axisInf[3] = {false, false, false};
        for (int c = 0; c < 3; ++c) {
            const float len = glm::length(glm::vec3(g[c]));
            axisInf[c] = !std::isfinite(len) || (infMask & (3u << (2 * c))) != 0u;
        }
        // The FINITE axes' corners (translation + rotation applied): the
        // zone's world AABB — an all-infinite zone degenerates to its
        // translation.
        for (int sz = -1; sz <= 1; sz += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sx = -1; sx <= 1; sx += 2) {
                    const glm::vec4 local(axisInf[0] ? 0.0f : float(sx),
                                          axisInf[1] ? 0.0f : float(sy),
                                          axisInf[2] ? 0.0f : float(sz),
                                          1.0f);
                    unionPoint(glm::vec3(g * local));
                }
        // The UNBOUNDED axes: extend the WORLD XZ grid where the axis
        // projects (a nonzero column component), or — a flat column, the
        // authoring norm is axis-aligned — the world axis c. NEVER the
        // vertical range (see the header).
        const auto unionAxisX = [&](float v) {
            mn.x = std::min(mn.x, v);
            mx.x = std::max(mx.x, v);
        };
        const auto unionAxisZ = [&](float v) {
            mn.z = std::min(mn.z, v);
            mx.z = std::max(mx.z, v);
        };
        for (int c = 0; c < 3; ++c) {
            if (!axisInf[c])
                continue;
            const glm::vec3 col = glm::vec3(g[c]);
            const bool flat = !(glm::length(col) > 0.0f);
            const bool extX = flat ? c == 0 : std::abs(col.x) > 1e-6f;
            const bool extZ = flat ? c == 2 : std::abs(col.z) > 1e-6f;
            if (extX) {
                unionAxisX(-infExtent);
                unionAxisX(infExtent);
            }
            if (extZ) {
                unionAxisZ(-infExtent);
                unionAxisZ(infExtent);
            }
        }
    }

    // ---- Tile-aligned + one tile of margin + the 16 km dim clamp. ----
    const glm::vec2 margin(kEnvTileSize);
    EnvGridExtent out{};
    out.gridMin = glm::floor(glm::vec2(mn.x, mn.z) / kEnvTileSize) * kEnvTileSize - margin;
    const glm::vec2 gMax = glm::ceil(glm::vec2(mx.x, mx.z) / kEnvTileSize) * kEnvTileSize + margin;
    out.dimX = std::min(uint32_t((gMax.x - out.gridMin.x) / kEnvTileSize), kEnvTileMaxDim);
    out.dimZ = std::min(uint32_t((gMax.y - out.gridMin.y) / kEnvTileSize), kEnvTileMaxDim);
    out.yMin = mn.y;
    out.yMax = mx.y;
    return out;
}

bool WorldEnvTiles::LoadCooked(glm::vec2 min, uint32_t dx, uint32_t dz,
                               const EnvTileRecord* looks,
                               const smvk::envzones::EnvZoneGpu* shapes,
                               uint32_t count) {
    built_ = false;
    if (!looks || !shapes || !count || count > kEnvTileMaxRecords ||
        !dx || !dz || dx > kEnvTileMaxDim || dz > kEnvTileMaxDim ||
        !std::isfinite(min.x) || !std::isfinite(min.y))
        return false;
    records_.assign(kEnvTileMaxRecords, EnvTileRecord{});
    shapes_.assign(kEnvTileMaxRecords, smvk::envzones::EnvZoneGpu{});
    std::copy(looks, looks + count, records_.begin());
    std::copy(shapes, shapes + count, shapes_.begin());
    recordCount_ = count;
    gridMin_ = min;
    dimX_ = dx;
    dimZ_ = dz;
    candidates_.clear();
    candidateIndices_.clear();
    farCandidates_.clear();
    cells_.assign(size_t(dx) * dz, EnvTileCell{});
    built_ = true;
    return true;
}

bool WorldEnvTiles::Build(const Scene& scene,
                          glm::vec2 gridMin,
                          uint32_t dimX,
                          uint32_t dimZ,
                          float yMin,
                          float yMax) {
    built_ = false;
    dimX = std::min(dimX, kEnvTileMaxDim);
    dimZ = std::min(dimZ, kEnvTileMaxDim);
    if (dimX == 0 || dimZ == 0)
        return false;
    gridMin_ = gridMin;
    dimX_ = dimX;
    dimZ_ = dimZ;

    // ---- Records: [0] = the base (the default environment block derived
    // against ITSELF — the all-infinite zone-zero, always present
    // at weight 1), then node zones derived against that same ACTIVE
    // resolved look. Runtime edits must not read the stale source default. ----
    records_.clear();
    records_.resize(kEnvTileMaxRecords);
    {
        const smvk::envzones::EnvZone baseLook =
            smvk::envzones::DeriveZoneLook(scene.environment, scene.environment);
        records_[0].sky = glm::vec4(baseLook.skyTint, baseLook.ambientI);
        records_[0].fog = glm::vec4(baseLook.fogColor, baseLook.fogDensity);
        records_[0].vol = glm::vec4(baseLook.volumetrics, baseLook.volumetricDensity, 0.0f, 0.0f);
    }

    // ---- Candidates: the SAME collection/sort the retired per-frame
    // packer used (priority desc -> bounded volume asc -> node id), with
    // the base prepended as the all-infinite fallback. ----
    candidates_.clear();
    {
        // The base candidate: an identity-transform, all-infinite OBB (the
        // whole world covered at weight 1), priority from the default
        // block (usually 0 — node zones win inside their volumes).
        Candidate base{};
        (void)smvk::envzones::MakeEnvZoneGpu(glm::mat4(1.0f),
                                             63u,
                                             smvk::envzones::EnvZone{},
                                             scene.environment.zonePriority,
                                             1.0f,
                                             0.0f,
                                             base.zone);
        base.record = 0;
        base.boundedVolume = std::numeric_limits<double>::infinity();
        base.nodeId = UINT32_MAX;
        base.priority = scene.environment.zonePriority;
        base.xzMin = glm::vec2(-std::numeric_limits<float>::infinity());
        base.xzMax = glm::vec2( std::numeric_limits<float>::infinity());
        candidates_.push_back(base);

        struct NodeZone {
            Candidate cand;
            float priority = 0.0f;
            double volume = 0.0;
            uint32_t nodeId = 0;
        };
        std::vector<NodeZone> nodeZones;
        nodeZones.reserve(scene.tables.envs.size());
        for (const EnvRow& envRow : scene.tables.envs) {
            const NodeId node = envRow.node;
            const tour::scene::SceneEnv& env = envRow.environment;
            if (node >= scene.globalTransform.size())
                continue;
            // EnvVolume shape knobs (absent row = the legacy defaults).
            uint32_t infMask = 0;
            float priority = env.zonePriority;
            float blendWeight = 1.0f;
            float blendRadius = 0.0f;
            bool enabled = true;
            if (const EnvVolumeRow* vit = FindEnvVolume(scene, node)) {
                infMask = vit->volume.infMask;
                priority = vit->volume.priority; // volume row wins
                blendWeight = vit->volume.blendWeight;
                blendRadius = vit->volume.blendRadius;
                enabled = vit->volume.enabled;
            }
            if (!enabled || blendWeight <= 0.0f)
                continue;

            Candidate cand{};
            const smvk::envzones::EnvZone look =
                smvk::envzones::DeriveZoneLook(env, scene.environment);
            if (!smvk::envzones::MakeEnvZoneGpu(scene.globalTransform[node],
                                                infMask,
                                                look,
                                                priority,
                                                blendWeight,
                                                blendRadius,
                                                cand.zone))
                continue; // empty zone (flat finite axis without an inf face)

            // Bounded volume: the product of the finite column lengths;
            // any unbounded face/INF column = +inf.
            const glm::mat4& g = scene.globalTransform[node];
            double volume = 1.0;
            bool unbounded = false;
            for (int c = 0; c < 3; ++c) {
                const float len = glm::length(glm::vec3(g[c]));
                if (!std::isfinite(len) || len <= 0.0f || (infMask & (3u << (2 * c))) != 0u) {
                    unbounded = true;
                    break;
                }
                volume *= double(len) * 2.0;
            }
            NodeZone nz{};
            // Conservative XZ projection, enlarged by fade radius.
            // Infinite faces are over-included, never silently missed.
            bool infinite = infMask != 0u;
            for (int a = 0; a < 3; ++a)
                infinite |= !std::isfinite(glm::length(glm::vec3(g[a])));
            if (infinite) {
                cand.xzMin = glm::vec2(-std::numeric_limits<float>::infinity());
                cand.xzMax = glm::vec2( std::numeric_limits<float>::infinity());
            } else {
                glm::vec2 lo(std::numeric_limits<float>::infinity());
                glm::vec2 hi(-std::numeric_limits<float>::infinity());
                for (int sx : {-1, 1})
                    for (int sy : {-1, 1})
                        for (int sz : {-1, 1}) {
                            const glm::vec3 p = glm::vec3(g * glm::vec4(float(sx), float(sy), float(sz), 1.0f));
                            lo = glm::min(lo, glm::vec2(p.x, p.z));
                            hi = glm::max(hi, glm::vec2(p.x, p.z));
                        }
                cand.xzMin = lo - glm::vec2(blendRadius);
                cand.xzMax = hi + glm::vec2(blendRadius);
            }
            nz.cand = cand;
            nz.cand.record = 0; // assigned after the sort
            nz.cand.boundedVolume = unbounded ? std::numeric_limits<double>::infinity() : volume;
            nz.cand.nodeId = node;
            nz.priority = priority;
            nz.volume = nz.cand.boundedVolume;
            nz.nodeId = node;
            nodeZones.push_back(nz);
        }
        // The packer's sort: priority desc -> volume asc (innermost) ->
        // node id. Past the record budget: keep the TOP zones (the base
        // never truncates — it is candidates_[0] and the sort is stable,
        // but slice from the front and re-append the base if the cut hits
        // it, exactly like the old overflow guard).
        std::stable_sort(
            nodeZones.begin(), nodeZones.end(), [](const NodeZone& a, const NodeZone& b) {
                if (a.priority != b.priority)
                    return a.priority > b.priority;
                if (a.volume != b.volume)
                    return a.volume < b.volume;
                return a.nodeId < b.nodeId;
            });
        const uint32_t maxNodeZones = kEnvTileMaxRecords - 1; // [0] = base
        uint32_t assigned = 0;
        for (const NodeZone& nz : nodeZones) {
            if (assigned >= maxNodeZones)
                break;
            Candidate cand = nz.cand;
            cand.record = 1 + assigned; // records_[0] is the base
            candidates_.push_back(cand);
            // Store the LOOK into the record.
            smvk::envzones::EnvZone look;
            look.skyTint = glm::vec3(cand.zone.sky);
            look.ambientI = cand.zone.sky.w;
            look.fogColor = glm::vec3(cand.zone.fog);
            look.fogDensity = cand.zone.fog.w;
            look.volumetrics = cand.zone.vol.x;
            look.volumetricDensity = cand.zone.vol.y;
            records_[cand.record].sky = glm::vec4(look.skyTint, look.ambientI);
            records_[cand.record].fog = glm::vec4(look.fogColor, look.fogDensity);
            records_[cand.record].vol =
                glm::vec4(look.volumetrics, look.volumetricDensity, 0.0f, 0.0f);
            ++assigned;
        }
        recordCount_ = 1 + assigned;
    }

    // Re-sort the FULL candidate list (base included) with the packer's
    // ordering so Evaluate()'s first-hit is the winner, matching the
    // retired shader walk exactly.
    std::stable_sort(
        candidates_.begin(), candidates_.end(), [](const Candidate& a, const Candidate& b) {
            if (a.priority != b.priority)
                return a.priority > b.priority;
            if (a.boundedVolume != b.boundedVolume)
                return a.boundedVolume < b.boundedVolume;
            return a.nodeId < b.nodeId;
        });

    // Spatial bake: keep every potentially influencing OBB, sorted by
    // priority, bounded volume, then node ID. Includes the base.
    shapes_.assign(kEnvTileMaxRecords, smvk::envzones::EnvZoneGpu{});
    farCandidates_.clear();
    // Always-resident global shortlist: avoid loading the entire far
    // tile grid. Include the base IN SORT ORDER so base priority still
    // beats any lower-priority far look (first influencing candidate).
    // 512m projected footprint or any infinite face is far-relevant.
    for (const Candidate& candidate : candidates_) {
        shapes_[candidate.record] = candidate.zone;
        const glm::vec2 span = candidate.xzMax - candidate.xzMin;
        const bool unbounded = !std::isfinite(span.x) || !std::isfinite(span.y);
        if (candidate.record == 0 || unbounded ||
            span.x >= 512.0f || span.y >= 512.0f)
            farCandidates_.push_back(candidate.record);
    }
    cells_.assign(size_t(dimX_) * dimZ_, EnvTileCell{});
    candidateIndices_.clear();
    for (uint32_t iz = 0; iz < dimZ_; ++iz)
        for (uint32_t ix = 0; ix < dimX_; ++ix) {
            EnvTileCell& cell = cells_[size_t(iz) * dimX_ + ix];
            cell.firstCandidate = uint32_t(candidateIndices_.size());
            const glm::vec2 lo = gridMin_ + glm::vec2(float(ix), float(iz)) * kEnvTileSize;
            const glm::vec2 hi = lo + glm::vec2(kEnvTileSize);
            for (const Candidate& candidate : candidates_) {
                if (candidate.xzMax.x < lo.x || candidate.xzMin.x > hi.x ||
                    candidate.xzMax.y < lo.y || candidate.xzMin.y > hi.y)
                    continue;
                candidateIndices_.push_back(candidate.record);
                ++cell.candidateCount;
            }
        }
    (void)yMin;
    (void)yMax;
    built_ = true;
    return true;
}

WorldEnvTiles::Lookup WorldEnvTiles::Evaluate(const glm::vec3& p) const {
    // The retired shader walk, verbatim: the FIRST influencing candidate
    // in the sorted list wins; nothing covering falls to the BASE record
    // at weight 1 (candidates_[0] is the all-infinite base).
    for (const Candidate& c : candidates_) {
        const float w =
            std::clamp(c.zone.shape.y, 0.0f, 1.0f) * smvk::envzones::EnvZoneFalloff(c.zone, p);
        if (!(w > 0.0f))
            continue;
        Lookup out{};
        out.record = c.record;
        out.weight = w;
        out.valid = true;
        return out;
    }
    Lookup base{};
    base.record = 0;
    base.weight = 1.0f;
    base.valid = true;
    return base;
}

WorldEnvTiles::Lookup WorldEnvTiles::At(glm::vec3 pos) const {
    Lookup out{};
    if (!built_) return out;
    out.valid = true;
    out.record = 0;
    out.weight = 1.0f;
    const float fx = std::floor((pos.x - gridMin_.x) / kEnvTileSize);
    const float fz = std::floor((pos.z - gridMin_.y) / kEnvTileSize);
    if (!(fx >= 0.0f) || !(fz >= 0.0f) ||
        fx >= float(dimX_) || fz >= float(dimZ_))
        return out;
    const EnvTileCell& cell = cells_[size_t(fz) * dimX_ + size_t(fx)];
    for (uint32_t i = 0; i < cell.candidateCount; ++i) {
        const uint32_t id = candidateIndices_[cell.firstCandidate + i];
        const auto& shape = shapes_[id];
        const float w = std::clamp(shape.shape.y, 0.0f, 1.0f) *
                        smvk::envzones::EnvZoneFalloff(shape, pos);
        if (w > 0.0f) {
            out.record = id;
            out.weight = w;
            return out;
        }
    }
    return out;
}

bool WorldEnvTiles::AnyVolumetricFog() const {
    // The volOn gates' zone-density leg (see the header comment): can
    // the zone tables raise visible extinction from a ZERO pass
    // density? Only a record's ABSOLUTE volumetricDensity (vol.y)
    // can — the ratio is multiplicative over the pass intensity the
    // gates already require positive. records_ is the fixed 64-slot
    // array; only [0, recordCount_) are live (the bake's assignment
    // prefix — everything past it is zeroed residue).
    if (!built_)
        return false;
    for (uint32_t i = 0; i < recordCount_; ++i)
        if (records_[i].vol.y > 0.0f)
            return true;
    return false;
}

} // namespace engine::worldpartition
