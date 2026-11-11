// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Reusable screen-space HDR distortion (authored RG flow map or heat shimmer).
// Run after scene HDR and before bloom/tonemap. Uses a distinct output image,
// since reading and writing the same attachment would create feedback.
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/vertex_layouts.h>
#include <Engine/Rendergraph/ShaderSwap.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

enum class DistortionMode : uint32_t {
    Field = 0, // RG = signed flow encoded 0..1, B = mask
    Heat = 1,  // animated, texture-free heat shimmer
};

struct DistortionDesc {
    Format format = Format::RGBA16_Float; // same as source HDR image
    DistortionMode mode = DistortionMode::Heat;
    float strengthPx = 4.0f;              // source pixels; 0 = identity
    float speed = 2.0f;                   // phase radians per second
    float frequency = 0.035f;             // spatial radians per pixel
    float centerX = 0.5f, centerY = 0.5f; // screen UV
    float radiusX = 1.0f, radiusY = 1.0f; // ellipse axes in screen UV
    float edgeSoftness = 0.25f;           // fractional feather width, 0..1
    float fieldScale = 1.0f;              // RG field amplitude
    const char* name = "distortion";
    const char* viewName = "distortionView";
    const char* logicalName = "distortionLogical";
};

// Must match engine/shaders/distortion.frag.hlsl DistortionPush exactly.
struct DistortionPush {
    uint32_t sceneIndex = UINT32_MAX;
    uint32_t fieldIndex = UINT32_MAX;
    uint32_t samplerIndex = 0;
    uint32_t mode = 1;
    float strengthPx = 4.0f;
    float time = 0.0f;
    float speed = 2.0f;
    float frequency = 0.035f;
    float texelX = 0.0f, texelY = 0.0f;
    float centerX = 0.5f, centerY = 0.5f;
    float radiusX = 1.0f, radiusY = 1.0f;
    float edgeSoftness = 0.25f;
    float fieldScale = 1.0f;
};
static_assert(sizeof(DistortionPush) == 64);

inline constexpr const char* kDistortionVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kDistortionPsPath = "content/shaders/distortion.frag.hlsl.spv";

struct Distortion {
    Holder<ImageHandle> image;
    Holder<ImageViewHandle> view;
    Holder<BindlessImageHandle> bindless;
    uint32_t bindlessIndex = UINT32_MAX;
    LogicalImageViewHandle logicalView{};
    Extent2D extent{0, 0};
    Format format = Format::RGBA16_Float;

    Holder<ShaderModuleHandle> vsModule;
    Holder<ShaderModuleHandle> fragModule;
    ShaderProgram program{};
    DistortionDesc desc{};

    [[nodiscard]] bool TargetValid() const {
        return !image.empty() && !view.empty() && !bindless.empty() && !logicalView.empty() &&
               bindlessIndex != UINT32_MAX;
    }
    [[nodiscard]] bool ShadersValid() const {
        return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
    }
    [[nodiscard]] bool Valid() const {
        return TargetValid() && ShadersValid();
    }
    [[nodiscard]] LogicalImageViewHandle OutputView() const {
        return logicalView;
    }
    [[nodiscard]] uint32_t OutputBindless() const {
        return bindlessIndex;
    }

    bool EnsureShaders(ResourceManager& ctx,
                       const char* vsPath = kDistortionVsPath,
                       const char* psPath = kDistortionPsPath) {
        if (ShadersValid())
            return true;
        try {
            if (vsModule.empty())
                vsModule = ctx.CreateShaderModule(readFile(vsPath ? vsPath : kDistortionVsPath),
                                                  "distortionVert");
            if (fragModule.empty())
                fragModule = ctx.CreateShaderModule(readFile(psPath ? psPath : kDistortionPsPath),
                                                    "distortionFrag");
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
    }

    [[nodiscard]] bool ReloadShaders(ResourceManager& ctx,
                                     std::vector<ShaderModuleHandle>& retired) {
        bool changed = false;
        changed |= SwapShaderModule(ctx, kDistortionVsPath, "distortionVert", vsModule, retired);
        changed |= SwapShaderModule(ctx, kDistortionPsPath, "distortionFrag", fragModule, retired);
        if (!changed)
            return false;
        program = ShaderProgram();
        EnsureShaders(ctx);
        return true;
    }

    void DestroyTarget(RenderGraph& rg) {
        if (!logicalView.empty())
            rg.FreeImageView(logicalView);
        logicalView = {};
        bindlessIndex = UINT32_MAX;
        bindless = {};
        view = {};
        image = {};
        extent = {0, 0};
    }

    bool CreateTarget(ResourceManager& ctx,
                      RenderGraph& rg,
                      Extent2D targetExtent,
                      const DistortionDesc& d) {
        if (!targetExtent.width || !targetExtent.height)
            return false;
        DestroyTarget(rg);
        ImageCreateInfo ci{};
        ci.extent = {targetExtent.width, targetExtent.height, 1};
        ci.format = d.format;
        ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.name = d.name;

        auto newImage = ctx.CreateImage(ci, nullptr);
        if (newImage.empty())
            return false;
        ImageViewCreateInfo vci{};
        vci.viewType = ImageViewType::View2D;
        vci.format = Format::Undefined;
        vci.subresourceRange = {ImageAspect::Color, 0, 1, 0, 1};
        vci.name = d.viewName;
        auto newView = ctx.CreateImageView(newImage, vci);
        if (newView.empty())
            return false;
        auto newBindless = ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
        if (newBindless.empty())
            return false;
        uint32_t newIndex = ctx.GetBindlessIndex(newBindless);
        auto newLogical = rg.AddExternalImageView(newView, d.format, d.logicalName);
        if (newLogical.empty())
            return false;

        image = std::move(newImage);
        view = std::move(newView);
        bindless = std::move(newBindless);
        bindlessIndex = newIndex;
        logicalView = newLogical;
        extent = targetExtent;
        format = d.format;
        return true;
    }

    bool Ensure(ResourceManager& ctx,
                RenderGraph& rg,
                Extent2D targetExtent,
                const DistortionDesc& d,
                const char* vsPath = kDistortionVsPath,
                const char* psPath = kDistortionPsPath) {
        desc = d;
        bool targetOk = TargetValid() && extent.width == targetExtent.width &&
                        extent.height == targetExtent.height && format == d.format;
        if (!targetOk)
            targetOk = CreateTarget(ctx, rg, targetExtent, d);
        return EnsureShaders(ctx, vsPath, psPath) && targetOk;
    }

    void Destroy(RenderGraph& rg) {
        DestroyTarget(rg);
        DestroyShaders();
    }

    // scene and field must be distinct from OutputView(); field is optional
    // and required only in Field mode. No source/target aliasing allowed.
    RenderPass& AddDistortionPass(RenderGraph& rg,
                                  LogicalImageViewHandle sceneView,
                                  uint32_t sceneBindlessIndex,
                                  uint32_t linearSamplerIndex,
                                  Extent2D passExtent,
                                  float timeSeconds,
                                  const DistortionDesc& settings,
                                  LogicalImageViewHandle fieldView = {},
                                  uint32_t fieldBindlessIndex = UINT32_MAX,
                                  const char* name = nullptr) const {
        assert(Valid());
        assert(!sceneView.empty() && sceneBindlessIndex != UINT32_MAX);
        assert(sceneView != logicalView);
        assert(passExtent.width == extent.width && passExtent.height == extent.height);
        assert(fieldView.empty() || fieldView != logicalView);

        bool useField = settings.mode == DistortionMode::Field && !fieldView.empty() &&
                        fieldBindlessIndex != UINT32_MAX;
        DistortionPush push{};
        push.sceneIndex = sceneBindlessIndex;
        push.fieldIndex = useField ? fieldBindlessIndex : UINT32_MAX;
        push.samplerIndex = linearSamplerIndex;
        push.mode = static_cast<uint32_t>(settings.mode);
        push.strengthPx =
            std::isfinite(settings.strengthPx) ? (std::max)(0.0f, settings.strengthPx) : 0.0f;
        push.time = std::isfinite(timeSeconds) ? timeSeconds : 0.0f;
        push.speed = std::isfinite(settings.speed) ? settings.speed : 0.0f;
        push.frequency = std::isfinite(settings.frequency) ? settings.frequency : 0.0f;
        push.texelX = 1.0f / static_cast<float>(passExtent.width);
        push.texelY = 1.0f / static_cast<float>(passExtent.height);
        push.centerX = settings.centerX;
        push.centerY = settings.centerY;
        push.radiusX = settings.radiusX;
        push.radiusY = settings.radiusY;
        push.edgeSoftness = std::clamp(settings.edgeSoftness, 0.0f, 1.0f);
        push.fieldScale = settings.fieldScale;

        std::vector<LogicalImageViewHandle> inputs{sceneView};
        if (useField)
            inputs.push_back(fieldView);
        RenderPass& pass = rg.AddPass(name ? name : settings.name);
        pass.SetOffset(0, 0)
            .SetExtent(passExtent)
            .SetInputImages(inputs)
            .SetColor({logicalView}, AttachmentLoadOp::DontCare, AttachmentStoreOp::Store);
        ShaderProgram progCopy = program;
        pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
            cmd.BindRenderPipeline(progCopy,
                                   tour::vertex_layouts::Empty,
                                   DepthSettings::Disabled(),
                                   StencilSettings::DefaultDisabled(),
                                   BlendSettings::Opaque(),
                                   MSAASettings::Count1(),
                                   RasterSettings{.cullMode = CullMode::None});
            cmd.PushConstants(0, sizeof(DistortionPush), &push, ShaderStage::All);
            cmd.Draw(3, 1, 0, 0);
        });
        return pass;
    }

    RenderPass& AddDistortionPass(RenderGraph& rg,
                                  LogicalImageViewHandle sceneView,
                                  uint32_t sceneBindlessIndex,
                                  uint32_t linearSamplerIndex,
                                  Extent2D passExtent,
                                  float timeSeconds,
                                  LogicalImageViewHandle fieldView = {},
                                  uint32_t fieldBindlessIndex = UINT32_MAX,
                                  const char* name = nullptr) const {
        return AddDistortionPass(rg,
                                 sceneView,
                                 sceneBindlessIndex,
                                 linearSamplerIndex,
                                 passExtent,
                                 timeSeconds,
                                 desc,
                                 fieldView,
                                 fieldBindlessIndex,
                                 name);
    }
};

} // namespace smvk
