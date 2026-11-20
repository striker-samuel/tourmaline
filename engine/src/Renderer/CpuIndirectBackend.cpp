// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/CpuIndirectBackend.h>

#include <Engine/Memory/pool.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/renderer.h>

namespace engine {

CpuIndirectBackend::~CpuIndirectBackend() {
    delete[] commands_;
    commands_ = nullptr;
}

bool CpuIndirectBackend::Initialize(GeometryBackendResources& resources) {
    if (!resources.ctx)
        return false;
    ctx_ = resources.ctx;
    smvk::ResourceManager& ctx = *resources.ctx;
    if (!commands_) {
        commands_ = new DrawCommand[kMaxCommands];
        if (!commands_)
            return false;
    }
    if (indirect_.empty()) {
        indirect_ = ctx.CreateBuffer(size_t(kMaxCommands) * sizeof(DrawCommand),
                                     smvk::BufferType_Indirect,
                                     "GeometryBackendCpuIndirect");
        if (indirect_.empty())
            return false;
    }
    return true;
}

void CpuIndirectBackend::Shutdown(GeometryBackendResources&) noexcept {
    commandCount_ = 0;
    ctx_ = nullptr;
    delete[] commands_;
    commands_ = nullptr;
    indirect_.reset();
}

void CpuIndirectBackend::Prepare(const GeometryFrameInputs& inputs) {
    // Canonical geometry publication supplies the per-mesh draw list; until
    // then this backend records a single conservative full-scene command so
    // the path is exercised end-to-end without a mesh table.
    commandCount_ = 0;
    if (inputs.candidateCount == 0)
        return;
    DrawCommand cmd{};
    cmd.indexCount = 0; // populated by the publisher (mesh triangle count * 3)
    cmd.instanceCount = inputs.candidateCount;
    cmd.firstIndex = 0;
    cmd.vertexOffset = 0;
    cmd.firstInstance = 0;
    commands_[0] = cmd;
    commandCount_ = 1;
}

void CpuIndirectBackend::RecordDepth(GeometryPassRecorder& recorder,
                                     const GeometryFrameInputs& inputs) {
    (void)inputs;
    if (!recorder.cmd || !recorder.depthOnly)
        return;
    if (depthProgram_.shaderCount == 0 || commandCount_ == 0 || indirect_.empty())
        return;
    recorder.cmd->BindRenderPipeline(depthProgram_);
    recorder.cmd->DrawIndexedIndirect(indirect_, 0, commandCount_, sizeof(DrawCommand));
}

void CpuIndirectBackend::RecordOpaque(GeometryPassRecorder& recorder,
                                      const GeometryFrameInputs& inputs) {
    (void)inputs;
    if (!recorder.cmd || recorder.depthOnly)
        return;
    if (rasterProgram_.shaderCount == 0 || commandCount_ == 0 || indirect_.empty() || !ctx_)
        return;
    // Upload the CPU-authored command stream just-in-time for this pass.
    ctx_->UploadDataToBuffer(indirect_,
                             commands_,
                             size_t(commandCount_) * sizeof(DrawCommand),
                             smvk::PipelineStageBit_DrawIndirect,
                             smvk::AccessFlags::IndirectCommandRead);
    recorder.cmd->BindRenderPipeline(rasterProgram_);
    recorder.cmd->DrawIndexedIndirect(indirect_, 0, commandCount_, sizeof(DrawCommand));
}

void CpuIndirectBackend::RecordMasked(GeometryPassRecorder& recorder,
                                      const GeometryFrameInputs& inputs) {
    RecordOpaque(recorder, inputs);
}

} // namespace engine
