// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <vector>

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
    Buffer relocation; // per-probe float4 offset (SDF-assisted relocation)
    Holder<ShaderModuleHandle> traceModule, blendModule, relocateModule;
    ShaderProgram traceProgram{}, blendProgram{}, relocateProgram{};
    bool ready = false;
    TraceBackend backend = TraceBackend::HardwareRT;
    // Camera-following scroll (probe units): the grid origin snaps to the
    // camera each frame. scroll is the integer grid coord of the volume's
    // min corner (origin = scroll * spacing); scrollPrev drives the toroidal
    // history carry (the blend reads old history at coord + (scroll-scrollPrev)).
    int32_t scroll[3] = {0, 0, 0};
    int32_t scrollPrev[3] = {0, 0, 0};
    // Set by the owner when the grid shape (spacing) changed so the blend
    // must reset this volume's history (the old probes are at different
    // world positions). Cleared after AddPasses consumes it.
    bool resetPending = false;

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
            if (backend == TraceBackend::HardwareRT)
                relocateModule = ctx.CreateShaderModule(
                    smvk::shaders::Read("content/shaders/ddgi_relocate.cs.hlsl.spv"),
                    "ddgiRelocate");
            if (!traceModule.empty() && !blendModule.empty() &&
                (backend != TraceBackend::HardwareRT || !relocateModule.empty()) &&
                scratch.Create(ctx, rg, sizes.scratch, "ddgi ray scratch") &&
                relocation.Create(ctx, rg, sizes.probes * sizeof(float) * 4, "ddgi relocation") &&
                history[0].irradiance.Create(ctx, rg, sizes.irradiance, "ddgi irradiance A") &&
                history[0].distance.Create(ctx, rg, sizes.distance, "ddgi moments A") &&
                history[1].irradiance.Create(ctx, rg, sizes.irradiance, "ddgi irradiance B") &&
                history[1].distance.Create(ctx, rg, sizes.distance, "ddgi moments B")) {
                // Zero the history: Vulkan buffer memory is UNINITIALIZED, and
                // the trace pass's E_indirect feedback reads history[slot^1]
                // on the FIRST frame (before any blend). The blend's reset only
                // guards its own read; the trace would otherwise sample garbage
                // and flash frame-to-frame. Zeroing makes the field read as
                // invalid (irradiance .w == 0) + zero distance, so E_indirect
                // contributes nothing until the first real blend lands.
                auto zero = [&](const Buffer& b, size_t bytes) {
                    const std::vector<uint8_t> zeros(bytes, 0);
                    ctx.UploadDataToBuffer(b.storage,
                                           zeros.data(),
                                           zeros.size(),
                                           PipelineStageBit_Compute,
                                           AccessFlags::ShaderRead);
                };
                zero(history[0].irradiance, sizes.irradiance);
                zero(history[0].distance, sizes.distance);
                zero(history[1].irradiance, sizes.irradiance);
                zero(history[1].distance, sizes.distance);
                if (backend == TraceBackend::StaticSdf)
                    zero(relocation, sizes.probes * sizeof(float) * 4);
                Shader trace{
                    .module = traceModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute};
                Shader blend{
                    .module = blendModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute};
                Shader relocate{.module = relocateModule,
                                .entryPoint = "mainCS",
                                .stage = ShaderStage::Compute};
                traceProgram = {.shaders = {trace}, .shaderCount = 1};
                blendProgram = {.shaders = {blend}, .shaderCount = 1};
                if (backend == TraceBackend::HardwareRT)
                    relocateProgram = {.shaders = {relocate}, .shaderCount = 1};
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
        relocation.Destroy(rg);
        for (auto& h : history) {
            h.irradiance.Destroy(rg);
            h.distance.Destroy(rg);
        }
        traceProgram = ShaderProgram();
        blendProgram = ShaderProgram();
        relocateProgram = ShaderProgram();
        traceModule = {};
        blendModule = {};
        relocateModule = {};
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
                   uint64_t rayMaterials = 0,
                   uint64_t positions = 0,
                   uint64_t indices = 0,
                   const SdfTraceInput* sdf = nullptr) const {
        if (!ready || !frameAddress || writeSlot > 1)
            return false;
        if (backend == TraceBackend::StaticSdf) {
            // The caller guarantees a resident, covered scene via sceneAddress.
            // reads[] is OPTIONAL (the StaticSdfGpuScene residency follows the
            // stable-upload model like SSR — no per-frame hints), so only
            // non-empty handles are hinted below.
            if (!sdf || !sdf->sceneAddress)
                return false;
        }
        const History& old = history[writeSlot ^ 1];
        const History& next = history[writeSlot];
        const int32_t delta[3] = {
            scroll[0] - scrollPrev[0],
            scroll[1] - scrollPrev[1],
            scroll[2] - scrollPrev[2],
        };
        if (backend == TraceBackend::HardwareRT) {
            // TLAS probe relocation: push buried probes into free space so
            // the surfaces they serve stop reading black. Runs once per
            // volume, before the trace (no SDF dependency — six axis rays).
            auto& pass = rg.AddComputePass("DDGIRelocate");
            Hint(pass, relocation, true);
            auto& f = rg.PassFrameData<Frame<RelocatePush>>(pass);
            f.program = const_cast<ShaderProgram*>(&relocateProgram);
            f.push = {relocation.address,
                      grid.origin[0],
                      grid.origin[1],
                      grid.origin[2],
                      grid.spacing,
                      grid.dims[0],
                      grid.dims[1],
                      grid.dims[2],
                      sizes.probes,
                      grid.maxDistance,
                      0u};
            f.groupsX = (sizes.probes + 63) / 64;
            f.groupsY = 1;
            pass.SetRecordFn(&Record<RelocatePush>);
        }
        {
            auto& pass = rg.AddComputePass("DDGITrace");
            Hint(pass, scratch, true);
            Hint(pass, relocation, false);
            // E_indirect feedback: the trace reads the previous frame's field
            // (and its distance moments) to gather multi-bounce color at hits.
            Hint(pass, old.irradiance, false);
            Hint(pass, old.distance, false);
            const float oldOrigin[3] = {
                float(scrollPrev[0]) * grid.spacing,
                float(scrollPrev[1]) * grid.spacing,
                float(scrollPrev[2]) * grid.spacing,
            };
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
                                 old.distance.address,
                                 rayMaterials,
                                 relocation.address,
                                 oldOrigin[0],
                                 oldOrigin[1],
                                 oldOrigin[2],
                                 delta[0],
                                 delta[1],
                                 delta[2],
                                 uint32_t(reset),
                                 0u,
                                 positions,
                                 indices};
            if (backend == TraceBackend::StaticSdf) {
                BufferSyncRequest req{};
                req.barrierMasks.accessMask = AccessFlags::ShaderRead;
                req.barrierMasks.stageMask = PipelineStageBit_Compute;
                for (auto h : sdf->reads)
                    if (!h.empty())
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
            Hint(pass, relocation, false);
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
                      delta[0],
                      delta[1],
                      delta[2],
                      grid.dims[0],
                      grid.dims[1],
                      grid.dims[2],
                      updateIndex,
                      relocation.address};
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
