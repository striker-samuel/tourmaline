// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// CORTAO (Circular Occluder Ray-Traced Ambient Occlusion) — Carbon Engine
// implementation (MIT, Fenris Creations). Three compute passes (prepass,
// main, filter) + a full-res occlusion output that the SSGI compose applies.
//
// SM 6.7 workaround (see CORTAO README): the packed data buffer is R32_FLOAT
// (float bits of the packed uint, converted via asfloat/asuint) and the
// occluder lookup table is R16_UNORM (scaled back to uint), so neither needs
// integer-format sampling bindings the engine's bindless set lacks.

#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/CORTAOParameters.h>
#include <Engine/Rendergraph/cortao_occluder_lut.h>
#include <Engine/Rendergraph/renderer.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Utils/shader_library.h>

#include <array>
#include <cstdint>

namespace smvk::cortao {

inline constexpr uint32_t kMaxMips = 8;
inline constexpr uint32_t kSampleCount = 16;
inline constexpr uint32_t kRayCount = 16;
inline constexpr uint32_t kMipCount = 6; // packed-data mip chain length

// ---- push mirrors (keep the HLSL twins in lockstep) ----

struct CortaoPrepassPush {
    uint32_t depthTextureIndex = UINT32_MAX;
    uint32_t normalTextureIndex = UINT32_MAX;
    uint32_t packedMip0Index = UINT32_MAX; // storage index of mip 0 (1..7 follow)
    uint32_t pad0 = 0;
    uint32_t packedResolution = 0;
    float depthScale = 0.0f;
    float depthOffset = 0.0f;
    uint32_t randomSeed = 0;
    uint32_t mipCount = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
    uint32_t pad3 = 0;
    uint64_t frame = 0;
};

struct CortaoMainPassPush {
    uint32_t packedTextureIndex = UINT32_MAX;
    uint32_t occluderLutIndex = UINT32_MAX;
    uint32_t normalTextureIndex = UINT32_MAX;
    uint32_t occlusionIndex = UINT32_MAX;
    uint32_t nearestBorderSampler = 0;
    uint32_t nearestClampSampler = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t packedResolution = 0;
    uint32_t randomSeed = 0;
    float normalBias = 0.0f;
    float maxApparentCircleRadiusCoefficient = 0.0f;
    float unprojectParamsX = 0.0f;
    float unprojectParamsY = 0.0f;
    float unprojectParamsZ = 0.0f;
    float unprojectParamsW = 0.0f;
    float projectionParamsX = 0.0f;
    float projectionParamsY = 0.0f;
    float projectionParamsZ = 0.0f;
    float projectionParamsW = 0.0f;
    float mipBias = 0.0f;
    float inverseMaxSlopeCompensation = 0.0f;
    float backfaceThreshold = 0.0f;
    float backfaceBoost = 0.0f;
    uint64_t frame = 0;
};

struct CortaoFilterPassPush {
    uint32_t packedTextureIndex = UINT32_MAX;
    uint32_t occlusionInIndex = UINT32_MAX;
    uint32_t occlusionOutIndex = UINT32_MAX;
    uint32_t nearestBorderSampler = 0;
    uint32_t packedResolution = 0;
    float depthSensitivity = 0.0f;
    float normalSensitivity = 0.0f;
    uint32_t pad0 = 0;
};

// ---- desc ----

struct CortaoDesc {
    Format packedFormat = Format::R32_Float; // SM 6.7 workaround
    // R32_Float (NOT R8_UNORM): the engine's storage binding is declared
    // RWTexture2D<float> -> R32f identity (see contact_shadow.h); R8_UNORM
    // would need a separate RWTexture2D<unorm float> binding the engine
    // doesn't declare.
    Format occlusionFormat = Format::R32_Float;
    float maxScreenspaceRadiusHeightRatio = 0.2f;
    float mipBias = -4.0f;
    float maxSlopeCompensation = 4.0f;
    float backfaceThreshold = 0.5f;
    float backfaceBoost = 0.5f;
    float filterDepthSensitivity = 512.0f;
    float filterNormalSensitivity = 16.0f;
    uint32_t mipCount = kMipCount;
    const char* name = "cortao";
};

// Default SPVs.
inline constexpr const char* kPrepassCsPath = "content/shaders/cortao_prepass.cs.hlsl.spv";
inline constexpr const char* kMainCsPath = "content/shaders/cortao_main.cs.hlsl.spv";
inline constexpr const char* kFilterCsPath = "content/shaders/cortao_filter.cs.hlsl.spv";
inline constexpr const char* kComposeVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kComposePsPath = "content/shaders/cortao_compose.frag.hlsl.spv";

struct CortaoComposePush {
    uint32_t occlusionIndex = UINT32_MAX;
    uint32_t aoTermIndex = UINT32_MAX;
    uint32_t hdrIndex = UINT32_MAX;
    float intensity = 1.0f;
};

struct Cortao {
    // Packed data buffer (R32_FLOAT, mipmapped).
    Holder<ImageHandle> packedImage;
    Holder<ImageViewHandle> packedView;         // full mip chain (sampled)
    Holder<BindlessImageHandle> packedBindless; // full mip chain (sampled)
    uint32_t packedIndex = UINT32_MAX;
    LogicalImageViewHandle packedLogical{};
    std::array<Holder<ImageViewHandle>, kMaxMips> packedMipViews{};
    std::array<Holder<BindlessImageHandle>, kMaxMips> packedMipBindless{};
    std::array<uint32_t, kMaxMips> packedMipIndices{};
    std::array<LogicalImageViewHandle, kMaxMips> packedMipLogical{};

    // Occluder lookup table (3D R16_UNORM).
    Holder<TextureHandle> occluderLut;
    uint32_t occluderLutIndex = UINT32_MAX;

    // Occlusion outputs (R32_FLOAT): raw (main output) + filtered (filter output).
    Holder<ImageHandle> occlusionRawImage;
    Holder<ImageViewHandle> occlusionRawView;
    Holder<BindlessImageHandle> occlusionRawBindless;
    uint32_t occlusionRawIndex = UINT32_MAX;
    LogicalImageViewHandle occlusionRawLogical{};
    Holder<ImageHandle> occlusionFilteredImage;
    Holder<ImageViewHandle> occlusionFilteredView;
    Holder<BindlessImageHandle> occlusionFilteredBindless;
    uint32_t occlusionFilteredIndex = UINT32_MAX;
    LogicalImageViewHandle occlusionFilteredLogical{};

    Holder<ShaderModuleHandle> prepassModule, mainModule, filterModule;
    ShaderProgram prepassProgram{}, mainProgram{}, filterProgram{};
    Holder<ShaderModuleHandle> composeVsModule, composePsModule;
    ShaderProgram composeProgram{};

    // Composed HDR (the compose output): full-res RGBA16F, ping-pong with the
    // scene HDR.
    Holder<ImageHandle> composedImage;
    Holder<ImageViewHandle> composedView;
    Holder<BindlessImageHandle> composedBindless;
    uint32_t composedIndex = UINT32_MAX;
    LogicalImageViewHandle composedLogical{};

    uint32_t nearestBorderSampler = 0;
    uint32_t nearestClampSampler = 0;

    Extent2D extent{0, 0};

    bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D e, const CortaoDesc& d) {
        // Resize / first run: tear down the extent-dependent resources and
        // recreate at the new size. Samplers + shaders are size-independent.
        if (extent.width != e.width || extent.height != e.height)
            Destroy(rg);
        extent = e;
        nearestBorderSampler = ctx.GetOrCreateSampler(SamplerCreateInfo{
            .magFilter = Filter::Nearest,
            .minFilter = Filter::Nearest,
            .mipmapMode = MipmapMode::Nearest,
            .addressU = AddressMode::ClampToBorder,
            .addressV = AddressMode::ClampToBorder,
            .addressW = AddressMode::ClampToBorder,
        });
        nearestClampSampler = ctx.GetOrCreateSampler(SamplerCreateInfo{
            .magFilter = Filter::Nearest,
            .minFilter = Filter::Nearest,
            .mipmapMode = MipmapMode::Nearest,
            .addressU = AddressMode::ClampToEdge,
            .addressV = AddressMode::ClampToEdge,
            .addressW = AddressMode::ClampToEdge,
        });

        bool ok = true;
        ok = EnsurePackedData(ctx, rg, d) && ok;
        ok = EnsureOccluderLut(ctx) && ok;
        ok = EnsureOcclusion(ctx, rg, d) && ok;
        ok = EnsureShaders(ctx) && ok;
        ok = EnsureComposed(ctx, rg) && ok;
        ok = EnsureComposeShader(ctx) && ok;
        return ok;
    }

    // Frees the render-graph logical views and releases the underlying
    // images/views/bindless handles. Samplers (owned by the ctx cache) and
    // shader modules (reused) are left intact.
    void Destroy(RenderGraph& rg) {
        if (!packedLogical.empty())
            rg.FreeImageView(packedLogical);
        packedLogical = {};
        for (uint32_t m = 0; m < kMaxMips; ++m) {
            if (!packedMipLogical[m].empty())
                rg.FreeImageView(packedMipLogical[m]);
            packedMipLogical[m] = {};
            packedMipViews[m].reset();
            packedMipBindless[m].reset();
            packedMipIndices[m] = UINT32_MAX;
        }
        packedView.reset();
        packedBindless.reset();
        packedIndex = UINT32_MAX;
        occluderLut.reset();
        occluderLutIndex = UINT32_MAX;
        if (!occlusionRawLogical.empty())
            rg.FreeImageView(occlusionRawLogical);
        occlusionRawLogical = {};
        occlusionRawView.reset();
        occlusionRawBindless.reset();
        occlusionRawIndex = UINT32_MAX;
        if (!occlusionFilteredLogical.empty())
            rg.FreeImageView(occlusionFilteredLogical);
        occlusionFilteredLogical = {};
        occlusionFilteredView.reset();
        occlusionFilteredBindless.reset();
        occlusionFilteredIndex = UINT32_MAX;
        if (!composedLogical.empty())
            rg.FreeImageView(composedLogical);
        composedLogical = {};
        composedView.reset();
        composedBindless.reset();
        composedIndex = UINT32_MAX;
    }

    bool EnsurePackedData(ResourceManager& ctx, RenderGraph& rg, const CortaoDesc& d) {
        if (!packedImage.empty() && packedIndex != UINT32_MAX)
            return true;
        ImageCreateInfo ci{};
        ci.type = ImageType::Image2D;
        ci.extent = {extent.width, extent.height, 1};
        ci.format = d.packedFormat;
        ci.usage = ImageUsage::Sampled | ImageUsage::Storage;
        ci.mipLevels = d.mipCount;
        ci.arrayLayers = 1;
        ci.name = "cortaoPacked";
        packedImage = ctx.CreateImage(ci, nullptr);
        if (packedImage.empty())
            return false;

        // Full mip chain (sampled).
        ImageViewCreateInfo vci{};
        vci.viewType = ImageViewType::View2D;
        vci.format = Format::Undefined;
        vci.subresourceRange = {ImageAspect::Color, 0, d.mipCount, 0, 1};
        vci.name = "cortaoPackedView";
        packedView = ctx.CreateImageView(packedImage, vci);
        // General: the main + filter compute passes sample this full-mip
        // view in General layout (the compute chain stays in General —
        // the SsProbes pattern), so the sampled descriptor must match.
        packedBindless = ctx.CreateBindlessImage(packedView, ImageLayout::General);
        if (packedBindless.empty())
            return false;
        packedIndex = ctx.GetBindlessIndex(packedBindless);
        packedLogical = rg.AddExternalImageView(packedView, d.packedFormat, "cortaoPackedLogical");

        // Per-mip storage views.
        for (uint32_t m = 0; m < d.mipCount; ++m) {
            ImageViewCreateInfo mvci{};
            mvci.viewType = ImageViewType::View2D;
            mvci.format = Format::Undefined;
            mvci.subresourceRange = {ImageAspect::Color, m, 1, 0, 1};
            mvci.name = "cortaoPackedMip";
            packedMipViews[m] = ctx.CreateImageView(packedImage, mvci);
            packedMipBindless[m] = ctx.CreateBindlessImage(packedMipViews[m], ImageLayout::General);
            if (packedMipBindless[m].empty())
                return false;
            packedMipIndices[m] = ctx.GetBindlessIndex(packedMipBindless[m]);
            packedMipLogical[m] = rg.AddExternalImageView(
                packedMipViews[m], d.packedFormat, "cortaoPackedMipLogical");
        }
        return true;
    }

    bool EnsureOccluderLut(ResourceManager& ctx) {
        if (!occluderLut.empty() && occluderLutIndex != UINT32_MAX)
            return true;
        ImageCreateInfo ci{};
        ci.type = ImageType::Image3D;
        ci.extent = {kOccluderLutWidth, kOccluderLutHeight, kOccluderLutDepth};
        ci.format = Format::R16_UNorm;
        ci.usage = ImageUsage::Sampled | ImageUsage::TransferDst;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.name = "cortaoOccluderLut";
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(kOccluderLut);
        occluderLut = ctx.CreateTexture(ci, raw);
        if (occluderLut.empty())
            return false;
        occluderLutIndex = ctx.GetTextureBindlessIndex(occluderLut);
        return occluderLutIndex != UINT32_MAX;
    }

    bool EnsureOcclusion(ResourceManager& ctx, RenderGraph& rg, const CortaoDesc& d) {
        if (!occlusionRawImage.empty() && occlusionRawIndex != UINT32_MAX &&
            !occlusionFilteredImage.empty() && occlusionFilteredIndex != UINT32_MAX)
            return true;
        ImageCreateInfo ci{};
        ci.extent = {extent.width, extent.height, 1};
        ci.format = d.occlusionFormat;
        ci.usage = ImageUsage::Sampled | ImageUsage::Storage;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.name = "cortaoOcclusionRaw";
        occlusionRawImage = ctx.CreateImage(ci, nullptr);
        if (occlusionRawImage.empty())
            return false;
        ImageViewCreateInfo vci{};
        vci.viewType = ImageViewType::View2D;
        vci.format = Format::Undefined;
        vci.subresourceRange = {ImageAspect::Color, 0, 1, 0, 1};
        vci.name = "cortaoOcclusionRawView";
        occlusionRawView = ctx.CreateImageView(occlusionRawImage, vci);
        occlusionRawBindless = ctx.CreateBindlessImage(occlusionRawView, ImageLayout::General);
        if (occlusionRawBindless.empty())
            return false;
        occlusionRawIndex = ctx.GetBindlessIndex(occlusionRawBindless);
        occlusionRawLogical = rg.AddExternalImageView(
            occlusionRawView, d.occlusionFormat, "cortaoOcclusionRawLogical");

        ci.name = "cortaoOcclusionFiltered";
        occlusionFilteredImage = ctx.CreateImage(ci, nullptr);
        if (occlusionFilteredImage.empty())
            return false;
        vci.name = "cortaoOcclusionFilteredView";
        occlusionFilteredView = ctx.CreateImageView(occlusionFilteredImage, vci);
        // General: the filtered output stays in General through the whole
        // compute chain (storage write -> the compose's General sampled
        // read), so the descriptor matches the storage-write layout and no
        // General->ShaderReadOnlyOptimal transition is required.
        occlusionFilteredBindless =
            ctx.CreateBindlessImage(occlusionFilteredView, ImageLayout::General);
        if (occlusionFilteredBindless.empty())
            return false;
        occlusionFilteredIndex = ctx.GetBindlessIndex(occlusionFilteredBindless);
        occlusionFilteredLogical = rg.AddExternalImageView(
            occlusionFilteredView, d.occlusionFormat, "cortaoOcclusionFilteredLogical");
        return true;
    }

    bool EnsureShaders(ResourceManager& ctx) {
        if (prepassProgram.shaderCount != 0 && mainProgram.shaderCount != 0 &&
            filterProgram.shaderCount != 0)
            return true;
        try {
            if (prepassModule.empty())
                prepassModule =
                    ctx.CreateShaderModule(smvk::shaders::Read(kPrepassCsPath), "cortaoPrepass");
            if (mainModule.empty())
                mainModule = ctx.CreateShaderModule(smvk::shaders::Read(kMainCsPath), "cortaoMain");
            if (filterModule.empty())
                filterModule =
                    ctx.CreateShaderModule(smvk::shaders::Read(kFilterCsPath), "cortaoFilter");
        } catch (...) {
            return false;
        }
        if (prepassModule.empty() || mainModule.empty() || filterModule.empty())
            return false;
        Shader s{.entryPoint = "mainCS", .stage = ShaderStage::Compute};
        s.module = prepassModule;
        prepassProgram = {.shaders = {s}, .shaderCount = 1};
        s.module = mainModule;
        mainProgram = {.shaders = {s}, .shaderCount = 1};
        s.module = filterModule;
        filterProgram = {.shaders = {s}, .shaderCount = 1};
        return true;
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

    bool EnsureComposed(ResourceManager& ctx, RenderGraph& rg) {
        if (!composedImage.empty() && composedIndex != UINT32_MAX)
            return true;
        ImageCreateInfo ci{};
        ci.extent = {extent.width, extent.height, 1};
        ci.format = Format::RGBA16_Float;
        ci.usage = ImageUsage::Sampled | ImageUsage::ColorAttachment;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.name = "cortaoComposed";
        composedImage = ctx.CreateImage(ci, nullptr);
        if (composedImage.empty())
            return false;
        ImageViewCreateInfo vci{};
        vci.viewType = ImageViewType::View2D;
        vci.format = Format::Undefined;
        vci.subresourceRange = {ImageAspect::Color, 0, 1, 0, 1};
        vci.name = "cortaoComposedView";
        composedView = ctx.CreateImageView(composedImage, vci);
        composedBindless =
            ctx.CreateBindlessImage(composedView, ImageLayout::ShaderReadOnlyOptimal);
        if (composedBindless.empty())
            return false;
        composedIndex = ctx.GetBindlessIndex(composedBindless);
        composedLogical =
            rg.AddExternalImageView(composedView, Format::RGBA16_Float, "cortaoComposedLogical");
        return true;
    }

    bool EnsureComposeShader(ResourceManager& ctx) {
        if (composeProgram.shaderCount != 0)
            return true;
        try {
            if (composeVsModule.empty())
                composeVsModule = ctx.CreateShaderModule(smvk::shaders::Read(kComposeVsPath),
                                                         "cortaoComposeVert");
            if (composePsModule.empty())
                composePsModule = ctx.CreateShaderModule(smvk::shaders::Read(kComposePsPath),
                                                         "cortaoComposeFrag");
        } catch (...) {
            return false;
        }
        if (composeVsModule.empty() || composePsModule.empty())
            return false;
        Shader vs{.module = composeVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex};
        Shader ps{
            .module = composePsModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment};
        composeProgram = {.shaders = {vs, ps}, .shaderCount = 2};
        return true;
    }

    // Emits prepass + main + filter. The caller fills the CORTAO config and
    // passes the logical views + bindless indices of the depth + normal
    // (for the read barriers) and the frame-uniform BDA.
    bool AddAoPasses(RenderGraph& rg,
                     const CORTAO::CORTAOConfigurationParameters& cfg,
                     LogicalImageViewHandle depthView,
                     uint32_t depthIndex,
                     LogicalImageViewHandle normalView,
                     uint32_t normalIndex,
                     uint64_t frameUniformsAddress) {
        if (prepassProgram.shaderCount == 0 || mainProgram.shaderCount == 0 ||
            filterProgram.shaderCount == 0 || packedIndex == UINT32_MAX ||
            occluderLutIndex == UINT32_MAX || occlusionRawIndex == UINT32_MAX ||
            occlusionFilteredIndex == UINT32_MAX)
            return false;

        CORTAO::CORTAOPrepassParameters prepassParams;
        CORTAO::PopulatePrepassParameters(cfg, &prepassParams);
        CORTAO::CORTAOMainPassParameters mainParams;
        CORTAO::PopulateMainPassParameters(cfg, &mainParams);
        CORTAO::CORTAOFilterPassParameters filterParams;
        CORTAO::PopulateFilterPassParameters(cfg, &filterParams);

        // The compute chain runs entirely in General layout (the SsProbes
        // pattern): no layout transitions between the three passes, and the
        // bindless descriptors stay consistent (General for storage + read).
        const ImageSyncRequest writeReq{
            .barrierMasks = {AccessFlags::ShaderWrite, PipelineStageBit_Compute},
            .layout = ImageLayout::General};
        const ImageSyncRequest readReq{
            .barrierMasks = {AccessFlags::ShaderRead, PipelineStageBit_Compute},
            .layout = ImageLayout::General};
        // Full-res reversed-Z depth: sampled via bindless (the SsProbes
        // compute-read contract).
        const ImageSyncRequest depthReadReq{
            .barrierMasks = {AccessFlags::ShaderRead, PipelineStageBit_Compute},
            .layout = ImageLayout::DepthStencilReadOnlyOptimal};
        // Shared G-buffer oct normal (world space): sampled via bindless.
        const ImageSyncRequest normalReadReq{
            .barrierMasks = {AccessFlags::ShaderRead, PipelineStageBit_Compute},
            .layout = ImageLayout::ShaderReadOnlyOptimal};

        // ---- prepass ----
        {
            auto& pass = rg.AddComputePass("CortaoPrepass");
            if (!depthView.empty())
                pass.AddImageHint(depthView, depthReadReq);
            if (!normalView.empty())
                pass.AddImageHint(normalView, normalReadReq);
            for (uint32_t m = 0; m < cfg.mipCount; ++m)
                pass.AddImageHint(packedMipLogical[m], writeReq);
            CortaoPrepassPush push{};
            push.depthTextureIndex = depthIndex;
            push.normalTextureIndex = normalIndex;
            push.packedMip0Index = packedMipIndices[0];
            push.packedResolution = prepassParams.packedResolution;
            push.depthScale = prepassParams.depthScale;
            push.depthOffset = prepassParams.depthOffset;
            push.randomSeed = prepassParams.randomSeed;
            push.mipCount = prepassParams.mipCount;
            push.frame = frameUniformsAddress;
            auto& f = rg.PassFrameData<Frame<CortaoPrepassPush>>(pass);
            f.program = &prepassProgram;
            f.push = push;
            f.groupsX = (extent.width + 7) / 8;
            f.groupsY = (extent.height + 7) / 8;
            pass.SetRecordFn(&Record<CortaoPrepassPush>);
        }

        // ---- main ----
        {
            auto& pass = rg.AddComputePass("CortaoMain");
            pass.AddImageHint(packedLogical, readReq);
            if (!normalView.empty())
                pass.AddImageHint(normalView, normalReadReq);
            pass.AddImageHint(occlusionRawLogical, writeReq);
            CortaoMainPassPush push{};
            push.packedTextureIndex = packedIndex;
            push.occluderLutIndex = occluderLutIndex;
            push.normalTextureIndex = normalIndex;
            push.occlusionIndex = occlusionRawIndex;
            push.nearestBorderSampler = nearestBorderSampler;
            push.nearestClampSampler = nearestClampSampler;
            push.packedResolution = mainParams.packedResolution;
            push.randomSeed = mainParams.randomSeed;
            push.normalBias = mainParams.normalBias;
            push.maxApparentCircleRadiusCoefficient = mainParams.maxApparentCircleRadiusCoefficient;
            push.unprojectParamsX = mainParams.unprojectParamsX;
            push.unprojectParamsY = mainParams.unprojectParamsY;
            push.unprojectParamsZ = mainParams.unprojectParamsZ;
            push.unprojectParamsW = mainParams.unprojectParamsW;
            push.projectionParamsX = mainParams.projectionParamsX;
            push.projectionParamsY = mainParams.projectionParamsY;
            push.projectionParamsZ = mainParams.projectionParamsZ;
            push.projectionParamsW = mainParams.projectionParamsW;
            push.mipBias = mainParams.mipBias;
            push.inverseMaxSlopeCompensation = mainParams.inverseMaxSlopeCompensation;
            push.backfaceThreshold = mainParams.backfaceThreshold;
            push.backfaceBoost = mainParams.backfaceBoost;
            push.frame = frameUniformsAddress;
            auto& f = rg.PassFrameData<Frame<CortaoMainPassPush>>(pass);
            f.program = &mainProgram;
            f.push = push;
            f.groupsX = (extent.width + 15) / 16;
            f.groupsY = (extent.height + 15) / 16;
            pass.SetRecordFn(&Record<CortaoMainPassPush>);
        }

        // ---- filter ----
        {
            auto& pass = rg.AddComputePass("CortaoFilter");
            pass.AddImageHint(packedLogical, readReq);
            pass.AddImageHint(occlusionRawLogical, readReq);
            pass.AddImageHint(occlusionFilteredLogical, writeReq);
            CortaoFilterPassPush push{};
            push.packedTextureIndex = packedIndex;
            push.occlusionInIndex = occlusionRawIndex;
            push.occlusionOutIndex = occlusionFilteredIndex;
            push.nearestBorderSampler = nearestBorderSampler;
            push.packedResolution = filterParams.packedResolution;
            push.depthSensitivity = filterParams.depthSensitivity;
            push.normalSensitivity = filterParams.normalSensitivity;
            auto& f = rg.PassFrameData<Frame<CortaoFilterPassPush>>(pass);
            f.program = &filterProgram;
            f.push = push;
            f.groupsX = (extent.width / 2 + 7) / 8;
            f.groupsY = (extent.height / 2 + 7) / 8;
            pass.SetRecordFn(&Record<CortaoFilterPassPush>);
        }

        return true;
    }

    struct ComposeFrame {
        ShaderProgram* program = nullptr;
        CortaoComposePush push{};
    };
    static void RecordCompose(CommandBuffer& cmd, const FrameContext& fc, const PassData& pd) {
        const auto& f = fc.rg->FrameData().Get<ComposeFrame>(pd.frameSlot);
        cmd.BindRenderPipeline(*f.program,
                               ::tour::vertex_layouts::Empty,
                               DepthSettings::Disabled(),
                               StencilSettings::DefaultDisabled(),
                               BlendSettings::Opaque(),
                               MSAASettings::Count1(),
                               RasterSettings{.cullMode = CullMode::None});
        cmd.PushConstants(0, sizeof(CortaoComposePush), &f.push, ShaderStage::All);
        cmd.Draw(3, 1, 0, 0); // fullscreen triangle
    }

    // Applies the CORTAO occlusion to the scene HDR: reads the HDR + aoTerm +
    // filtered occlusion, writes the composed HDR target.
    bool AddComposePass(RenderGraph& rg,
                        LogicalImageViewHandle hdrView,
                        uint32_t hdrBindless,
                        LogicalImageViewHandle aoTermView,
                        uint32_t aoTermBindless,
                        float intensity) {
        if (composeProgram.shaderCount == 0 || composedLogical.empty() || hdrView.empty() ||
            occlusionFilteredIndex == UINT32_MAX)
            return false;
        RenderPass& pass = rg.AddPass("CortaoCompose");
        // The filtered occlusion is a STORAGE image read in GENERAL layout
        // (the filter wrote it through the storage binding); hint it BEFORE
        // SetInputImages so the input list does not stamp ShaderReadOnlyOptimal
        // over it. The HDR + aoTerm are color attachments (ShaderReadOnlyOptimal).
        const ImageSyncRequest occlusionRead{
            .barrierMasks = {AccessFlags::ShaderRead, PipelineStageBit_Fragment},
            .layout = ImageLayout::General};
        pass.AddImageHint(occlusionFilteredLogical, occlusionRead);
        std::vector<LogicalImageViewHandle> inputs = {occlusionFilteredLogical, hdrView};
        if (!aoTermView.empty())
            inputs.push_back(aoTermView);
        pass.SetOffset(0, 0).SetExtent(extent).SetInputImages(inputs);
        pass.SetColor({composedLogical}, AttachmentLoadOp::DontCare, AttachmentStoreOp::Store);
        CortaoComposePush push{};
        push.occlusionIndex = occlusionFilteredIndex;
        push.aoTermIndex = aoTermBindless;
        push.hdrIndex = hdrBindless;
        push.intensity = intensity;
        auto& f = rg.PassFrameData<ComposeFrame>(pass);
        f.program = &composeProgram;
        f.push = push;
        pass.SetRecordFn(&RecordCompose);
        return true;
    }

    [[nodiscard]] LogicalImageViewHandle OutputView() const {
        return composedLogical;
    }
    [[nodiscard]] uint32_t OutputBindless() const {
        return composedIndex;
    }

    [[nodiscard]] bool Valid() const {
        return packedIndex != UINT32_MAX && occluderLutIndex != UINT32_MAX &&
               occlusionRawIndex != UINT32_MAX && occlusionFilteredIndex != UINT32_MAX &&
               prepassProgram.shaderCount != 0 && mainProgram.shaderCount != 0 &&
               filterProgram.shaderCount != 0 && composeProgram.shaderCount != 0 &&
               !composedLogical.empty();
    }
};

} // namespace smvk::cortao
