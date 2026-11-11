// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Device-free selection of ONE final raster backend for canonical GPU Scene
// work. Identity, source admission, materials and LOD remain authoritative in
// the existing RenderDatabase / WorldSynchronizer, not a parallel registry.
#include <cstdint>
namespace tour::gpu_scene {
enum class RasterAlphaMode : uint8_t { Opaque, Mask, Blend };
enum class RasterBackend : uint8_t {
    LegacyCompatibility, TaskMesh, IndexedFallback, Transparent
};
inline constexpr uint32_t kNoOpaqueRasterClass = 0xffffffffu;
struct RasterRouteAvailability {
    bool gpuSceneOwned = false; // complete per-flight source admission
    bool taskMeshReady = false; // compatible meshlet streams and PSO
    bool indexedReady = false; // valid indexed geometry and PSO
    bool transparentReady = false; // dedicated sorted/OIT BLEND pass ready
};
struct RasterRoute {
    RasterBackend backend = RasterBackend::LegacyCompatibility;
    uint32_t opaqueRasterClass = kNoOpaqueRasterClass;
};
// BLEND is never silently treated as MASK, even when task/mesh is ready.
// Until all material variants are supported, compatibility keeps ownership.
[[nodiscard]] constexpr RasterRoute SelectRasterRoute(
    RasterAlphaMode alphaMode, bool doubleSided,
    RasterRouteAvailability ready) noexcept {
    if (!ready.gpuSceneOwned) return {};
    if (alphaMode == RasterAlphaMode::Blend)
        return {ready.transparentReady ? RasterBackend::Transparent
                                       : RasterBackend::LegacyCompatibility,
                kNoOpaqueRasterClass};
    const uint32_t cls = (alphaMode == RasterAlphaMode::Mask ? 2u : 0u) |
                         (doubleSided ? 1u : 0u);
    if (ready.taskMeshReady) return {RasterBackend::TaskMesh, cls};
    if (ready.indexedReady) return {RasterBackend::IndexedFallback, cls};
    return {RasterBackend::LegacyCompatibility, cls};
}
} // namespace tour::gpu_scene
