// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// GPU-scene meshlet-indexed raster contract. A candidate maps one current
// GPU work slot to one cluster of a resident indexed geometry registration.
// It is intentionally independent of GfxFilter's per-view draw identity.
#include <Engine/Renderer/gfx_filter.h>
#include <Engine/Renderer/gpu_scene_raster.h>
#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>
#include <limits>

namespace tour::gpu_scene {

struct alignas(16) RasterMeshlet {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t workSlot = 0;
    uint32_t flags = 0; // bit 0: double-sided
    glm::vec4 sphere{0.0f}; // local center, radius
};
static_assert(sizeof(RasterMeshlet) == 32);

// Derive one command candidate from the SAME cluster-ordered heap as the
// unfiltered fallback. Caller must supply a valid work slot and the owning
// mesh registration. Never fabricate a command from discontinuous geometry.
[[nodiscard]] inline bool ResolveRasterMeshlet(
    const gfx::Cluster& cluster, uint32_t expectedMesh, uint32_t indexBase,
    uint32_t workSlot, bool doubleSided, RasterMeshlet& out) noexcept {
    out = {};
    if (cluster.mesh != expectedMesh || cluster.triCount == 0 ||
        cluster.triCount > std::numeric_limits<uint32_t>::max() / 3u ||
        cluster.indexOffset > std::numeric_limits<uint32_t>::max() - indexBase)
        return false;
    const uint32_t count = cluster.triCount * 3u;
    if (count > std::numeric_limits<uint32_t>::max() -
                    (cluster.indexOffset + indexBase))
        return false;
    out.firstIndex = cluster.indexOffset + indexBase;
    out.indexCount = count;
    out.workSlot = workSlot;
    out.flags = doubleSided ? 1u : 0u;
    // Missing/nonfinite bounds MUST fail open in the cull shader. Do not
    // mistake a zero/default bound for proof of invisibility.
    const auto c = cluster.boundCenter;
    out.sphere = glm::vec4(c, cluster.boundRadius);
    return true;
}

[[nodiscard]] inline RasterIndirectCommand RasterMeshletCommand(
    const RasterMeshlet& candidate, bool visible) noexcept {
    if (!visible || candidate.indexCount == 0)
        return EmptyRasterIndirectCommand();
    return RasterIndirectCommand{candidate.indexCount, 1u,
                                 candidate.firstIndex, 0, candidate.workSlot};
}

} // namespace tour::gpu_scene
