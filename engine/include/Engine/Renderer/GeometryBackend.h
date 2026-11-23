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

// Work/command generation: the cull (batch cull, filter, fused visibility)
// that produces the per-flight indirect command stream. This is the PRODUCER
// half — it owns the cull dispatch and the command buffers it writes, but
// never records a draw. Separated from IGeometryRenderer so a cull strategy
// can switch independently of the draw submission (and vice versa).
class IGeometryWorkGenerator {
  public:
    virtual ~IGeometryWorkGenerator() = default;
    // Cull + produce the frame's draw commands (the compute passes). The
    // results land in the command buffers published by the frame inputs.
    virtual void Generate(const GeometryFrameInputs&) = 0;
};

// Geometry render: consumes the generated command stream and submits draws
// (indexed indirect, task/mesh, filtered). This is the CONSUMER half — it
// owns the raster pipeline binding and the draw calls, but never produces a
// command. Separated from IGeometryWorkGenerator so the submission strategy
// can switch independently of the cull.
class IGeometryRenderer {
  public:
    virtual ~IGeometryRenderer() = default;
    virtual void RecordDepth(GeometryPassRecorder&, const GeometryFrameInputs&) = 0;
    virtual void RecordOpaque(GeometryPassRecorder&, const GeometryFrameInputs&) = 0;
    virtual void RecordMasked(GeometryPassRecorder&, const GeometryFrameInputs&) = 0;
};

// One geometry backend pairs a work generator with a renderer and owns their
// shared lifecycle. The two halves are reachable by upcast
// (IGeometryBackend* -> IGeometryWorkGenerator* / IGeometryRenderer*), so a
// composer can hold one backend but hand the producer/consumer halves to the
// phases that need them separately. Never run multiple implementations on the
// same geometry in one pass. Common depth/Hi-Z and shading targets are owned
// by the frame composer.
class IGeometryBackend : public IGeometryWorkGenerator, public IGeometryRenderer {
  public:
    ~IGeometryBackend() override = default;
    [[nodiscard]] virtual GeometryBackendKind Kind() const noexcept = 0;
    virtual bool Initialize(GeometryBackendResources&) = 0;
    virtual void Shutdown(GeometryBackendResources&) noexcept = 0;
};

} // namespace engine
