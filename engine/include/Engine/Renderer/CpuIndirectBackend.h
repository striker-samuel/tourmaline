// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// CPU-recorded indirect backend: the SECOND live IGeometryBackend, proving the
// contract supports swappability (proposal step 5). Unlike the GPU-scene
// indexed backend (whose draw commands are produced by a compute cull), this
// backend generates VkDrawIndexedIndirectCommand entries ON THE CPU from the
// canonical draw list and uploads them to an indirect buffer each frame.
//
// "CPU-recorded" means CPU-generated/uploaded indirect commands, NOT software
// rasterization. The actual per-mesh draw list (index ranges, instance counts)
// is supplied by the canonical geometry publication (a separate proposal step);
// this class owns the command buffer, upload and draw recording.
#include <Engine/Memory/pool.h>
#include <Engine/Renderer/GeometryBackendTypes.h>

#include <cstdint>

namespace engine {

class CpuIndirectBackend final : public IGeometryBackend {
  public:
    CpuIndirectBackend() = default;
    ~CpuIndirectBackend() override;
    CpuIndirectBackend(const CpuIndirectBackend&) = delete;
    CpuIndirectBackend& operator=(const CpuIndirectBackend&) = delete;

    GeometryBackendKind Kind() const noexcept override {
        return GeometryBackendKind::CpuIndirect;
    }
    bool Initialize(GeometryBackendResources& resources) override;
    void Shutdown(GeometryBackendResources& resources) noexcept override;
    void Generate(const GeometryFrameInputs& inputs) override;
    void RecordDepth(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;
    void RecordOpaque(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;
    void RecordMasked(GeometryPassRecorder& recorder, const GeometryFrameInputs& inputs) override;

  private:
    // Canonical VkDrawIndexedIndirectCommand (5 uint32 words), CPU-authored.
    struct DrawCommand {
        uint32_t indexCount = 0;
        uint32_t instanceCount = 1;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t firstInstance = 0;
    };
    // Fixed-capacity scratch: one command per admitted instance bound. Sized
    // to the engine's per-call partition convention (65535). Heap-owned raw
    // array (no std::vector: the renderer ABI stays container-free), allocated
    // in Initialize.
    static constexpr uint32_t kMaxCommands = 65535u;

    DrawCommand* commands_ = nullptr; // CPU-authored this frame (owned)
    uint32_t commandCount_ = 0;
    smvk::ResourceManager* ctx_ = nullptr;      // retained from Initialize (upload)
    smvk::Holder<smvk::BufferHandle> indirect_; // uploaded indirect buffer
    smvk::ShaderProgram rasterProgram_;
    smvk::ShaderProgram depthProgram_;
};

} // namespace engine
