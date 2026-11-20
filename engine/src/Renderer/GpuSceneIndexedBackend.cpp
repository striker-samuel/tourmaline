// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/GpuSceneIndexedBackend.h>

#include <Engine/Memory/pool.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/renderer.h>

namespace engine {

namespace {
// VkDrawIndexedIndirectCommand is 5 uint32 words (indexCount, instanceCount,
// firstIndex, vertexOffset, firstInstance) = 20 bytes. Kept as a local
// constant so this backend does not need the Vulkan header directly.
constexpr uint32_t kIndexedIndirectStride = 20u;
// Conservative default reservation: 4 PSO bins x up to 65535 commands, the
// engine's per-call partition convention. The live integration re-derives
// this from the frame's candidate bound.
constexpr uint32_t kDefaultCommandCapacity = 4u * 65535u;
} // namespace

bool GpuSceneIndexedBackend::Initialize(GeometryBackendResources& resources) {
    if (!resources.ctx)
        return false;
    smvk::ResourceManager& ctx = *resources.ctx;

    // The GPU-produced VkDrawIndexedIndirectCommand stream (indirect usage:
    // the raster build compute writes it, the draw reads it).
    if (indirect_.empty()) {
        indirect_ = ctx.CreateBuffer(size_t(kDefaultCommandCapacity) * kIndexedIndirectStride,
                                     smvk::BufferType_Indirect,
                                     "GeometryBackendIndexedIndirect");
        if (indirect_.empty())
            return false;
    }
    // Per-work-row scratch for the producer (reserved stride mirror).
    if (work_.empty()) {
        work_ = ctx.CreateBuffer(size_t(kDefaultCommandCapacity) * sizeof(uint32_t),
                                 smvk::BufferType_Storage,
                                 "GeometryBackendIndexedWork");
        if (work_.empty())
            return false;
    }
    commandCount_ = kDefaultCommandCapacity;
    return true;
}

void GpuSceneIndexedBackend::Shutdown(GeometryBackendResources&) noexcept {
    work_.reset();
    indirect_.reset();
    commandCount_ = 0;
    frameIndex_ = 0;
}

void GpuSceneIndexedBackend::Prepare(const GeometryFrameInputs& inputs) {
    // Copy the per-frame counts; the composer owns the addresses for this
    // flight. commandCount_ is the reserved bound, never the GPU survivor
    // count (that is read by the device via the indirect stream).
    if (inputs.frame)
        frameIndex_ = inputs.frame->frameIndex;
    if (inputs.candidateCount != 0)
        commandCount_ = inputs.candidateCount;
}

void GpuSceneIndexedBackend::RecordDepth(GeometryPassRecorder& recorder,
                                         const GeometryFrameInputs& inputs) {
    (void)inputs;
    if (!recorder.cmd || !recorder.depthOnly)
        return;
    if (depthProgram_.shaderCount == 0 || indirect_.empty() || commandCount_ == 0)
        return;
    // Depth prepass: the same indexed indirect stream, through the depth
    // program. Masked/opaque split and per-bin offsets are the composer's
    // scheduling decision; the backend records the canonical submission.
    recorder.cmd->BindRenderPipeline(depthProgram_);
    recorder.cmd->DrawIndexedIndirect(indirect_, 0, commandCount_, kIndexedIndirectStride);
}

void GpuSceneIndexedBackend::RecordOpaque(GeometryPassRecorder& recorder,
                                          const GeometryFrameInputs& inputs) {
    (void)inputs;
    if (!recorder.cmd || recorder.depthOnly)
        return;
    if (rasterProgram_.shaderCount == 0 || indirect_.empty() || commandCount_ == 0)
        return;
    recorder.cmd->BindRenderPipeline(rasterProgram_);
    recorder.cmd->DrawIndexedIndirect(indirect_, 0, commandCount_, kIndexedIndirectStride);
}

void GpuSceneIndexedBackend::RecordMasked(GeometryPassRecorder& recorder,
                                          const GeometryFrameInputs& inputs) {
    // Masked (alpha-tested) submission reuses the opaque indexed stream with
    // the masked program/A2C variant. The live integration selects the A2C
    // twin; this self-contained backend records the same draw shape.
    RecordOpaque(recorder, inputs);
}

} // namespace engine
