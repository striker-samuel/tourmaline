// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Offline terrain-only intersection seam ribbon baker.
// The caller supplies world-space mesh triangles (three vertices per face).
// No GPU, Vulkan, editor, or CGAL dependency. Results are source geometry,
// not a registered runtime renderer draw. See docs/terrain_seam_ribbons.md.

#include <Engine/Rendergraph/terrain_hfield.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace smvk::terrain {

struct SeamRibbonOptions {
    float width = 0.75f;               // full ribbon width, metres
    float sampleStep = 0.25f;          // maximum edge root-search spacing
    float lift = 0.015f;               // positive Y offset avoids terrain z-fighting
    uint32_t maxSamplesPerEdge = 4096; // cap pathological input
};

struct SeamRibbonVertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{};      // u = metres along segment, v = cross-section
    float weight = 0.0f; // 1 at intersection, 0 at ribbon outer edges
};

struct SeamRibbonMesh {
    std::vector<SeamRibbonVertex> vertices;
    std::vector<uint32_t> indices; // independent 4-vertex strip per contact segment
    uint32_t contactSegments = 0;
    uint32_t skippedTriangles = 0; // ambiguous/tangent/degenerate intersections
};

namespace detail {
[[nodiscard]] inline float TerrainDifference(const TerrainHField& f, const glm::vec3& p) noexcept {
    return p.y - SampleHeight(f, p.x, p.z);
}

inline void AddContact(std::vector<glm::vec3>& points, const glm::vec3& p, float tolerance) {
    for (const auto& q : points)
        if (glm::dot(q - p, q - p) <= tolerance * tolerance)
            return;
    points.push_back(p);
}

// Search along the *mesh* edge: linear interpolation of signed height
// differences at endpoints alone misses multiple terrain crossings.
inline void FindEdgeContacts(const TerrainHField& f,
                             const glm::vec3& a,
                             const glm::vec3& b,
                             const SeamRibbonOptions& opts,
                             std::vector<glm::vec3>& points) {
    const float planarLength = glm::length(glm::vec2(b.x - a.x, b.z - a.z));
    const float step = std::max(opts.sampleStep, 0.001f);
    const uint32_t n = std::clamp(static_cast<uint32_t>(std::ceil(planarLength / step)),
                                  1u,
                                  std::max(opts.maxSamplesPerEdge, 1u));
    const float tol = std::max(step * 0.01f, 1e-4f);
    glm::vec3 prev = a;
    float dp = TerrainDifference(f, prev);
    if (std::abs(dp) <= tol)
        AddContact(points, prev, tol);
    for (uint32_t i = 1; i <= n; ++i) {
        const float t = float(i) / float(n);
        const glm::vec3 curr = a + (b - a) * t;
        const float dc = TerrainDifference(f, curr);
        if (std::abs(dc) <= tol)
            AddContact(points, curr, tol);
        if ((dp < -tol && dc > tol) || (dp > tol && dc < -tol)) {
            float lo = float(i - 1) / float(n), hi = t;
            const bool positiveAtLo = dp > 0.0f;
            for (int iter = 0; iter < 18; ++iter) {
                const float mid = (lo + hi) * 0.5f;
                const glm::vec3 p = a + (b - a) * mid;
                if ((TerrainDifference(f, p) > 0.0f) == positiveAtLo)
                    lo = mid;
                else
                    hi = mid;
            }
            AddContact(points, a + (b - a) * ((lo + hi) * 0.5f), tol);
        }
        prev = curr;
        dp = dc;
    }
}
} // namespace detail

// Produce terrain-conforming ribbons from world-space mesh triangles.
// Two contacts per triangle form one intersection segment. This v1 skips
// triangles with more than two distinct contacts (multi-crossing surfaces),
// coplanar terrain triangles and tangent-only contacts instead of inventing
// topology. Duplicate segments on shared edges should be welded before
// production rendering. This is a bake primitive, not yet a spline fitter.
[[nodiscard]] inline SeamRibbonMesh BakeTerrainSeamRibbons(
    const TerrainHField& terrain,
    std::span<const glm::vec3> triangleVertices,
    const SeamRibbonOptions& opts = {}) {
    SeamRibbonMesh result;
    if (!terrain.valid() || opts.width <= 0.0f || opts.sampleStep <= 0.0f ||
        opts.maxSamplesPerEdge == 0)
        return result;

    const float halfWidth = opts.width * 0.5f;
    for (size_t i = 0; i + 2 < triangleVertices.size(); i += 3) {
        const glm::vec3 a = triangleVertices[i];
        const glm::vec3 b = triangleVertices[i + 1];
        const glm::vec3 c = triangleVertices[i + 2];
        std::vector<glm::vec3> contacts;
        contacts.reserve(4);
        detail::FindEdgeContacts(terrain, a, b, opts, contacts);
        detail::FindEdgeContacts(terrain, b, c, opts, contacts);
        detail::FindEdgeContacts(terrain, c, a, opts, contacts);
        if (contacts.size() != 2) {
            if (!contacts.empty())
                ++result.skippedTriangles;
            continue;
        }

        const glm::vec2 delta(contacts[1].x - contacts[0].x, contacts[1].z - contacts[0].z);
        const float len = glm::length(delta);
        if (len < 1e-5f) {
            ++result.skippedTriangles;
            continue;
        }
        const glm::vec2 side(-delta.y / len, delta.x / len);
        const uint32_t first = static_cast<uint32_t>(result.vertices.size());
        for (int endpoint = 0; endpoint < 2; ++endpoint) {
            const glm::vec3 p = contacts[endpoint];
            for (int strip = 0; strip < 3; ++strip) {
                const float v = float(strip) * 0.5f;
                const float offset = (v - 0.5f) * opts.width;
                const float x = p.x + side.x * offset;
                const float z = p.z + side.y * offset;
                result.vertices.push_back({glm::vec3(x, SampleHeight(terrain, x, z) + opts.lift, z),
                                           SampleNormal(terrain, x, z),
                                           glm::vec2(endpoint ? len : 0.0f, v),
                                           strip == 1 ? 1.0f : 0.0f});
            }
        }
        for (uint32_t j = 0; j < 2; ++j) {
            const uint32_t v = first + j;
            result.indices.insert(result.indices.end(), {v, v + 1, v + 3, v + 1, v + 4, v + 3});
        }
        ++result.contactSegments;
    }
    return result;
}

} // namespace smvk::terrain
