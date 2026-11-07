// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// CPU reference tracing for STATIC mesh-local SDFs. This intentionally
// does not track animated/skinned objects or alter the SSR render graph.
// The result is suitable for offline reflection probes and for validating
// a future GPU SDF reflection pass against the exact same TSDF sidecars.

#include <Engine/Asset/sdf.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include <glm/mat4x4.hpp>

namespace tour::asset {

// The caller owns the volume and keeps its transform fixed for the trace.
// The transform may be nonuniformly scaled, but must be invertible/affine.
// A moving/skinned mesh must NOT be registered as a static instance.
struct StaticSdfInstance {
    const SdfVolume* volume = nullptr;
    glm::mat4 meshToWorld{1.0f};
};

struct StaticSdfTraceOptions {
    float maxDistance = 50.0f; // world units, including reflection origin bias
    float hitEpsilon = 0.01f;  // minimum world-space surface tolerance
    float originBias = 0.03f;  // reflection self-intersection bias (world units)
    uint32_t maxSteps = 128;   // maximum sphere-tracing iterations per instance
};

struct StaticSdfHit {
    size_t instanceIndex = 0;
    float distance = 0.0f;    // world distance from the original ray origin
    glm::vec3 position{0.0f}; // world-space hit position
    glm::vec3 normal{0.0f};   // world-space normal facing the incident ray
};

// Ray origin and direction are in world space (direction need not be unit).
// Finds the closest STATIC instance in the given collection. Empty, invalid,
// out-of-range and degenerate entries are safely skipped. The hit is only
// written on success; missing/invalid SDF data never produces a false hit.
bool TraceStaticSdfRay(std::span<const StaticSdfInstance> instances,
                       const glm::vec3& worldOrigin,
                       const glm::vec3& worldDirection,
                       const StaticSdfTraceOptions& options,
                       StaticSdfHit& hit);

// Reflect the INCIDENT direction at the supplied receiver normal, bias the
// origin along the reflected direction, then find the nearest static SDF.
// This is a geometric ray query, NOT an HDR/shading or GPU SSR composite.
bool TraceStaticSdfReflection(std::span<const StaticSdfInstance> instances,
                              const glm::vec3& receiverWorldPosition,
                              const glm::vec3& incidentWorldDirection,
                              const glm::vec3& receiverWorldNormal,
                              const StaticSdfTraceOptions& options,
                              StaticSdfHit& hit);

} // namespace tour::asset
