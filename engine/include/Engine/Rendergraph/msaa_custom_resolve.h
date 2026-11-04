// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Opt-in programmable HDR resolve for the Main 2x multisampled color.
// The baseline matches hardware AVERAGE; no edge heuristic is enabled.
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Rendergraph/ShaderSwap.h>
#include <Engine/RHI/resource_manager.h>
#include <cstdint>
#include <vector>

namespace smvk {
struct MsaaCustomResolve {
    Holder<ShaderModuleHandle> vs, ps;
    ShaderProgram program{};
    static constexpr const char* kVs = "content/shaders/tonemap.vert.hlsl.spv";
    static constexpr const char* kPs = "content/shaders/msaa_custom_resolve.frag.hlsl.spv";

    bool Valid() const { return program.shaderCount == 2; }
    bool Ensure(ResourceManager& ctx) {
        if (Valid()) return true;
        try {
            if (vs.empty()) vs = ctx.CreateShaderModule(readFile(kVs), "msaaResolveVert");
            if (ps.empty()) ps = ctx.CreateShaderModule(readFile(kPs), "msaaResolveFrag");
        } catch (...) { return false; }
        if (vs.empty() || ps.empty()) return false;
        Shader v{ .module = vs, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
        Shader f{ .module = ps, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
        program = { .shaders = { v, f }, .shaderCount = 2 };
        return true;
    }
    bool ReloadShaders(ResourceManager& ctx, std::vector<ShaderModuleHandle>& retired) {
        bool changed = false;
        changed |= SwapShaderModule(ctx, kVs, "msaaResolveVert", vs, retired);
        changed |= SwapShaderModule(ctx, kPs, "msaaResolveFrag", ps, retired);
        if (changed) { program = {}; Ensure(ctx); }
        return changed;
    }
    void AddPass(RenderGraph& rg, LogicalImageViewHandle src,
        LogicalImageViewHandle dst, uint32_t bindlessIndex, Extent2D extent) const {
        struct Push { uint32_t colorIndex; } push{ bindlessIndex };
        auto& pass = rg.AddPass("MsaaCustomResolve");
        pass.SetOffset(0, 0).SetExtent(extent).SetInputImages({src})
            .SetColor({dst}, AttachmentLoadOp::DontCare, AttachmentStoreOp::Store);
        ShaderProgram prog = program;
        pass.SetRecordingFunc([prog, push](CommandBuffer& cmd) mutable {
            cmd.BindRenderPipeline(prog, VertexLayoutId::Empty,
                DepthSettings::Disabled(), StencilSettings::DefaultDisabled(),
                BlendSettings::Opaque(), MSAASettings::Count1(),
                RasterSettings{ .cullMode = CullMode::None });
            cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
            cmd.Draw(3, 1, 0, 0);
        });
    }
};
} // namespace smvk
