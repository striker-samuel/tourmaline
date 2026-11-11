// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Memory/linear_vector.h>
#include <Engine/Renderer/GfxRenderer.h>
#include <Engine/Renderer/Renderer.h>
#include <Engine/Rendergraph/FrameContext.h>

#include <Engine/Profile/profile.h>
#include <Engine/Renderer/gfx_dynamic_capacity.h> // the declared draw bound (DumpCullBaseline)
#include <Engine/Renderer/mesh.h>
#include <Engine/Utils/shader_library.h> // the unified shader reader (dev trees + engine-shaders.pak)
#include <Engine/debug_log.h>
#include <Engine/platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace engine {

static constexpr const char* kGfxSeedVsPath = "content/shaders/gfxseed.vert.hlsl.spv";
static constexpr const char* kGfxSeedMaskedVsPath = "content/shaders/gfxseedmasked.vert.hlsl.spv";
static constexpr const char* kGfxShadowSeedVsPath = "content/shaders/gfxshadowseed.vert.hlsl.spv";
static constexpr const char* kGfxShadowSeedMaskedVsPath =
    "content/shaders/gfxshadowseedmasked.vert.hlsl.spv";
static constexpr const char* kMeshDebugVsPath = "content/shaders/meshdebug.vert.hlsl.spv";
static constexpr const char* kMeshDebugFragPath = "content/shaders/meshdebug.frag.hlsl.spv";
static constexpr const char* kLineVsPath = "content/shaders/debugline.vert.hlsl.spv";
static constexpr const char* kLineFragPath = "content/shaders/debugline.frag.hlsl.spv";

void GfxRenderer::DebugClusterColor(uint32_t cluster,
                                    float coneCutoff,
                                    int mode,
                                    float outRgba[4]) {
    if (mode == 1) {
        // Cone heat: cutoff ~1 (degenerate/wide, never culls) -> red,
        // tight useful cones -> green/cyan.
        float t = std::clamp((coneCutoff + 1.0f) * 0.5f, 0.0f, 1.0f);
        outRgba[0] = t;
        outRgba[1] = 1.0f - std::abs(t - 0.5f) * 2.0f * 0.5f;
        outRgba[2] = 1.0f - t;
        outRgba[3] = 1.0f;
        return;
    }
    // Stable hash hue per cluster (golden-ratio scramble), full saturation.
    uint32_t h = cluster * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const float hue = (h % 1024) / 1024.0f;
    const float s = 0.85f, v = 0.95f;
    const float c = v * s;
    const float hh = hue * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hh < 1.0f) {
        r = c;
        g = x;
    } else if (hh < 2.0f) {
        r = x;
        g = c;
    } else if (hh < 3.0f) {
        g = c;
        b = x;
    } else if (hh < 4.0f) {
        g = x;
        b = c;
    } else if (hh < 5.0f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    const float m = v - c;
    outRgba[0] = r + m;
    outRgba[1] = g + m;
    outRgba[2] = b + m;
    outRgba[3] = 1.0f;
}

void GfxRenderer::AppendPasses(smvk::FrameContext& fc) {
    // The gfx-filter render path is retired: no filter/seed/remainder
    // passes. The GPU-scene backends schedule Hi-Z via PrepareGpuSceneHiz.
    (void)fc;
    lastSlot_ = UINT32_MAX;
    seededThisFrame_ = false;
}

bool GfxRenderer::Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) {
    const char* vsPath = smvk::gfx::kGfxFilteredVsPath;
    if (!smvk::gfx::Ensure(gfx, ctx, rg))
        return false;
    // HZB SPD A/B + verify switches (TOURMALINE_HIZ_SPD /
    // TOURMALINE_HIZ_SPD_VERIFY — the TOURMALINE_HIZ family names the
    // CULLING pyramid; the SSGI MAX twin is TOURMALINE_HZB_SPD_*).
    // Read ONCE here so the headless bench matrix can pin them; both
    // default OFF (per-mip) until the verify gate runs green — a wrong
    // MIN pyramid over-culls, geometry disappears.
    {
        bool live = false, verify = false;
        if (const char* e = std::getenv("TOURMALINE_HIZ_SPD"))
            live = std::atoi(e) != 0;
        if (const char* e = std::getenv("TOURMALINE_HIZ_SPD_VERIFY"))
            verify = std::atoi(e) != 0;
        smvk::gfx::SetHizSpd(gfx, live, verify);
        if (live || verify)
            std::cerr << "[GfxRenderer] HZB SPD (culling): live=" << live << " verify=" << verify
                      << "\n";
    }
    if (filteredProgram.shaderCount == 0 || filteredVsModule.empty()) {
        try {
            if (filteredVsModule.empty())
                filteredVsModule =
                    ctx.CreateShaderModule(smvk::shaders::Read(vsPath), "gfxFilteredVS");
        } catch (...) {
            return false;
        }
        if (filteredVsModule.empty() || mainFrag_.empty())
            return false;
        smvk::Shader vs{
            .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
        smvk::Shader fs{
            .module = mainFrag_, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
        filteredProgram = {.shaders = {vs, fs}, .shaderCount = 2};
    }
    // A2C twin (same filtered VS + the alpha-to-coverage lit FS): built
    // only when the app configured the module; Main falls back to the
    // discard variant otherwise.
    if (filteredProgramA2C.shaderCount == 0 && !filteredVsModule.empty() && !mainA2CFrag_.empty()) {
        smvk::Shader vs{
            .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
        smvk::Shader fs{
            .module = mainA2CFrag_, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
        filteredProgramA2C = {.shaders = {vs, fs}, .shaderCount = 2};
    }
    // Wireframe twin (same filtered VS + the flat-line FS): built only when
    // the app configured the module; Main falls back to the shaded filtered
    // program otherwise.
    if (filteredWireProgram.shaderCount == 0 && !filteredVsModule.empty() && !wireFrag_.empty()) {
        smvk::Shader vs{
            .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
        smvk::Shader fs{
            .module = wireFrag_, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
        filteredWireProgram = {.shaders = {vs, fs}, .shaderCount = 2};
    }
    // Fixed-lobe lit twins (same filtered VS + the SG-permuted lit FS):
    // built only for configured modules; Main falls back to the generic
    // filtered program per missing range class.
    for (uint32_t li = 0; li < 4; ++li) {
        if (filteredLitSg_[li].shaderCount == 0 && !filteredVsModule.empty() &&
            !litFragSgModules_[li].empty()) {
            smvk::Shader vs{.module = filteredVsModule,
                            .entryPoint = "mainVS",
                            .stage = smvk::ShaderStage::Vertex};
            smvk::Shader fs{.module = litFragSgModules_[li],
                            .entryPoint = "mainPS",
                            .stage = smvk::ShaderStage::Fragment};
            filteredLitSg_[li] = {.shaders = {vs, fs}, .shaderCount = 2};
        }
    }
    // GI permutation twins (same filtered VS + the .giN lit FS — the
    // same modules the direct draws bind as mainProgramGi). Built only
    // for configured modules; the batch bind falls back to the generic
    // filtered program per missing variant, which compiles the GI
    // branches out (see FilteredProgramGi).
    for (uint32_t gi = 0; gi < 4; ++gi) {
        if (filteredProgramGi_[gi].shaderCount == 0 && !filteredVsModule.empty() &&
            !litFragGiModules_[gi].empty()) {
            smvk::Shader vs{.module = filteredVsModule,
                            .entryPoint = "mainVS",
                            .stage = smvk::ShaderStage::Vertex};
            smvk::Shader fs{.module = litFragGiModules_[gi],
                            .entryPoint = "mainPS",
                            .stage = smvk::ShaderStage::Fragment};
            filteredProgramGi_[gi] = {.shaders = {vs, fs}, .shaderCount = 2};
        }
    }
    // Standard-material twins (same filtered VS + the .std/.giNstd lit
    // FS — MAT_STANDARD, the layered machinery compiled out). Best-effort
    // per module: missing variants fall back to the monolith twins
    // (FilteredProgramGi/FilteredLitProgram pick per layered-ness).
    if (filteredProgramStd.shaderCount == 0 && !filteredVsModule.empty() && !mainFragStd_.empty()) {
        smvk::Shader vs{
            .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
        smvk::Shader fs{
            .module = mainFragStd_, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
        filteredProgramStd = {.shaders = {vs, fs}, .shaderCount = 2};
    }
    for (uint32_t gi = 0; gi < 4; ++gi) {
        if (filteredProgramGiStd_[gi].shaderCount == 0 && !filteredVsModule.empty() &&
            !litFragGiStdModules_[gi].empty()) {
            smvk::Shader vs{.module = filteredVsModule,
                            .entryPoint = "mainVS",
                            .stage = smvk::ShaderStage::Vertex};
            smvk::Shader fs{.module = litFragGiStdModules_[gi],
                            .entryPoint = "mainPS",
                            .stage = smvk::ShaderStage::Fragment};
            filteredProgramGiStd_[gi] = {.shaders = {vs, fs}, .shaderCount = 2};
        }
    }
    // PROJECT-LIT family twins (the project-authored lit FS modules
    // the app configured — the same modules the direct draws bind as
    // the mainProject* family programs). Built only for configured
    // modules; the family picks fall back to the stock picks when a
    // twin is missing (a project that compiled no family at all).
    if (filteredProjectProgram.shaderCount == 0 && !filteredVsModule.empty() &&
        !projectFrag_.empty()) {
        smvk::Shader vs{
            .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
        smvk::Shader fs{
            .module = projectFrag_, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
        filteredProjectProgram = {.shaders = {vs, fs}, .shaderCount = 2};
    }
    if (filteredProjectProgramA2C.shaderCount == 0 && !filteredVsModule.empty() &&
        !projectA2CFrag_.empty()) {
        smvk::Shader vs{
            .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
        smvk::Shader fs{.module = projectA2CFrag_,
                        .entryPoint = "mainPS",
                        .stage = smvk::ShaderStage::Fragment};
        filteredProjectProgramA2C = {.shaders = {vs, fs}, .shaderCount = 2};
    }
    for (uint32_t li = 0; li < 4; ++li) {
        if (filteredProjectLitSg_[li].shaderCount == 0 && !filteredVsModule.empty() &&
            !projectLitFragSgModules_[li].empty()) {
            smvk::Shader vs{.module = filteredVsModule,
                            .entryPoint = "mainVS",
                            .stage = smvk::ShaderStage::Vertex};
            smvk::Shader fs{.module = projectLitFragSgModules_[li],
                            .entryPoint = "mainPS",
                            .stage = smvk::ShaderStage::Fragment};
            filteredProjectLitSg_[li] = {.shaders = {vs, fs}, .shaderCount = 2};
        }
    }
    for (uint32_t gi = 0; gi < 4; ++gi) {
        if (filteredProjectProgramGi_[gi].shaderCount == 0 && !filteredVsModule.empty() &&
            !projectLitFragGiModules_[gi].empty()) {
            smvk::Shader vs{.module = filteredVsModule,
                            .entryPoint = "mainVS",
                            .stage = smvk::ShaderStage::Vertex};
            smvk::Shader fs{.module = projectLitFragGiModules_[gi],
                            .entryPoint = "mainPS",
                            .stage = smvk::ShaderStage::Fragment};
            filteredProjectProgramGi_[gi] = {.shaders = {vs, fs}, .shaderCount = 2};
        }
    }
    if (filteredProjectProgramStd.shaderCount == 0 && !filteredVsModule.empty() &&
        !projectFragStd_.empty()) {
        smvk::Shader vs{
            .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
        smvk::Shader fs{.module = projectFragStd_,
                        .entryPoint = "mainPS",
                        .stage = smvk::ShaderStage::Fragment};
        filteredProjectProgramStd = {.shaders = {vs, fs}, .shaderCount = 2};
    }
    for (uint32_t gi = 0; gi < 4; ++gi) {
        if (filteredProjectProgramGiStd_[gi].shaderCount == 0 && !filteredVsModule.empty() &&
            !projectLitFragGiStdModules_[gi].empty()) {
            smvk::Shader vs{.module = filteredVsModule,
                            .entryPoint = "mainVS",
                            .stage = smvk::ShaderStage::Vertex};
            smvk::Shader fs{.module = projectLitFragGiStdModules_[gi],
                            .entryPoint = "mainPS",
                            .stage = smvk::ShaderStage::Fragment};
            filteredProjectProgramGiStd_[gi] = {.shaders = {vs, fs}, .shaderCount = 2};
        }
    }
    // Debug programs (self-contained; failure only disables debug views).
    if (meshDebugProgram.shaderCount == 0 || lineProgram.shaderCount == 0) {
        try {
            if (meshDebugVsModule.empty())
                meshDebugVsModule =
                    ctx.CreateShaderModule(smvk::shaders::Read(kMeshDebugVsPath), "meshDebugVS");
            if (meshDebugFragModule.empty())
                meshDebugFragModule =
                    ctx.CreateShaderModule(smvk::shaders::Read(kMeshDebugFragPath), "meshDebugFS");
            if (lineVsModule.empty())
                lineVsModule =
                    ctx.CreateShaderModule(smvk::shaders::Read(kLineVsPath), "debugLineVS");
            if (lineFragModule.empty())
                lineFragModule =
                    ctx.CreateShaderModule(smvk::shaders::Read(kLineFragPath), "debugLineFS");
        } catch (...) {
            return true; // filter works; debug views stay off
        }
        if (!meshDebugVsModule.empty() && !meshDebugFragModule.empty()) {
            smvk::Shader vs{.module = meshDebugVsModule,
                            .entryPoint = "mainVS",
                            .stage = smvk::ShaderStage::Vertex};
            smvk::Shader fs{.module = meshDebugFragModule,
                            .entryPoint = "mainPS",
                            .stage = smvk::ShaderStage::Fragment};
            meshDebugProgram = {.shaders = {vs, fs}, .shaderCount = 2};
        }
        if (!lineVsModule.empty() && !lineFragModule.empty()) {
            smvk::Shader vs{
                .module = lineVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
            smvk::Shader fs{.module = lineFragModule,
                            .entryPoint = "mainPS",
                            .stage = smvk::ShaderStage::Fragment};
            lineProgram = {.shaders = {vs, fs}, .shaderCount = 2};
        }
    }
    // Deformed cluster bounds (skinned cull fix): the per-flight
    // skinBounds ring + the GfxSkinBounds compute program. Best-effort —
    // a missing .spv only disables the pass, and the batch cull then
    // reads the static (bind-pose) table as before.
    (void)smvk::gfx::EnsureSkinBounds(gfx, ctx, rg);
    return true;
}

smvk::ShaderProgram GfxRenderer::FilteredProgramWith(
    smvk::ResourceManager& ctx,
    const smvk::Holder<smvk::ShaderModuleHandle>& frag) {
    // Default-init, not value-init: ShaderProgram is a fully
    // defaulted aggregate (every member carries an NSDMI), so both
    // spellings are semantically identical - but GCC 13 ICEs in
    // gimplify on the explicit `p{}` (array NSDMI + empty
    // brace-init-list, gimplify.cc:3059). MSVC/Clang take either.
    smvk::ShaderProgram p;
    if (frag.empty())
        return p;
    // On-demand VS load (best-effort, cached — the lit path may not
    // have needed the module yet): a failure leaves shaderCount 0 and
    // the caller falls back.
    if (filteredVsModule.empty()) {
        try {
            filteredVsModule = ctx.CreateShaderModule(
                smvk::shaders::Read(smvk::gfx::kGfxFilteredVsPath), "gfxFilteredVS");
        } catch (...) {
            return p;
        }
    }
    if (filteredVsModule.empty())
        return p;
    smvk::Shader vs{
        .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
    smvk::Shader fs{.module = frag, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
    return {.shaders = {vs, fs}, .shaderCount = 2};
}

std::vector<smvk::ShaderModuleHandle> GfxRenderer::ReleaseSelfModules(const SelfModuleSet& which) {
    std::vector<smvk::ShaderModuleHandle> retired;
    auto release = [&retired](smvk::Holder<smvk::ShaderModuleHandle>& holder) {
        if (!holder.empty())
            retired.push_back(holder.release());
    };
    if (which.filteredVs)
        release(filteredVsModule);
    return retired;
}

void GfxRenderer::AppendHizPasses(smvk::FrameContext& base,
                                  smvk::LogicalImageViewHandle depthLogical,
                                  uint32_t depthBindlessIndex,
                                  const char* passName) {
    AppFrameContext& fc = static_cast<AppFrameContext&>(base);
    if (lastSlot_ == UINT32_MAX || lastSlot_ >= smvk::gfx::ViewCount(gfx))
        return;
    const uint32_t slot = lastSlot_;
    const smvk::gfx::TemporalView& t = smvk::gfx::Temporal(gfx, slot);
    // Current flight's pyramid (same rule as every other ringed buffer;
    // slices can disagree briefly across a resize before all rebuild).
    if (t.hiz[fc.frameIndex].mipCount == 0)
        return;

    SMVK_PROFILE_RENDER("GfxAppendHizPasses");
    SMVK_PROFILE_TAG("mipCount", t.hiz[fc.frameIndex].mipCount);

    GfxRenderer* self = this;
    smvk::ResourceManager* ctx = fc.ctx;
    // SPD path selection (hzb_spd .min — the single-dispatch twin).
    // Extents beyond 4096 exceed the tested 64x64-tile upper-level
    // capacity (AMD SPD's documented limit) — always the per-mip path.
    // Verify mode forces the CULL CHAIN onto the proven per-mip
    // pyramid and runs SPD into the hizVerify shadow: a wrong MIN
    // pyramid must not eat geometry while it is being validated.
    const uint32_t hizW = t.hiz[fc.frameIndex].width;
    const uint32_t hizH = t.hiz[fc.frameIndex].height;
    const bool spdExtentOk = hizW <= 4096u && hizH <= 4096u;
    const bool spdProgramOk = gfx.hizSpdProgram.shaderCount == 1;
    const bool spdCounterOk = !t.hizSpdCounter.empty() && !t.hizSpdCounterLogical.empty();
    const bool spdLive =
        gfx.hizSpd && !gfx.hizSpdVerify && spdProgramOk && spdExtentOk && spdCounterOk;
    if (spdLive) {
        // ONE dispatch replaces the per-mip loop; the node keeps the
        // caller's name (GfxHiz / GfxHizRefresh) so the two build
        // sites stay distinguishable — the NODE COUNT (1 vs ~12) is
        // the A/B signal in framegraph timings.
        smvk::RenderPass& spdPass = smvk::gfx::AddHizSpdPass(
            gfx, *fc.rg, slot, depthLogical, passName ? passName : "GfxHiz");
        spdPass.SetRecordingFunc([self, ctx, slot, depthBindlessIndex](smvk::CommandBuffer& cmd) {
            ::smvk::profiler::GpuPassScope hizSpdScope("GfxHizSpd", __FILE__, __LINE__);
            SMVK_PROFILE_TAG("spd", 1);
            smvk::gfx::PushAndDispatchHizSpd(
                self->gfx, cmd, *ctx, slot, depthBindlessIndex, /*shadow=*/false);
        });
    } else {
        for (uint32_t m = 0; m < t.hiz[fc.frameIndex].mipCount; ++m) {
            const bool fromDepth = (m == 0);
            smvk::RenderPass& hizPass =
                smvk::gfx::AddHizPass(gfx,
                                      *fc.rg,
                                      slot,
                                      m,
                                      fromDepth ? depthLogical : smvk::LogicalImageViewHandle{},
                                      passName ? passName : "GfxHiz");
            hizPass.SetRecordingFunc(
                [self, ctx, slot, m, depthBindlessIndex, fromDepth](smvk::CommandBuffer& cmd) {
                    // Per-mip GPU scope (the pass-level scope uses the shared
                    // "GfxHiz" name). Static literals: Optick stores the name
                    // pointer, so run-time names must have static storage.
                    static const char* const kHizMipGpuNames[16] = {
                        "GfxHizMip0",
                        "GfxHizMip1",
                        "GfxHizMip2",
                        "GfxHizMip3",
                        "GfxHizMip4",
                        "GfxHizMip5",
                        "GfxHizMip6",
                        "GfxHizMip7",
                        "GfxHizMip8",
                        "GfxHizMip9",
                        "GfxHizMip10",
                        "GfxHizMip11",
                        "GfxHizMip12",
                        "GfxHizMip13",
                        "GfxHizMip14",
                        "GfxHizMip15",
                    };
                    ::smvk::profiler::GpuPassScope hizMipScope(
                        kHizMipGpuNames[m < 16 ? m : 15], __FILE__, __LINE__);
                    SMVK_PROFILE_TAG("mip", m);
                    smvk::gfx::PushAndDispatchHiz(
                        self->gfx, cmd, *ctx, slot, m, fromDepth ? depthBindlessIndex : UINT32_MAX);
                });
        }
    }
    // Verify shadow (TOURMALINE_HIZ_SPD_VERIFY): the SPD twin into
    // this flight's hizVerify slice from the SAME seed depth the
    // per-mip path above consumed; gfx::VerifyHizSpd byte-compares
    // the two post-Execute. Both builds read the depth view and
    // write disjoint buffers — the graph orders them freely.
    if (gfx.hizSpdVerify && spdProgramOk && spdExtentOk && spdCounterOk &&
        t.hizVerify[fc.frameIndex].mipCount != 0) {
        smvk::RenderPass& vPass = smvk::gfx::AddHizSpdPass(
            gfx, *fc.rg, slot, depthLogical, "GfxHizSpdVerify", /*shadow=*/true);
        vPass.SetRecordingFunc([self, ctx, slot, depthBindlessIndex](smvk::CommandBuffer& cmd) {
            ::smvk::profiler::GpuPassScope hizSpdVScope("GfxHizSpdVerify", __FILE__, __LINE__);
            smvk::gfx::PushAndDispatchHizSpd(
                self->gfx, cmd, *ctx, slot, depthBindlessIndex, /*shadow=*/true);
        });
        gfx.hizSpdVerifyArmed = true;
    }
}

bool GfxRenderer::PrepareGpuSceneHiz(smvk::ResourceManager& ctx,
                                     smvk::RenderGraph& rg,
                                     const smvk::RenderView& view,
                                     const glm::vec3& eye,
                                     uint32_t flight,
                                     uint32_t width,
                                     uint32_t height) {
    lastSlot_ = UINT32_MAX;
    smvk::gfx::BeginFrame(gfx);
    const uint32_t slot =
        smvk::gfx::BeginView(gfx, ctx, rg, view, eye, config.cullFlags, width, height);
    if (slot == UINT32_MAX)
        return false;
    // BeginView has no flight argument; FinalizeView normally stamps it
    // for GfxFilter. The GPU-only path skips FinalizeView entirely.
    gfx.views[slot].frameIndex = flight;
    if (!smvk::gfx::EnsureHiz(gfx, ctx, rg, slot, width, height))
        return false;
    lastSlot_ = slot;
    return true;
}

void GfxRenderer::VerifyHizSpd(smvk::ResourceManager& ctx) {
    if (lastSlot_ == UINT32_MAX || lastSlot_ >= smvk::gfx::ViewCount(gfx))
        return;
    smvk::gfx::VerifyHizSpd(gfx, ctx, lastSlot_);
}

void GfxRenderer::RegisterWorldMeshes(smvk::World& world) {
    meshMap_.assign(world.meshLocations.size(), UINT32_MAX);
    lodMeshMap_.clear();
    lodMeshMap_.resize(world.meshLocations.size());
    uint32_t fromCooked = 0, fromUpload = 0, skipped = 0;
    uint64_t skippedTris = 0;
    // Dedupe: distinct base meshes can share one LOD sibling URI, and the
    // same base mesh is never registered twice. Key = owning cluster +
    // pool index (stable for the register pass; all clusters are alive).
    struct LocKey {
        const void* cluster = nullptr;
        uint32_t index = 0;
        bool operator==(const LocKey& o) const {
            return cluster == o.cluster && index == o.index;
        }
    };
    struct LocHash {
        size_t operator()(const LocKey& k) const noexcept {
            return std::hash<const void*>{}(k.cluster) * 31u + k.index;
        }
    };
    std::unordered_map<LocKey, uint32_t, LocHash> done;
    auto registerOne = [&](const smvk::World::MeshLocation& loc) -> uint32_t {
        if (!loc.cluster || !loc.handle.valid())
            return UINT32_MAX;
        const LocKey key{loc.cluster, loc.handle.index()};
        if (auto it = done.find(key); it != done.end())
            return it->second;
        const smvk::StaticMesh* mesh = loc.cluster->staticMeshes.get(loc.handle);
        if (!mesh || mesh->geometry.surfaces.empty())
            return UINT32_MAX;
        const uint32_t baseVertex =
            (uint32_t)std::max<int32_t>(mesh->geometry.surfaces[0].baseVertex, 0);
#if 0 // TMP-DIAG scanlands hunt (disabled): per-mesh registration + meshlet validation dumps.
      // Remove later.
			{
				std::string uri;
				if (loc.cluster)
					uri = loc.cluster->GetMeshUri(loc.handle).Str();
				std::fprintf(stderr, "[TMP-DIAG] reg pool=%u uri=%s baseVertex=%d indexCount=%u vertCount=%u hasMeshlets=%d\n",
					loc.handle.index(), uri.c_str(), mesh->geometry.surfaces.empty() ? -999 : mesh->geometry.surfaces[0].baseVertex,
					mesh->geometry.indexCount, mesh->geometry.vertexCount,
					(mesh->meshlets && !mesh->meshlets->empty()) ? 1 : 0);
			}
			if (mesh->meshlets && !mesh->meshlets->empty()) {
				const smvk::CookedMeshlets& cm = *mesh->meshlets;
				const uint32_t vcount = mesh->geometry.vertexCount;
				size_t bad = 0, badB = 0;
				float mxr = 0.0f; double sumr = 0.0; size_t big = 0;
				auto badf = [](float v) { return !(v == v) || v == std::numeric_limits<float>::infinity() || v == -std::numeric_limits<float>::infinity(); };
				for (size_t mi = 0; mi < cm.meshlets.size(); ++mi) {
					const smvk::Meshlet& ml = cm.meshlets[mi];
					mxr = (ml.boundRadius > mxr) ? ml.boundRadius : mxr;
					sumr += ml.boundRadius;
					if (ml.boundRadius > 20.0f) ++big;
					if (badf(ml.boundCenter.x) || badf(ml.boundCenter.y) || badf(ml.boundCenter.z)
						|| badf(ml.boundRadius) || badf(ml.coneApex.x) || badf(ml.coneApex.y)
						|| badf(ml.coneApex.z) || badf(ml.coneAxis.x) || badf(ml.coneAxis.y)
						|| badf(ml.coneAxis.z) || badf(ml.coneCutoff))
						++badB;
					if (ml.vertexOffset + ml.vertexCount > cm.vertices.size()
						|| ml.triangleOffset + ml.triangleCount * 3u > cm.triangles.size())
						++bad;
					else {
						for (uint32_t vi = 0; vi < ml.vertexCount; ++vi)
							if (cm.vertices[ml.vertexOffset + vi] >= vcount) { ++bad; break; }
						for (uint32_t ti = 0; ti < ml.triangleCount * 3u; ++ti)
							if (cm.triangles[ml.triangleOffset + ti] >= ml.vertexCount) { ++bad; break; }
					}
				}
				if (bad > 0 || badB > 0) {
					std::string uri;
					if (loc.cluster)
						uri = loc.cluster->GetMeshUri(loc.handle).Str();
					std::fprintf(stderr, "[TMP-DIAG] meshlets CORRUPT/NAN uri=%s vcount=%u meshlets=%zu bad=%zu badB=%zu\n",
						uri.c_str(), vcount, cm.meshlets.size(), bad, badB);
				}
				if (cm.meshlets.size() > 100) {
					std::string uri3;
					if (loc.cluster)
						uri3 = loc.cluster->GetMeshUri(loc.handle).Str();
					std::fprintf(stderr, "[TMP-DIAG] meshletr uri=%s n=%zu maxR=%.1f meanR=%.1f big=%zu\n",
						uri3.c_str(), cm.meshlets.size(), mxr, sumr / (double)cm.meshlets.size(), big);
				}
			}
#endif
        uint32_t gfxMesh = UINT32_MAX;
        if (mesh->meshlets && !mesh->meshlets->empty()) {
            gfxMesh = smvk::gfx::RegisterMeshFromCooked(gfx, *mesh->meshlets, baseVertex);
            if (gfxMesh != UINT32_MAX)
                ++fromCooked;
        } else if (const smvk::StaticMeshUpload* retained =
                       loc.cluster->GetRetainedUpload(loc.handle)) {
            gfxMesh = smvk::gfx::RegisterMeshFromUpload(gfx, *retained, baseVertex);
            if (gfxMesh != UINT32_MAX)
                ++fromUpload;
        }
        if (gfxMesh == UINT32_MAX)
            skippedTris += mesh->geometry.indexCount / 3;
        // TMP-DIAG (TOURMALINE_DUMPGFX, the LOD-sibling draw-drop hunt): the
        // runtime vertex window vs the cooked-meshlet lifted-id coverage per
        // registered gfx mesh. The CPU/GPU cull chains and the filter CS all
        // consume the lifted global ids (RegisterMeshFromCooked lifts
        // window[tri] by surfaces[0].baseVertex); ids that fall outside
        // [baseVertex, baseVertex + vertexCount) fetch OTHER meshes'
        // positions/attributes — triangles that pass every cull (bounds are
        // cooked from the local geometry, still correct) yet rasterize
        // nowhere near the object (the distant-sibling black band). The
        // window-id audit catches it at the source: cooked.vertices[] must
        // stay under the MESH's vertexCount. Remove later.
        if (gfxMesh != UINT32_MAX && std::getenv("TOURMALINE_DUMPGFX")) {
            std::string uri =
                loc.cluster ? loc.cluster->GetMeshUri(loc.handle).Str() : std::string{};
            const uint32_t vcount = mesh->geometry.vertexCount;
            const int32_t baseVertexI =
                mesh->geometry.surfaces.empty() ? -1 : mesh->geometry.surfaces[0].baseVertex;
            uint32_t idMin = UINT32_MAX, idMax = 0;
            uint32_t kept = 0, skippedMl = 0;
            size_t badWindow = 0, badTri = 0;
            if (mesh->meshlets && !mesh->meshlets->empty()) {
                const smvk::CookedMeshlets& cm = *mesh->meshlets;
                for (const smvk::Meshlet& m : cm.meshlets) {
                    if (m.triangleCount == 0 || m.triangleCount > 64 || m.vertexCount == 0 ||
                        m.vertexOffset + m.vertexCount > cm.vertices.size() ||
                        m.triangleOffset + m.triangleCount * 3u > cm.triangles.size()) {
                        ++skippedMl;
                        continue;
                    }
                    ++kept;
                    for (uint32_t v = 0; v < m.vertexCount; ++v)
                        if (cm.vertices[m.vertexOffset + v] >= vcount)
                            ++badWindow;
                    for (uint32_t t = 0; t < m.triangleCount * 3u; ++t) {
                        const uint8_t local = cm.triangles[m.triangleOffset + t];
                        if (local >= m.vertexCount) {
                            ++badTri;
                            continue;
                        }
                        const uint32_t id = (uint32_t)std::max<int32_t>(baseVertexI, 0) +
                                            cm.vertices[m.vertexOffset + local];
                        idMin = std::min(idMin, id);
                        idMax = std::max(idMax, id);
                    }
                }
            }
            const auto& mb = gfx.meshBounds[gfxMesh];
            const auto& me = gfx.meshes[gfxMesh];
            std::fprintf(stderr,
                         "[TMP-DIAG] gfx=%u uri=%s baseVertex=%d vcount=%u "
                         "meshlets=%u/%u ids=[%u..%u] badWindow=%zu badTri=%zu "
                         "clusters=%u union=(%.2f,%.2f,%.2f r=%.2f) "
                         "aabb=[%.2f,%.2f,%.2f..%.2f,%.2f,%.2f]\n",
                         gfxMesh,
                         uri.c_str(),
                         baseVertexI,
                         vcount,
                         kept,
                         kept + skippedMl,
                         idMin,
                         idMax,
                         badWindow,
                         badTri,
                         me.clusterCount,
                         mb.x,
                         mb.y,
                         mb.z,
                         mb.w,
                         mesh->geometry.aabbMin.x,
                         mesh->geometry.aabbMin.y,
                         mesh->geometry.aabbMin.z,
                         mesh->geometry.aabbMax.x,
                         mesh->geometry.aabbMax.y,
                         mesh->geometry.aabbMax.z);
            // TMP-DIAG (extends DUMPGFX): per-cluster shape summary for
            // skinned meshes (the knight) — the RUNTIME cluster table's
            // radii vs the cook's dashboard (a scrambled round-trip shows
            // maxR/meanR ~ the whole mesh; tiny split fragments show small
            // radii with many clusters). Remove later.
            if (mesh->skinned) {
                float rMin = 1e30f, rMax = 0.0f;
                double rSum = 0.0;
                uint32_t rN = 0, tMin = 4096, tMax = 0;
                double tSum = 0.0;
                for (uint32_t ci = 0; ci < me.clusterCount; ++ci) {
                    const smvk::gfx::Cluster& c = gfx.clusters[me.clusterStart + ci];
                    rMin = std::min(rMin, c.boundRadius);
                    rMax = std::max(rMax, c.boundRadius);
                    rSum += (double)c.boundRadius;
                    ++rN;
                    tMin = std::min(tMin, c.triCount);
                    tMax = std::max(tMax, c.triCount);
                    tSum += (double)c.triCount;
                }
                if (rN > 0)
                    std::fprintf(stderr,
                                 "[TMP-DIAG] gfx=%u runtimeClusters=%u "
                                 "R[min=%.3f mean=%.3f max=%.3f] "
                                 "tris[min=%u mean=%.1f max=%u]\n",
                                 gfxMesh,
                                 rN,
                                 rMin,
                                 rSum / (double)rN,
                                 rMax,
                                 tMin,
                                 tSum / (double)rN,
                                 tMax);
            }
        }
        done.emplace(key, gfxMesh);
        return gfxMesh;
    };
    // Every LOD sibling gets its own gfx mesh so FilterMainView can push
    // the selected LOD into the HZB-culled indirect ranges (previously
    // only LOD0 was registered and LOD>0 drew direct, unculled).
    // TMP-DIAG (TOURMALINE_DUMPGFX): chain length + per-level world
    // error for the first meshes (the LOD-selection inputs: zero
    // errors pin everything at LOD0 regardless of budget). Remove later.
    if (std::getenv("TOURMALINE_DUMPGFX")) {
        for (size_t i = 0; i < world.meshLocations.size() && i < 8; ++i) {
            const uint32_t cl = world.LodChainLength((uint32_t)i);
            std::string es;
            for (uint32_t l = 0; l < cl && l < 8; ++l) {
                char b[64];
                std::snprintf(b, sizeof(b), " %.4g", (double)world.LodErrorAt((uint32_t)i, l));
                es += b;
            }
            std::fprintf(
                stderr, "[TMP-DIAG] chain worldMesh=%zu len=%u err=[%s ]\n", i, cl, es.c_str());
        }
    }
    for (size_t i = 0; i < world.meshLocations.size(); ++i) {
        const uint32_t chainLen = world.LodChainLength((uint32_t)i);
        if (chainLen <= 1) {
            const uint32_t g = registerOne(world.meshLocations[i]);
            if (g == UINT32_MAX)
                ++skipped;
            meshMap_[i] = g;
            lodMeshMap_[i] = {g};
            continue;
        }
        std::vector<uint32_t> perLod;
        perLod.reserve(chainLen);
        for (uint32_t l = 0; l < chainLen; ++l) {
            auto hit = world.ResolveLodMesh((uint32_t)i, l);
            const uint32_t g = hit ? registerOne(*hit) : UINT32_MAX;
            if (g == UINT32_MAX)
                ++skipped;
            perLod.push_back(g);
        }
        meshMap_[i] = perLod.empty() ? UINT32_MAX : perLod[0];
        lodMeshMap_[i] = std::move(perLod);
    }
    SMVK_DEBUG_LOG(
        "[GfxFilter] meshes: %u cooked + %u runtime-cooked = %u filtered, %u direct (%llu tris)\n",
        fromCooked,
        fromUpload,
        fromCooked + fromUpload,
        skipped,
        (unsigned long long)skippedTris);
    SMVK_DEBUG_LOG("[GfxFilter] clusters: %u, filter indices: %llu\n",
                   smvk::gfx::ClusterCount(gfx),
                   (unsigned long long)gfx.filterIndices.size());
}

void GfxRenderer::UploadStaticData(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) {
    smvk::gfx::UploadStaticData(gfx, ctx, rg);
}

// CULL BASELINE DUMP (TOURMALINE_CULL_DUMP): reads the main view's
// draw-cull verdict buffer back (word i = 1 visible / 0 culled, see
// gfxdrawcull.cs.hlsl) plus the finalized filter stats and writes the
// canonical camera-locked JSON the perf harness compares against its
// tracked baselines (the correctness gate for GPU-driven renderer
// changes). Debug one-shot download: accepts one flight of staleness —
// the pinned bench camera is static, so the verdicts are identical.
static void DumpCullBaseline(smvk::gfx::State& s,
                             uint32_t view,
                             uint32_t frameIndex,
                             smvk::ResourceManager& ctx,
                             uint64_t appFrame) {
    smvk::gfx::View& v = s.views[view];
    const uint32_t drawCount = v.stats.draws;
    // Runtime cull-state word count (the dynamic cutover — the slice is
    // drawCapacity + 5 words: verdicts + the counter/diag suffix).
    const size_t wordCount = smvk::gfx::CullStateWords(s, view, frameIndex);
    std::vector<uint32_t> words(wordCount, 0u);
    if (drawCount == 0 || drawCount > smvk::gfx::kDeclaredMaxDraws ||
        v.cullStateBuffers[frameIndex].empty() ||
        !ctx.DownloadDataFromBuffer(
            v.cullStateBuffers[frameIndex], words.data(), wordCount * sizeof(uint32_t))) {
        std::fprintf(stderr,
                     "[CullDump] FAILED: verdict readback unavailable "
                     "(drawCount=%u)\n",
                     drawCount);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories("exports", ec);
    char name[64];
    std::snprintf(name, sizeof(name), "exports/cull_dump_%llu.json", (unsigned long long)appFrame);
    FILE* f = std::fopen(name, "wb");
    if (!f) {
        std::fprintf(stderr, "[CullDump] FAILED: cannot open %s\n", name);
        return;
    }
    uint32_t visible = 0;
    std::fprintf(f,
                 "{\"schema_version\":1,\"frame\":%llu,\"view\":%u,"
                 "\"draw_count\":%u,\"visible_draws\":[",
                 (unsigned long long)appFrame,
                 (unsigned)view,
                 drawCount);
    for (uint32_t i = 0; i < drawCount; ++i)
        if (words[i] != 0u) {
            std::fprintf(f, "%s%u", visible ? "," : "", i);
            ++visible;
        }
    std::fprintf(f, "],\"culled_draws\":[");
    bool first = true;
    for (uint32_t i = 0; i < drawCount; ++i)
        if (words[i] == 0u) {
            std::fprintf(f, "%s%u", first ? "" : ",", i);
            first = false;
        }
    std::fprintf(f,
                 "],\"stats\":{\"draws\":%u,\"commands\":%u,"
                 "\"triangles_processed\":%u,\"clusters_processed\":%u,"
                 "\"clusters_culled\":%u}}\n",
                 v.stats.draws,
                 v.stats.commands,
                 v.stats.trianglesProcessed,
                 v.stats.clustersProcessed,
                 v.stats.clustersCulled);
    std::fclose(f);
    std::fprintf(stderr, "[CullDump] wrote %s (%u draws, %u visible)\n", name, drawCount, visible);
}

uint64_t GfxRenderer::EnsureChartXforms(smvk::World& world, smvk::ResourceManager& ctx) {
    // A new world can have the same revision as an older one. Never
    // reuse chart data uploaded for a different scene.
    const bool tableChanged = lmXformsWorld_ != &world || world.lmChartVersion != lmXformsVersion_;
    if (!tableChanged && !lmXformsBuf_.empty())
        return lmXformsAddr_;
    lmXformsWorld_ = &world;
    lmXformsVersion_ = world.lmChartVersion;
    const size_t count = std::max<size_t>(world.lmChartTable.size(), 1);
    const size_t bytes = count * sizeof(glm::vec4);
    // Allocate a fresh buffer when chart data changes. In-place uploads
    // can overwrite transforms still read by in-flight GPU frames.
    // The prior holder retires through the resource manager.
    if (tableChanged || lmXformsBuf_.empty() || lmXformsCap_ < bytes) {
        lmXformsBuf_ = ctx.CreateBuffer(bytes, smvk::BufferType_Storage, "lm chart xforms");
        lmXformsCap_ = bytes;
    }
    if (lmXformsBuf_.empty()) {
        lmXformsAddr_ = 0;
        return 0;
    }
    if (!world.lmChartTable.empty()) {
        ctx.UploadDataToBuffer(lmXformsBuf_,
                               world.lmChartTable.data(),
                               bytes,
                               smvk::PipelineStage(smvk::PipelineStageBit_Vertex),
                               smvk::AccessFlags::ShaderRead);
    }
    lmXformsAddr_ = ctx.GetBufferDeviceAddress(lmXformsBuf_);
    return lmXformsAddr_;
}

void GfxRenderer::DumpHzb(smvk::ResourceManager& ctx, uint32_t slot) {
    smvk::gfx::DumpHzb(gfx, ctx, slot);
}

} // namespace engine
