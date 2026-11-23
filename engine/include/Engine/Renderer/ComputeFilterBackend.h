// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Compute-filtered geometry backend: the recovered compute batch-cull + Hi-Z
// filter path. Geometry is culled by a compute shader (batch cull -> seed ->
// remainder against the frame's Hi-Z), and the surviving draws are submitted
// through a filtered vertex/mesh shader. This is the "GeometryFX filtering"
// backend of docs/modular_world_renderer_proposal.md — named ComputeFilter
// because the geometry is filtered by a compute pass, not because it is a
// legacy path.
//
// The retired filter/seed/remainder shaders (gfxbatchcull / gfxfilter /
// gfxfiltered / gfxfilter) must be recovered from the historical revision and
// re-ABI-validated before this backend can run; until then Initialize reports
// unavailable and the registry gates it off. The structure is complete so the
// port fills in shader compilation + the filter dispatch, not a new contract.
#include <Engine/Renderer/GeometryBackendTypes.h>

namespace engine {

class ComputeFilterBackend final : public IGeometryBackend {
  public:
    GeometryBackendKind Kind() const noexcept override {
        return GeometryBackendKind::ComputeFilter;
    }
    bool Initialize(GeometryBackendResources& resources) override;
    void Shutdown(GeometryBackendResources& resources) noexcept override;
    void Generate(const GeometryFrameInputs& inputs) override;
    void RecordDepth(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;
    void RecordOpaque(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;
    void RecordMasked(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;
};

} // namespace engine
