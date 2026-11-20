// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Concrete GPU-scene indexed-indirect backend: the first live implementation
// of IGeometryBackend (GeometryBackend.h). It owns the persistent indirect
// command stream + work scratch buffer and records the GPU-produced
// VkDrawIndexedIndirectCommand stream against the composer's command buffer.
//
// This is the "backend adapter" step of docs/modular_world_renderer_proposal.md:
// it wraps the already-working indexed path behind the new contract. Runtime
// wiring into WorldRenderer (and output-parity against the direct path) is the
// follow-up; this class is self-contained and unit-testable.
#include <Engine/Memory/pool.h>
#include <Engine/Renderer/GeometryBackendTypes.h>

namespace engine {

class GpuSceneIndexedBackend final : public IGeometryBackend {
  public:
    GeometryBackendKind Kind() const noexcept override {
        return GeometryBackendKind::GpuSceneIndexed;
    }
    bool Initialize(GeometryBackendResources& resources) override;
    void Shutdown(GeometryBackendResources& resources) noexcept override;
    void Prepare(const GeometryFrameInputs& inputs) override;
    void RecordDepth(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;
    void RecordOpaque(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;
    void RecordMasked(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;

  private:
    // Backend-owned buffers (RAII holders auto-release on destruction and on
    // Shutdown). The raster program twins are filled by the live integration
    // (program compilation needs the shader library); they default to empty.
    smvk::Holder<smvk::BufferHandle> work_;
    smvk::Holder<smvk::BufferHandle> indirect_;
    smvk::ShaderProgram rasterProgram_;
    smvk::ShaderProgram depthProgram_;
    uint32_t commandCount_ = 0; // reserved draw-command count (from Prepare)
    uint32_t frameIndex_ = 0;
};

} // namespace engine
