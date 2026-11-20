// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Experimental, device-free contract for decomposing WorldRenderer.
// Not wired to the runtime: no changes to the active rendergraph path.
#include <cstdint>

namespace engine {

enum class GeometryBackendKind : uint8_t {
    CpuIndirect,      // CPU-generated/uploaded indexed-indirect commands
    GpuSceneIndexed,  // GPU-scene cull producing indexed-indirect commands
    GpuSceneTaskMesh, // GPU-scene cull + task/mesh shader submission
    ComputeFilter     // compute batch-cull + Hi-Z filter feeding filtered draws
};

enum class GeometryConfigError : uint8_t {
    None,
    HizNeedsDepth,
    OcclusionNeedsHiz,
    FragmentNeedsGeometry
};

struct GeometryPipelineConfig {
    GeometryBackendKind backend = GeometryBackendKind::GpuSceneIndexed;
    bool depthPrepass = true;
    bool hiz = true;
    bool hizOcclusion = true;
    bool fragmentPass = true;
    bool postProcess = true;
    bool geometrySubmission = true;
};

// Only structural invariants are checked here. The integrator must also
// validate device capabilities, materials, per-view resources and shader ABIs.
[[nodiscard]] constexpr GeometryConfigError ValidateGeometryPipeline(
    const GeometryPipelineConfig& config) noexcept {
    if (config.hiz && !config.depthPrepass)
        return GeometryConfigError::HizNeedsDepth;
    if (config.hizOcclusion && !config.hiz)
        return GeometryConfigError::OcclusionNeedsHiz;
    if (config.fragmentPass && !config.geometrySubmission)
        return GeometryConfigError::FragmentNeedsGeometry;
    return GeometryConfigError::None;
}

// Immutable, per-flight snapshots are intentionally opaque in this phase.
// Their concrete types and lifetimes must be established by scene/streaming
// extraction before this interface is implemented by a live renderer.
struct GeometryFrameInputs;
struct GeometryPassRecorder;
struct GeometryBackendResources;

// One backend may own raster submission for an admitted geometry unit.
// Never run multiple implementations on the same geometry in one pass.
// Common depth/Hi-Z and shading targets are owned by the frame composer.
class IGeometryBackend {
  public:
    virtual ~IGeometryBackend() = default;
    [[nodiscard]] virtual GeometryBackendKind Kind() const noexcept = 0;
    virtual bool Initialize(GeometryBackendResources&) = 0;
    virtual void Shutdown(GeometryBackendResources&) noexcept = 0;
    virtual void Prepare(const GeometryFrameInputs&) = 0;
    virtual void RecordDepth(GeometryPassRecorder&, const GeometryFrameInputs&) = 0;
    virtual void RecordOpaque(GeometryPassRecorder&, const GeometryFrameInputs&) = 0;
    virtual void RecordMasked(GeometryPassRecorder&, const GeometryFrameInputs&) = 0;
};

} // namespace engine
