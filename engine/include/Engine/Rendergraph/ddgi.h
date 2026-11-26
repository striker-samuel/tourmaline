// SPDX-License-Identifier: MIT
#pragma once
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/ddgi_types.h>
#include <Engine/Rendergraph/renderer.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Utils/shader_library.h>

namespace smvk::ddgi {

// Explicit ownership; call Destroy only at a graph/resource retirement
// boundary. The caller must establish cross-frame history and TLAS ordering.
// SDF callers additionally own coverage, upload synchronization and residency.
// WorldRenderer keeps selecting hardware RT until those are wired.
struct Volume {
    struct Buffer {
        Holder<BufferHandle> storage;
        LogicalBufferHandle logical{};
        uint64_t address = 0;
        bool Create(ResourceManager& ctx, RenderGraph& rg, size_t bytes, const char* name) {
            storage = ctx.CreateBuffer(bytes, BufferType_Storage, name);
            if (storage.empty())
                return false;
            address = ctx.GetBufferDeviceAddress(storage);
            if (!address)
                return false;
            logical = rg.AddExternalBuffer(storage, name);
            return !logical.empty();
        }
        void Destroy(RenderGraph& rg) {
            if (!logical.empty())
                rg.FreeBuffer(logical);
            logical = {};
            storage = {};
            address = 0;
        }
    };
    struct History {
        Buffer irradiance, distance;
    };
    Grid grid{};
    Sizes sizes{};
    Buffer scratch;
    History history[2];
    Holder<ShaderModuleHandle> traceModule, blendModule;
    ShaderProgram traceProgram{}, blendProgram{};
    bool ready = false;
    TraceBackend backend = TraceBackend::HardwareRT;

    // Header, instances, nodes, children, leaves, materials. The caller
    // guarantees coverage of primary and shadow rays and stable GPU lifetime.
    struct SdfTraceInput {
        uint64_t sceneAddress = 0;
        LogicalBufferHandle reads[6]{};
    };

    bool Create(ResourceManager& ctx,
                RenderGraph& rg,
                const Grid& desc,
                size_t byteBudget,
                TraceBackend selected = TraceBackend::HardwareRT) {
        // Reconfiguration requires the owner to retire previous GPU uses.
        if (ready ||
            (selected != TraceBackend::HardwareRT && selected != TraceBackend::StaticSdf) ||
            (selected == TraceBackend::HardwareRT && !ctx.SupportsRayQueries()) ||
            !CalculateSizes(desc, byteBudget, sizes))
            return false;
        grid = desc;
        backend = selected;
        try {
            traceModule = ctx.CreateShaderModule(
                smvk::shaders::Read(backend == TraceBackend::StaticSdf
                                        ? "content/shaders/ddgi_trace_sdf.cs.hlsl.spv"
                                        : "content/shaders/ddgi_trace.cs.hlsl.spv"),
                "ddgiTrace");
            blendModule = ctx.CreateShaderModule(
                smvk::shaders::Read("content/shaders/ddgi_blend.cs.hlsl.spv"), "ddgiBlend");
            if (!traceModule.empty() && !blendModule.empty() &&
                scratch.Create(ctx, rg, sizes.scratch, "ddgi ray scratch") &&
                history[0].irradiance.Create(ctx, rg, sizes.irradiance, "ddgi irradiance A") &&
                history[0].distance.Create(ctx, rg, sizes.distance, "ddgi moments A") &&
                history[1].irradiance.Create(ctx, rg, sizes.irradiance, "ddgi irradiance B") &&
                history[1].distance.Create(ctx, rg, sizes.distance, "ddgi moments B")) {
                Shader trace{
                    .module = traceModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute};
                Shader blend{
                    .module = blendModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute};
                traceProgram = {.shaders = {trace}, .shaderCount = 1};
                blendProgram = {.shaders = {blend}, .shaderCount = 1};
                ready = true;
                return true;
            }
        } catch (...) {
            // Fail closed; preserve the existing GI path.
        }
        Destroy(rg);
        return false;
    }

    void Destroy(RenderGraph& rg) {
        ready = false;
        scratch.Destroy(rg);
        for (auto& h : history) {
            h.irradiance.Destroy(rg);
            h.distance.Destroy(rg);
        }
        traceProgram = ShaderProgram();
        blendProgram = ShaderProgram();
        traceModule = {};
        blendModule = {};
        sizes = {};
    }

    template <class Push> struct Frame {
        ShaderProgram* program = nullptr;
        Push push{};
        uint32_t groupsX = 0, groupsY = 0;
    };
    template <class Push>
    static void Record(CommandBuffer& cmd, const FrameContext& fc, const PassData& pd) {
        const auto& f = fc.rg->FrameData().Get<Frame<Push>>(pd.frameSlot);
        cmd.BindComputePipeline(*f.program);
        cmd.PushConstants(0, sizeof(Push), &f.push, ShaderStage::Compute);
        cmd.Dispatch(f.groupsX, f.groupsY, 1);
    }
    static void Hint(RenderPass& pass, const Buffer& b, bool write, bool fragment = false) {
        BufferSyncRequest req{};
        req.barrierMasks.accessMask = write ? AccessFlags::ShaderWrite : AccessFlags::ShaderRead;
        req.barrierMasks.stageMask =
            fragment ? PipelineStageBit_Fragment : PipelineStageBit_Compute;
        pass.AddBufferHint(b.logical, req);
    }

    // reset=true on first dispatch and whenever prior history is invalid.
    // Caller selects writeSlot only after prior readers have retired and
    // ensures prior producer completion; this is NOT a flight-index ring.
    bool AddPasses(RenderGraph& rg,
                   uint64_t frameAddress,
                   uint32_t writeSlot,
                   uint32_t updateIndex,
                   bool reset,
                   const SdfTraceInput* sdf = nullptr) const {
        if (!ready || !frameAddress || writeSlot > 1)
            return false;
        if (backend == TraceBackend::StaticSdf) {
            if (!sdf || !sdf->sceneAddress)
                return false;
            for (auto h : sdf->reads)
                if (h.empty())
                    return false;
        }
        const History& old = history[writeSlot ^ 1];
        const History& next = history[writeSlot];
        {
            auto& pass = rg.AddComputePass("DDGITrace");
            Hint(pass, scratch, true);
            // E_indirect feedback: the trace reads the previous frame's field
            // (and its distance moments) to gather multi-bounce color at hits.
            Hint(pass, old.irradiance, false);
            Hint(pass, old.distance, false);
            const TracePush push{frameAddress,
                                 scratch.address,
                                 grid.origin[0],
                                 grid.origin[1],
                                 grid.origin[2],
                                 grid.spacing,
                                 grid.dims[0],
                                 grid.dims[1],
                                 grid.dims[2],
                                 grid.rays,
                                 grid.maxDistance,
                                 updateIndex,
                                 old.irradiance.address,
                                 old.distance.address};
            if (backend == TraceBackend::StaticSdf) {
                BufferSyncRequest req{};
                req.barrierMasks.accessMask = AccessFlags::ShaderRead;
                req.barrierMasks.stageMask = PipelineStageBit_Compute;
                for (auto h : sdf->reads)
                    pass.AddBufferHint(h, req);
                auto& f = rg.PassFrameData<Frame<SdfTracePush>>(pass);
                f.program = const_cast<ShaderProgram*>(&traceProgram);
                f.push = {push, sdf->sceneAddress};
                f.groupsX = (grid.rays + 63) / 64;
                f.groupsY = sizes.probes;
                pass.SetRecordFn(&Record<SdfTracePush>);
            } else {
                auto& f = rg.PassFrameData<Frame<TracePush>>(pass);
                f.program = const_cast<ShaderProgram*>(&traceProgram);
                f.push = push;
                f.groupsX = (grid.rays + 63) / 64;
                f.groupsY = sizes.probes;
                pass.SetRecordFn(&Record<TracePush>);
            }
        }
        {
            auto& pass = rg.AddComputePass("DDGIBlend");
            Hint(pass, scratch, false);
            Hint(pass, old.irradiance, false);
            Hint(pass, old.distance, false);
            Hint(pass, next.irradiance, true);
            Hint(pass, next.distance, true);
            auto& f = rg.PassFrameData<Frame<BlendPush>>(pass);
            f.program = const_cast<ShaderProgram*>(&blendProgram);
            f.push = {scratch.address,
                      old.irradiance.address,
                      old.distance.address,
                      next.irradiance.address,
                      next.distance.address,
                      sizes.probes,
                      grid.rays,
                      uint32_t(reset),
                      grid.hysteresis,
                      grid.maxDistance,
                      0};
            f.groupsX = (kDistanceBins + 63) / 64;
            f.groupsY = sizes.probes;
            pass.SetRecordFn(&Record<BlendPush>);
        }
        return true;
    }

    void AddMainReadHints(RenderPass& pass, uint32_t publishedSlot) const {
        if (!ready || publishedSlot > 1)
            return;
        Hint(pass, history[publishedSlot].irradiance, false, true);
        Hint(pass, history[publishedSlot].distance, false, true);
    }
};

} // namespace smvk::ddgi
