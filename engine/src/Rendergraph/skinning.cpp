// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU SKINNING pass (the Northlight compute box; see
// Rendergraph/skinning.h + gfxskinning.cs.hlsl for the full contract).

#include <Engine/Rendergraph/skinning.h>

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Rendergraph/renderer.h>

#include <cstdio>
#include <fstream>

namespace smvk::skinning {

namespace {

std::vector<char> ReadShader(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
}

// Per-flight slot payload + non-capturing record function for the skinning
// dispatch (static compiled ABI).
struct SkinFrame {
    ShaderProgram* program = nullptr;
    SkinPush push{};
    uint32_t dispatchX = 0;
};

void RecordSkinning(CommandBuffer& cmd, const FrameContext& fc, const PassData& pd) {
    const SkinFrame& f = fc.rg->FrameData().Get<SkinFrame>(pd.frameSlot);
    cmd.BindComputePipeline(*f.program);
    cmd.PushConstants(0, sizeof(f.push), &f.push, ShaderStage::Compute);
    cmd.Dispatch(f.dispatchX, 1, 1);
    // The deformed heap is BDA-only (the graph has no handle to hint on), so
    // it will not order this dispatch before the draw passes' deformed
    // fetches. Emit the compute->graphics dependency through the RHI seam
    // (per-backend: an all-commands barrier on Vulkan, a global UAV barrier on
    // D3D12); once the deformed span is declared as a graph buffer resource
    // this folds back into the per-resource barrier machinery.
    cmd.EmitGlobalMemoryBarrier();
}

} // namespace

bool SkinVisual::Ensure(ResourceManager& ctx) {
    if (valid)
        return true;
    std::vector<char> code = ReadShader(kSkinningCsPath);
    if (code.empty()) {
        std::fprintf(stderr, "[Skinning] compute shader missing: %s\n", kSkinningCsPath);
        return false;
    }
    csModule = ctx.CreateShaderModule(std::move(code), "skinningCS");
    if (csModule.empty())
        return false;
    program = {.shaders = {Shader{
                   .module = csModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute}},
               .shaderCount = 1};
    valid = true;
    return true;
}

void SkinVisual::Destroy() {
    for (auto& b : paletteBuffers)
        b = {};
    csModule = {};
    program = ShaderProgram();
    valid = false;
}

bool SkinVisual::ReloadShaders(ResourceManager& ctx, std::vector<ShaderModuleHandle>& retired) {
    if (!valid)
        return false; // nothing standing: the next Ensure builds fresh
    if (!SwapShaderModule(ctx, kSkinningCsPath, "skinningCS", csModule, retired))
        return false;
    program = ShaderProgram();
    if (!csModule.empty()) {
        program = {.shaders = {Shader{
                       .module = csModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute}},
                   .shaderCount = 1};
    }
    return true;
}

bool SkinVisual::AppendPass(ResourceManager& ctx,
                            RenderGraph& rg,
                            uint32_t frameIndex,
                            const SkinPush& push,
                            std::span<const glm::mat4> palette) {
    // Fail closed on oversized palettes/ranges: the palette buffer is sized
    // kSkinPaletteMax mat4s and the shader's ranges table is SKIN_RANGES_MAX
    // rows, so a caller-provided count above either cap would overrun the
    // destination upload / the declared BDA bounds. The caller already clamps
    // paletteCount, but this seam is the one audited gate (never rely on the
    // caller for the allocation bound).
    if (!valid || push.paletteCount == 0 || push.paletteCount > kSkinPaletteMax ||
        push.rangeCount == 0 || push.rangeCount > kSkinRangesMax || push.skin == 0 ||
        push.ranges == 0 || push.deformed == 0 || palette.size() < push.paletteCount)
        return false;

    // This flight's palette buffer (lazily created).
    const uint32_t f = frameIndex % smvk::limits::MaxFramesInFlight;
    if (paletteBuffers[f].empty()) {
        paletteBuffers[f] = ctx.CreateBuffer(
            kSkinPaletteMax * sizeof(glm::mat4), BufferType_Storage, "skinning palette");
        if (paletteBuffers[f].empty()) {
            std::fprintf(stderr, "[Skinning] palette buffer alloc failed\n");
            return false;
        }
    }

    // The palette upload (the CPU bone-eval box's output - the joint
    // matrices the cluster's SampleAnimation built this frame) lands in
    // this flight's buffer; the push reads it back by BDA.
    ctx.UploadDataToBuffer(paletteBuffers[f],
                           palette.data(),
                           push.paletteCount * sizeof(glm::mat4),
                           PipelineStage(PipelineStageBit_Compute),
                           AccessFlags::ShaderRead,
                           0,
                           0);
    SkinPush p = push;
    p.palette = ctx.GetBufferDeviceAddress(paletteBuffers[f]);
    // The bind-pose heap BDAs (the pass's input side; the same static
    // heaps every draw path reads - the RHI globals from vulcan_impl.h;
    // they stay the bind pose FOREVER, the pass only READS them).
    p.posHeap = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticVertexPosBuffer);
    p.shadeHeap = ctx.GetBufferDeviceAddress(staticVertexFmtHeap.m_staticVertexShadingBuffer);

    RenderPass& pass = rg.AddComputePass("GPUSkinning");
    SkinFrame& frame = rg.PassFrameData<SkinFrame>(pass);
    frame.program = &program;
    frame.push = p;
    frame.dispatchX = push.rangeCount;
    pass.SetRecordFn(&RecordSkinning);
    return true;
}

} // namespace smvk::skinning
