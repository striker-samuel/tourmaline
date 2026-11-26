// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Impostor ray-tracing proxy selection (see DeriveConfig::impostorProxy and
// Engine/Renderer/mesh.h ImpostorProxy). Only a SMALL subset of static meshes
// carry a six-view depth proxy: the .geometry baked it, the importer uploaded
// it as a hidden sibling mesh, and the RT builder swaps it in for the full mesh
// at far distance so BLAS traversal cost stays flat on distant shadow casters.
//
// The data path is sparse and allocation-free on the hot path:
//   WorldPage::impostorProxies_  — a compact map keyed by base mesh (load-time)
//   World::GetImpostorProxy      — a pointer lookup (no allocation)
//   the RT build                 — FrameVector<RayTracingInstance> (frame arena)
// Only the selection helper below is new runtime math; it reuses the same
// ANGULAR threshold regardless of viewport/FOV.

#include <cstdint>

#include <Engine/Renderer/mesh.h> // ImpostorProxy (the sparse proxy record)

namespace smvk {

// Angular radius (radians) below which a mesh's world-space bound is small
// enough on screen that its exact silhouette no longer matters for shadow
// casting. ~0.11 deg. Independent of viewport/FOV by construction.
inline constexpr float kImpostorSwitchAngularRadius = 0.002f;

// True when an instance whose world-space bound radius is `radiusWorld` at
// `distance` from the camera should use the impostor proxy instead of the
// full mesh in the ray-query world. Pure + CPU-testable.
inline bool UseImpostorProxyAt(float radiusWorld, float distance) {
    if (!(radiusWorld > 0.0f) || !(distance > 0.0f))
        return false;
    return (radiusWorld / distance) < kImpostorSwitchAngularRadius;
}

} // namespace smvk
