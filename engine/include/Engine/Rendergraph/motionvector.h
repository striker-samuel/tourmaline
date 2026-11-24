// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Motion vector target + pass (per-pixel prevUV - currUV, RG16F).
//
// Ports the HdrTarget (target lifecycle) + Tonemapper (program ownership)
// patterns into one place. The pass re-rasterizes opaque scene geometry
// depth-tested READ-ONLY against the 1x depth-prepass buffer (reversed-Z
// GreaterEqual, no writes), so it works identically in MSAA and non-MSAA
// modes – Main's transient 2x depth is never touched.
//
// Caller sets the recording func (bind program + draw + per-draw push
// constants: model + prevModel), same contract as AddCascadePass.
//
// Usage (app/src/main.cpp):
//   smvk::MotionVectors motionVectors;
//   motionVectors.Ensure(ctx, rg, renderExtent);   // target + programs
//   motionVectors.AddMotionVectorPass(rg, depthView, extent, "MotionVectors")
//       .SetRecordingFunc(...);
//   ... motionVectors.target.logicalView / .bindlessIndex ...

#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/vertex_layouts.h>
#include <Engine/Rendergraph/ShaderSwap.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/platform.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

struct MotionVectorDesc {
    Format format = Format::RG16_Float; // 2x f16 UV delta, plenty for |v| < 1
    const char* name = "motion";
    const char* viewName = "motionView";
    const char* logicalName = "motionLogical";
};

struct MotionVectorTarget {
    Holder<ImageHandle> image;
    Holder<ImageViewHandle> view;
    Holder<BindlessImageHandle> bindless;
    uint32_t bindlessIndex = UINT32_MAX;
    // The STORAGE bindless (General layout) for the camera-motion compute
    // write (RWTexture2D). Dual-visible with `bindless` above (which stays
    // ShaderReadOnlyOptimal for the sampled temporal consumers) — the
    // contact-shadow pattern: one image, a sampling descriptor + a storage
    // descriptor.
    Holder<BindlessImageHandle> storageBindless;
    uint32_t storageBindlessIndex = UINT32_MAX;
    LogicalImageViewHandle logicalView;
    Extent2D extent{0, 0};
    Format format = Format::RG16_Float;

    [[nodiscard]] bool Valid() const {
        return !image.empty();
    }

    bool Create(ResourceManager& ctx,
                RenderGraph& rg,
                Extent2D targetExtent,
                const MotionVectorDesc& d = {}) {
        if (targetExtent.width == 0 || targetExtent.height == 0)
            return false;
        Destroy(rg);

        ImageCreateInfo ci{};
        ci.extent = {targetExtent.width, targetExtent.height, 1};
        ci.format = d.format;
        ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::Storage;
        // TransferSrc: the TOURMALINE_MOTION_DUMP one-shot readback (armed
        // dumps only — the normal path keeps the leanest usage set).
        if (DebugImageDumpArmed())
            ci.usage |= ImageUsage::TransferSrc;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.name = d.name;

        Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
        if (newImage.empty())
            return false;

        ImageViewCreateInfo vci{};
        vci.viewType = ImageViewType::View2D;
        vci.format = Format::Undefined; // inherit
        vci.subresourceRange = {ImageAspect::Color, 0, 1, 0, 1};
        vci.name = d.viewName;

        Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
        if (newView.empty())
            return false;

        Holder<BindlessImageHandle> newBindless =
            ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
        if (newBindless.empty())
            return false;
        const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);
        // The storage twin (General) for the camera-motion compute write.
        Holder<BindlessImageHandle> newStorageBindless =
            ctx.CreateBindlessImage(newView, ImageLayout::General);
        if (newStorageBindless.empty())
            return false;
        const uint32_t newStorageBindlessIndex = ctx.GetBindlessIndex(newStorageBindless);

        LogicalImageViewHandle newLogical =
            rg.AddExternalImageView(newView, d.format, d.logicalName);
        if (newLogical.empty())
            return false;

        image = std::move(newImage);
        view = std::move(newView);
        bindless = std::move(newBindless);
        bindlessIndex = newBindlessIndex;
        storageBindless = std::move(newStorageBindless);
        storageBindlessIndex = newStorageBindlessIndex;
        logicalView = newLogical;
        extent = targetExtent;
        format = d.format;
        ctx.GetOrCreateSampler(SamplerCreateInfo{});
        return true;
    }

    void Destroy(RenderGraph& rg) {
        if (!logicalView.empty())
            rg.FreeImageView(logicalView);
        logicalView = {};
        bindlessIndex = UINT32_MAX;
        bindless = {};
        storageBindlessIndex = UINT32_MAX;
        storageBindless = {};
        view = {};
        image = {};
        extent = {0, 0};
    }

    bool Ensure(ResourceManager& ctx,
                RenderGraph& rg,
                Extent2D targetExtent,
                const MotionVectorDesc& d = {}) {
        if (Valid() && extent.width == targetExtent.width && extent.height == targetExtent.height &&
            format == d.format) {
            return true;
        }
        return Create(ctx, rg, targetExtent, d);
    }
};

// Default SPVs (::tour::vertex_layouts::Static VS + motion PS).
inline constexpr const char* kMotionVectorVsPath = "content/shaders/motionvector.vert.hlsl.spv";
inline constexpr const char* kMotionVectorPsPath = "content/shaders/motionvector.frag.hlsl.spv";

// Camera-only motion reconstruction (Phase 3): mirrors
// gpu_scene_camera_motion.cs.hlsl CameraMotionPush. uint64/float4x4 members
// carry the natural 16-byte alignment; the HLSL struct packs to 168 bytes —
// the 8 bytes of trailing C++ padding are simply never read.
inline constexpr const char* kCameraMotionCsPath = "content/shaders/gpu_scene_camera_motion.cs.hlsl.spv";
struct CameraMotionPush {
    glm::mat4 invRelativeViewProj{1.0f};   // current inverse rebased view-proj
    glm::mat4 prevRelativeViewProj{1.0f};  // previous rebased view-proj
    glm::vec4 originDelta{0.0f, 0.0f, 0.0f, 0.0f}; // xyz = curCameraPos - prevCameraPos
    uint32_t depthTextureIndex = UINT32_MAX;        // full-res opaque depth (prepass)
    uint32_t outImageIndex = UINT32_MAX;            // motion target storage bindless index
    uint32_t outWidth = 0;                         // motion extent (dispatch domain)
    uint32_t outHeight = 0;
    uint32_t samplerIndex = 0;                     // point/clamp (hard depth taps)
    uint32_t pad0 = 0;
};

struct MotionVectors {
    MotionVectorTarget target;
    Holder<ShaderModuleHandle> vsModule;
    Holder<ShaderModuleHandle> fragModule;
    ShaderProgram program{};
    Holder<ShaderModuleHandle> cameraMotionModule;
    ShaderProgram cameraMotionProgram{};

    [[nodiscard]] bool ShadersValid() const {
        return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
    }

    [[nodiscard]] bool Valid() const {
        return target.Valid() && ShadersValid();
    }

    [[nodiscard]] bool CameraMotionShadersValid() const {
        return cameraMotionProgram.shaderCount != 0 && !cameraMotionModule.empty();
    }

    // The camera-only reconstruction leg (Phase 3): loaded on demand so a
    // missing module keeps the raster fallback intact.
    bool EnsureCameraMotionShaders(ResourceManager& ctx) {
        if (CameraMotionShadersValid())
            return true;
        try {
            if (cameraMotionModule.empty())
                cameraMotionModule =
                    ctx.CreateShaderModule(readFile(kCameraMotionCsPath), "cameraMotionCS");
        } catch (...) {
            return false;
        }
        if (cameraMotionModule.empty())
            return false;
        Shader cs{.module = cameraMotionModule,
                  .entryPoint = "mainCS",
                  .stage = ShaderStage::Compute};
        cameraMotionProgram = {.shaders = {cs}, .shaderCount = 1};
        return true;
    }

    bool EnsureShaders(ResourceManager& ctx,
                       const char* vsPath = kMotionVectorVsPath,
                       const char* psPath = kMotionVectorPsPath) {
        if (ShadersValid())
            return true;
        try {
            const std::string vs = vsPath ? vsPath : kMotionVectorVsPath;
            const std::string ps = psPath ? psPath : kMotionVectorPsPath;
            if (vsModule.empty())
                vsModule = ctx.CreateShaderModule(readFile(vs), "motionVectorVert");
            if (fragModule.empty())
                fragModule = ctx.CreateShaderModule(readFile(ps), "motionVectorFrag");
        } catch (...) {
            return false;
        }
        if (vsModule.empty() || fragModule.empty())
            return false;
        Shader vs{.module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex};
        Shader ps{.module = fragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment};
        program = {.shaders = {vs, ps}, .shaderCount = 2};
        return true;
    }

    void DestroyShaders() {
        program = ShaderProgram();
        vsModule = {};
        fragModule = {};
        cameraMotionProgram = ShaderProgram();
        cameraMotionModule = {};
    }

    // HOT-RELOAD consumer leg (Rendergraph/ShaderSwap.h): fresh-first
    // module swap (a failed read keeps the standing module), zero the
    // program, re-Ensure. Returns true when anything moved; the released
    // old handles ride `retired` for the caller's retirement pass.
    [[nodiscard]] bool ReloadShaders(ResourceManager& ctx,
                                     std::vector<ShaderModuleHandle>& retired) {
        bool any = false;
        any |= SwapShaderModule(ctx, kMotionVectorVsPath, "motionVectorVert", vsModule, retired);
        any |= SwapShaderModule(ctx, kMotionVectorPsPath, "motionVectorFrag", fragModule, retired);
        if (!any)
            return false;
        program = ShaderProgram();
        EnsureShaders(ctx);
        return true;
    }

    // Idempotent target + program creation. Returns true when ready.
    bool Ensure(ResourceManager& ctx,
                RenderGraph& rg,
                Extent2D targetExtent,
                const MotionVectorDesc& d = {}) {
        bool ok = target.Ensure(ctx, rg, targetExtent, d);
        ok = EnsureShaders(ctx) && ok;
        return ok;
    }

    void Destroy(RenderGraph& rg) {
        target.Destroy(rg);
        DestroyShaders();
    }

    // Emits the motion pass: RG16F color (clear 0) + 1x depth loaded
    // read-only (prepass filled it) and STORED (Main, SSAO, the tile
    // cull and deferred shadows all consume it downstream — DontCare
    // here discards it out from under them: fullscreen corruption with
    // Main falling back to draw-order (far wall covers the hall) plus
    // depth-fighting speckles). Caller records draws.
    RenderPass& AddMotionVectorPass(RenderGraph& rg,
                                    LogicalImageViewHandle depthView,
                                    LogicalImageViewHandle motionView,
                                    Extent2D passExtent,
                                    const char* name = nullptr,
                                    bool load = false) const {
        assert(ShadersValid());
        assert(!depthView.empty());
        assert(!motionView.empty());

        RenderPass& pass = rg.AddPass(name ? name : "MotionVectors");
        std::vector<LogicalImageViewHandle> colors;
        colors.push_back(motionView);
        // `load` preserves the camera-only reconstruction the compute pass
        // wrote; the default clear is the standalone raster path.
        pass.SetOffset(0, 0)
            .SetExtent(passExtent)
            .SetColor(colors,
                      load ? AttachmentLoadOp::Load : AttachmentLoadOp::Clear,
                      AttachmentStoreOp::Store,
                      ClearColor(0, 0, 0, 0))
            .SetDepth(depthView,
                      AttachmentLoadOp::Load,
                      AttachmentStoreOp::Store,
                      ClearDepthStencil(0, 0));
        return pass;
    }

    // The camera-only reconstruction leg (Phase 3): ONE compute pass that
    // writes the motion target through its storage bindless (the dynamic-only
    // raster pass then LOADs and overrides it for genuinely moving geometry).
    // Runs BEFORE the raster leg; the raster leg's depth edge still orders the
    // whole motion chain after the prepass.
    void AddCameraMotionPass(RenderGraph& rg,
                             LogicalImageViewHandle depthView,
                             uint32_t depthBindlessIndex,
                             uint32_t pointSamplerIndex,
                             const CameraMotionPush& push,
                             const char* name = "CameraMotion") const {
        assert(CameraMotionShadersValid());
        assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
        assert(push.outImageIndex != UINT32_MAX);

        RenderPass& pass = rg.AddComputePass(name);
        {
            // The opaque depth, sampled from compute (the SSRClassify/HZB shape).
            ImageSyncRequest depthReq{};
            depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
            depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
            depthReq.layout = ImageLayout::DepthStencilReadOnlyOptimal;
            pass.AddImageHint(depthView, depthReq);
        }
        {
            // The motion target storage write (General); the raster override
            // LOADs it as a color attachment afterwards.
            ImageSyncRequest writeReq{};
            writeReq.barrierMasks.accessMask = AccessFlags::ShaderWrite;
            writeReq.barrierMasks.stageMask = PipelineStageBit_Compute;
            writeReq.layout = ImageLayout::General;
            pass.AddImageHint(target.logicalView, writeReq);
        }

        ShaderProgram progCopy = cameraMotionProgram; // copyable (handles only)
        const uint32_t w = push.outWidth;
        const uint32_t h = push.outHeight;
        pass.SetRecordingFunc([progCopy, push, w, h](CommandBuffer& cmd) mutable {
            cmd.BindComputePipeline(progCopy);
            cmd.PushConstants(0, sizeof(CameraMotionPush), &push, ShaderStage::Compute);
            cmd.Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });
    }
};

} // namespace smvk
