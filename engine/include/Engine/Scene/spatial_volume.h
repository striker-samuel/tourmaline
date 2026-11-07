// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE SHARED SPATIAL-VOLUME SHAPE (docs/audio_graph.md §4.3): the
// neutral oriented-box math EVERY volume-flavoured node association
// agrees on — the GI-volume PLACEMENT convention (center = the node's
// global translation, axes = the normalized transform columns,
// half-extents = HALF the column lengths), with per-face metres-based
// soft fades. Audio rooms, GI volumes and any future volume kind
// share ONE shape definition; the GI bake's own plane machinery
// (GiVolumeBoxPlanesRotated / GiVolumeDef::Weight) keeps its math —
// tests pin the PARITY (test_audiograph / test_scene_tables), the
// audio runtime NEVER includes GI headers.
//
// The convention trap this header exists to avoid: environment zones
// use a DIFFERENT half-extent convention (their [-1,1]^3 unit box);
// copying that math doubles a GI-shaped room's bounds. Everything
// here derives from the GLOBAL TRANSFORM exactly like the editor's
// GiVolumeOBB (editor/src/editor_commands.cpp).
//
// Pure math, header-only, GPU-free, GI-free.

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace tour::scene {

// One oriented box decoded from a node's global transform.
struct SpatialVolumeBox {
    glm::vec3 center{0.0f}; // globalTransform[3]
    glm::mat3 axes{1.0f};   // COLUMNS = the box axes (normalized)
    glm::vec3 half{0.5f};   // half-extents along the axes (metres)

    [[nodiscard]] bool Valid() const {
        // Finite + non-degenerate: every axis normalized to unit
        // length and a positive half-extent (a flat or NaN'd shape
        // covers nothing — weights read 0, never NaN).
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(center[i]) || !std::isfinite(half[i]))
                return false;
            if (!(half[i] > 0.0f))
                return false;
            const glm::vec3 axis = axes[i];
            if (!std::isfinite(axis.x) || !std::isfinite(axis.y) || !std::isfinite(axis.z))
                return false;
        }
        return true;
    }
};

// Decodes the box from a node's GLOBAL transform: center = the
// translation column, axis[i] = the normalized i-th basis column,
// half[i] = half the column's length (the GI-volume placement
// convention — the editor's GiVolumeOBB, verbatim).
[[nodiscard]] inline SpatialVolumeBox VolumeBoxFromGlobalTransform(const glm::mat4& g) {
    SpatialVolumeBox box;
    box.center = glm::vec3(g[3]);
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 col = glm::vec3(g[i]);
        const float len = glm::length(col);
        box.half[i] = 0.5f * len;
        box.axes[i] = (len > 1e-6f) ? col / len : glm::vec3(0.0f);
    }
    return box;
}

// The per-face smooth coverage in [0,1]: `d` is the signed INSIDE
// distance in metres (negative = outside the face). The face ramps
// from 0 at d = -outsideFade (fully beyond the face's outer fade) to
// 1 at d = +insideFade (safely inside). smoothstep, like the GI
// volume's fades (GiVolumeSmoothStep parity).
[[nodiscard]] inline float SpatialVolumeFaceWeight(float d, float insideFade, float outsideFade) {
    const float a = -std::max(outsideFade, 0.0f);
    const float b = std::max(insideFade, 0.0f);
    if (b <= a)
        return d >= b ? 1.0f : (d <= a ? 0.0f : 0.5f); // degenerate ramp
    float t = (d - a) / (b - a);
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// The box's coverage weight at `pos` in [0,1]: the MINIMUM of the six
// face weights (the nearest face dominates — the GiVolumeDef::Weight
// rule). A degenerate/invalid box covers nothing (0).
[[nodiscard]] inline float SpatialVolumeWeight(const SpatialVolumeBox& b,
                                               const glm::vec3& pos,
                                               float insideFade,
                                               float outsideFade) {
    if (!b.Valid())
        return 0.0f;
    const glm::vec3 local = pos - b.center;
    float w = 1.0f;
    for (int i = 0; i < 3; ++i) {
        const float u = glm::dot(b.axes[i], local); // signed along axis
        const float wNeg = SpatialVolumeFaceWeight(u + b.half[i], insideFade, outsideFade);
        const float wPos = SpatialVolumeFaceWeight(b.half[i] - u, insideFade, outsideFade);
        w = std::min(w, std::min(wNeg, wPos));
        if (w <= 0.0f)
            return 0.0f;
    }
    return w;
}

// The eight box corners (bit pattern: bit0 -> -x/+x on axis 0, bit1
// axis 1, bit2 axis 2) — the gizmo/overlay drawing shape.
[[nodiscard]] inline void SpatialVolumeCorners(const SpatialVolumeBox& b, glm::vec3 out[8]) {
    for (int i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? b.half[0] : -b.half[0];
        const float sy = (i & 2) ? b.half[1] : -b.half[1];
        const float sz = (i & 4) ? b.half[2] : -b.half[2];
        out[i] = b.center + b.axes[0] * sx + b.axes[1] * sy + b.axes[2] * sz;
    }
}

} // namespace tour::scene
