// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// This UI is compiled only into the standalone App host, not the renderer.
// WorldRenderer exposes renderer state through a host-supplied UI callback.
#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/RendererLimits.h>
#include <Engine/Renderer/WorldRenderer.h>
#include <Engine/Renderer/gfx_filter.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Rendergraph/volumetric.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace engine {
// Mirror the renderer's shared UI bounds. Keep in sync with
// WorldRendererPassDescriptors.inl when modifying the renderer presets.
static constexpr uint32_t NUM_CASCADES = 4;
static constexpr float RENDER_SCALE_MIN = 0.25f;
static constexpr float RENDER_SCALE_MAX = 2.0f;
static constexpr float UI_SCALE_MIN = 0.5f;
static constexpr float UI_SCALE_MAX = 2.0f;
static constexpr float UI_DPI_MIN = 0.5f;
static constexpr float UI_DPI_MAX = 3.0f;
static constexpr uint32_t FP_TILE_SIZE = 16;
static constexpr uint32_t FP_MAX_LIGHTS = engine::renderer::limits::MaxSimultaneousLights;
static constexpr uint32_t FP_MAX_DECALS = 256;
static constexpr uint32_t FP_MAX_DECALS_PER_TILE = 16;
static constexpr uint32_t PARTICLE_MAX_EMITTERS = 16;
void WorldRenderer::DrawAppSettingsUi(const DebugUiFrame& frame) {
    if (!frame.fb || !frame.env)
        return;
    smvk::Framebuffer& fb = *frame.fb;
    const smvk::EnvironmentSettings& frameEnv = *frame.env;
    const auto uiExtent = frame.uiExtent;
    const auto renderExtent = frame.renderExtent;
    const auto swapExtent = frame.swapExtent;
    const uint32_t cascadeViewBase = frame.cascadeViewBase;
    const uint32_t spotViewBase = frame.spotViewBase;
    const uint32_t pointViewBase = frame.pointViewBase;
    if (m_debugPanelOpen) {
        {
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const float pad = 8.0f;
            const float sideW = 480.0f;
            ImGui::SetNextWindowPos(ImVec2(disp.x - sideW - pad, pad), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(sideW, disp.y - pad * 2.0f), ImGuiCond_Always);
        }
        ImGui::Begin("Tourmaline - Debug",
                     &m_debugPanelOpen,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse);
        {
            // Live FPS/CPU/GPU readouts live in the text HUD (Ctrl+T)
            // now (DrawHudOverlay); the panel keeps controls only.
            if (ImGui::Button(m_showHud ? "Hide stats HUD (Ctrl+T)" : "Show stats HUD (Ctrl+T)"))
                m_showHud = !m_showHud;
        }
        if (ImGui::BeginTabBar("TourmalineTabs")) {
            // ---------------------------------------------------------
            // Quality: every setting that changes the image, in FLAT
            // sections (Display / Shadows / Lighting / Post Processing
            // / Atmosphere / Scene & Geometry) — no nested dropdowns,
            // no readbacks (numbers live in Stats; views in Debug).
            // ---------------------------------------------------------
            if (ImGui::BeginTabItem("Quality")) {
                // ---- Display: presentation, resolution, upscaling,
                // AA and sampling.
                ImGui::SeparatorText("Display");
                {
                    const char* modeNames[] = {"VSync FIFO (strict)",
                                               "VSync Mailbox (smooth)",
                                               "Uncapped Immediate",
                                               "VSync FIFO relaxed (no 30fps lock)"};
                    int modeIdx = static_cast<int>(m_host.GetPresentMode());
                    if (ImGui::Combo("Present mode", &modeIdx, modeNames, 4))
                        SetPresentMode(static_cast<smvk::PresentMode>(modeIdx));
                    ImGui::SliderFloat("Render scale",
                                       &m_renderScale,
                                       RENDER_SCALE_MIN,
                                       RENDER_SCALE_MAX,
                                       "%.2fx");
                    ImGui::SliderFloat(
                        "UI res scale", &m_uiScale, UI_SCALE_MIN, UI_SCALE_MAX, "%.2fx");
                    ImGui::SliderFloat(
                        "UI DPI scale", &m_uiDpiScale, UI_DPI_MIN, UI_DPI_MAX, "%.2fx");
                    if (ImGui::IsItemEdited() && m_uiTextureService.applyDpiScale)
                        m_uiTextureService.applyDpiScale(m_uiDpiScale);
                    // Conventional order for the combo; indices remap to the
                    // sl-aligned DlssMode enum values.
                    const char* dlssNames[] = {"Off",
                                               "Ultra Performance",
                                               "Performance",
                                               "Balanced",
                                               "Quality",
                                               "Ultra Quality",
                                               "DLAA"};
                    const int dlssEnumOf[] = {
                        static_cast<int>(smvk::DlssMode::Off),
                        static_cast<int>(smvk::DlssMode::UltraPerformance),
                        static_cast<int>(smvk::DlssMode::MaxPerformance),
                        static_cast<int>(smvk::DlssMode::Balanced),
                        static_cast<int>(smvk::DlssMode::MaxQuality),
                        static_cast<int>(smvk::DlssMode::UltraQuality),
                        static_cast<int>(smvk::DlssMode::DLAA),
                    };
                    int dlssSel = 0;
                    for (int i = 0; i < 7; ++i)
                        if (dlssEnumOf[i] == static_cast<int>(m_dlssMode))
                            dlssSel = i;
                    if (ImGui::Combo("DLSS mode", &dlssSel, dlssNames, 7))
                        m_dlssMode = static_cast<smvk::DlssMode>(dlssEnumOf[dlssSel]);
                    if (m_dlssFrameActive)
                        ImGui::Text("DLSS active: scene %ux%u -> swap %ux%u (phase %u/%u)",
                                    renderExtent.width,
                                    renderExtent.height,
                                    swapExtent.width,
                                    swapExtent.height,
                                    m_dlssPhase,
                                    m_dlssPhaseCount);
                    else if (DlssRequested()) {
                        // Why the requested mode is not running (one line).
                        if (!smvk::DlssAvailable())
                            ImGui::Text("DLSS inactive: Streamline runtime not loaded");
                        else if (!m_dlssSupported)
                            ImGui::Text("DLSS inactive: unsupported adapter");
                        else if (!m_passes.motionVectors)
                            ImGui::Text("DLSS inactive: needs the motion-vector pass");
                    }
                    // AA mode combo (Off / MSAA 2x / SMAA 1x / SMAA T2x);
                    // msaaEnabled derives from the mode every frame, so the
                    // combo is the single source of truth. DLSS replaces AA.
                    const char* aaNames[] = {
                        "Off", "MSAA 2x", "SMAA 1x", "SMAA T2x", "MSAA 2x Custom Resolve"};
                    int aaIdx = static_cast<int>(m_aaMode);
                    ImGui::BeginDisabled(m_dlssFrameActive);
                    if (ImGui::Combo("Antialiasing (Z / 1-4)", &aaIdx, aaNames, 5))
                        m_aaMode = static_cast<AaMode>(aaIdx);
                    ImGui::EndDisabled();
                    // Transparency technique for Blend materials.
                    const char* transpModes[] = {"Dithered (default)", "Alpha blend"};
                    int transpIdx = (m_transparentMode == smvk::TransparentMode::Blended) ? 1 : 0;
                    if (ImGui::Combo("Transparency", &transpIdx, transpModes, 2))
                        m_transparentMode = (transpIdx == 1) ? smvk::TransparentMode::Blended
                                                             : smvk::TransparentMode::Dithered;
                    // Texture filtering: applies to the bindless samplers
                    // (slots 0 + 2) via in-place recreate.
                    const char* filtNames[] = {"Trilinear",
                                               "Anisotropic 2x",
                                               "Anisotropic 4x",
                                               "Anisotropic 8x",
                                               "Anisotropic 16x"};
                    int filtIdx = static_cast<int>(m_textureFiltering);
                    if (ImGui::Combo("Texture filtering", &filtIdx, filtNames, 5))
                        SetTextureFiltering(
                            static_cast<TextureFiltering>(std::clamp(filtIdx, 0, 4)));
                }

                // ---- Shadows: directional cascades, local-light tiles
                // and the filtering mode.
                ImGui::SeparatorText("Shadows");
                {
                    // Tile resolutions (atlases recreate once on change via
                    // the per-frame Ensure; one-time hitch).
                    const uint32_t resOptions[] = {512, 1024, 2048, 4096};
                    const char* resNames[] = {"512", "1024", "2048", "4096"};
                    int resIdx = 1; // canon = 1024
                    for (int i = 0; i < 4; ++i)
                        if (resOptions[i] == m_cascadeResolution)
                            resIdx = i;
                    if (ImGui::Combo("Cascade resolution", &resIdx, resNames, 4))
                        m_cascadeResolution = resOptions[std::clamp(resIdx, 0, 3)];
                    // Local-light tile resolution (the SHARED spot + point
                    // atlas; PCF depth + the EVSM moment twin).
                    const uint32_t spotOptions[] = {256, 512, 1024};
                    const char* spotNames[] = {"256", "512 (PS5)", "1024"};
                    int spotIdx = 1; // canon = 512
                    for (int i = 0; i < 3; ++i)
                        if (spotOptions[i] == m_spotShadowResolution)
                            spotIdx = i;
                    if (ImGui::Combo("Spot/point tile resolution", &spotIdx, spotNames, 3))
                        m_spotShadowResolution = spotOptions[std::clamp(spotIdx, 0, 2)];
                    ImGui::SliderFloat("Split lambda", &m_cascadeLambda, 0.0f, 1.0f, "%.2f");
                    ImGui::Checkbox("SDSM (fit splits to measured depth)", &m_sdsmEnabled);
                    ImGui::SliderFloat("Shadow distance (m, 0 = scene far)",
                                       &m_cascadeMaxDistance,
                                       0.0f,
                                       500.0f,
                                       "%.0f");
                    ImGui::Text("Effective far: %.1f m (scene far %.1f m)",
                                (double)CascadeFar(),
                                (double)m_sceneFarClip);
                    ImGui::SliderFloat(
                        "Depth bias scale", &m_cascadeDepthBiasScale, 0.0f, 4.0f, "%.2f");
                    ImGui::SliderFloat(
                        "Normal bias scale", &m_cascadeNormalBiasScale, 0.0f, 4.0f, "%.2f");
                    ImGui::Separator();
                    ImGui::Checkbox("Spot shadows (per-light flag)", &m_spotShadowsEnabled);
                    int spotMax = m_spotShadowMax;
                    if (ImGui::SliderInt(
                            "Shadow spots max", &spotMax, 0, (int)smvk::kMaxSpotShadows))
                        m_spotShadowMax = spotMax;
                    ImGui::SliderFloat(
                        "Shadow max distance (m)", &m_spotShadowMaxDistance, 5.0f, 120.0f);
                    // Point lights: six cube-face tiles per selected light in
                    // the SAME atlas (PCF or the EVSM moments).
                    ImGui::Checkbox("Point shadows (per-light flag)", &m_pointShadowsEnabled);
                    int pointMax = m_pointShadowMax;
                    if (ImGui::SliderInt(
                            "Shadow points max", &pointMax, 0, (int)smvk::kMaxPointShadowLights))
                        m_pointShadowMax = pointMax;
                    ImGui::SliderFloat(
                        "Point shadow max distance (m)", &m_pointShadowMaxDistance, 5.0f, 120.0f);
                    ImGui::Separator();
                    // EVSM: RGBA32F 2xMSAA moments + async separable blur,
                    // 1-frame-lag pipelined. Off = PCF depth atlases.
                    ImGui::Checkbox("EVSM enabled", &m_evsmEnabled);
                    ImGui::SameLine();
                    ImGui::TextDisabled(EvsmActive() ? "(active)" : "(PCF fallback)");
                    int evsmPipe = (m_evsmPipeline == smvk::EvsmPipelineMode::SameFrame) ? 1 : 0;
                    if (ImGui::Combo("Pipeline", &evsmPipe, "One-frame lag\0Same-frame\0")) {
                        m_evsmPipeline = (evsmPipe == 1) ? smvk::EvsmPipelineMode::SameFrame
                                                         : smvk::EvsmPipelineMode::OneFrameLag;
                    }
                    ImGui::SliderFloat("Softness", &m_evsmSoftness, 0.0f, 3.0f);
                    ImGui::SliderFloat("Local light size (m)", &m_evsmSpotLightSize, 0.0f, 2.0f);
                }

                // ---- Lighting: ambient/GI, AO, specular IBL, SSR.
                ImGui::SeparatorText("Lighting");
                {
                    ImGui::SliderFloat("GI intensity", &m_giIntensity, 0.0f, 2.0f, "%.2f");
                    if (m_lmLoaded)
                        ImGui::Checkbox("Lightmaps", &m_lmEnabled);
                    // Screen-space ambient occlusion / SSGI chain.
                    static const char* const kAoNames[3] = {"No AO", "SSAO", "SAO"};
                    int aoIdx = static_cast<int>(m_aoMode);
                    if (ImGui::Combo("AO mode (O)", &aoIdx, kAoNames, 3))
                        m_aoMode = static_cast<AoMode>(std::clamp(aoIdx, 0, 2));
                    if (m_aoMode != AoMode::None) {
                        ImGui::SliderFloat("AO radius (m)", &m_ssaoRadius, 0.05f, 2.0f);
                        ImGui::SliderFloat("AO bias", &m_ssaoBias, 0.0f, 0.1f);
                        ImGui::SliderFloat("AO intensity", &m_ssaoIntensity, 0.0f, 2.0f);
                        ImGui::SliderFloat("AO max distance (m)", &m_ssaoMaxDistance, 2.0f, 100.0f);
                        ImGui::SliderFloat("AO blur sharpness", &m_ssaoBlurSharpness, 0.0f, 32.0f);
                        if (m_aoMode == AoMode::Ssao)
                            ImGui::SliderInt("SSAO samples", &m_ssaoSamples, 1, 32);
                        else {
                            ImGui::SliderInt("SAO samples", &m_saoSamples, 1, 32);
                            ImGui::SliderInt("SAO spiral turns", &m_saoSpiralTurns, 1, 16);
                        }
                    }
                    ImGui::Checkbox("Cubemap specular IBL", &m_cubemapEnabled);
                    ImGui::SliderFloat(
                        "Cubemap intensity", &m_cubemapIntensity, 0.0f, 4.0f, "%.2f");
                    ImGui::SliderFloat("Cubemap LOD bias", &m_cubemapLodBias, -2.0f, 4.0f, "%.2f");
                    if (!m_cubemapRows.empty()) {
                        int streamBudget = int(m_cubemapStreamBudget);
                        if (ImGui::SliderInt("Stream budget (nearest rows)", &streamBudget, 0, 64))
                            m_cubemapStreamBudget = uint32_t(std::clamp(streamBudget, 0, 64));
                    }
                    ImGui::Checkbox("Screen-space reflections (SSR)", &m_ssrEnabled);
                    if (m_ssrEnabled) {
                        ImGui::Checkbox("Static SDF off-screen reflections (experimental)",
                                        &m_staticSdfReflectionsEnabled);
                        if (m_staticSdfReflectionsEnabled)
                            ImGui::TextDisabled(
                                "GPU SDF: %u static opaque instances (baked sidecars only)",
                                m_staticSdfGpu.InstanceCount());
                        ImGui::SliderInt("Ray steps", &m_ssrSteps, 1, 128);
                        ImGui::SliderInt("Refine steps", &m_ssrRefineSteps, 0, 8);
                        ImGui::SliderFloat("Max distance (m)", &m_ssrMaxDistance, 1.0f, 200.0f);
                        ImGui::SliderFloat("Thickness (m)", &m_ssrThickness, 0.01f, 2.0f);
                        ImGui::SliderFloat("Intensity", &m_ssrIntensity, 0.0f, 4.0f);
                        ImGui::SliderFloat("F0 (glass 0.04)", &m_ssrF0, 0.0f, 1.0f);
                        ImGui::SliderFloat("Bilateral sharpness", &m_ssrBlurSharpness, 0.0f, 32.0f);
                        // Blur filter A/B (see smvk::SsrBlurMode): the 2D
                        // baseline vs the separable H+V candidate. A live
                        // flip resets the temporal history.
                        const char* blurModes[2] = {"2D bilateral (baseline)",
                                                    "Separable H+V (fast)"};
                        ImGui::Combo("Blur mode", &m_ssrBlurMode, blurModes, 2);
                    }
                    // Sun contact shadows (contact_shadow.h): the
                    // pre-Main depth raymarch toward the sun, min'd
                    // with the CSM/EVSM factor in the lit body - the
                    // contact-hardening detail the cascades cannot
                    // resolve. The full/half resolution combo is the
                    // A/B the tuning plan asks to benchmark (half
                    // saves the tracing but can lose thin occluders,
                    // which are the effect's point).
                    ImGui::Checkbox("Sun contact shadows", &m_contactShadowsEnabled);
                    if (m_contactShadowsEnabled) {
                        ImGui::SliderFloat(
                            "Contact distance (m)", &m_contactShadowMaxDistance, 0.05f, 2.0f);
                        ImGui::SliderFloat(
                            "Contact thickness (m)", &m_contactShadowThickness, 0.005f, 0.2f);
                        ImGui::SliderFloat(
                            "Contact start bias (m)", &m_contactShadowStartBias, 0.0f, 0.05f);
                        ImGui::SliderInt("Contact steps", &m_contactShadowSteps, 4, 32);
                        const char* contactRes[2] = {"Full", "Half"};
                        int contactResIdx = (m_contactShadowResolutionDivisor == 1) ? 0 : 1;
                        if (ImGui::Combo("Contact resolution", &contactResIdx, contactRes, 2))
                            m_contactShadowResolutionDivisor = (contactResIdx == 0) ? 1 : 2;
                    }
                    const bool raySupported = m_context && m_context->SupportsRayQueries();
                    if (!raySupported)
                        ImGui::BeginDisabled();
                    ImGui::Checkbox("Hardware ray-traced sun shadows (KHR ray query)",
                                    &m_rayTracedSunShadowsEnabled);
                    if (!raySupported) {
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "VK_KHR_acceleration_structure + VK_KHR_ray_query not available");
                        ImGui::EndDisabled();
                    } else if (m_rayTracedSunShadowsEnabled) {
                        if (m_context->SupportsOpacityMicromaps()) {
                            ImGui::Checkbox("Opacity micromaps (alpha MASK foliage)",
                                            &m_opacityMicromapsEnabled);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "VK_EXT_opacity_micromap: GPU-baked 16 microtriangles per "
                                    "masked triangle. Rebuilds BLAS/TLAS on changes.");
                        } else {
                            ImGui::TextDisabled(
                                "Opacity micromaps unsupported (VK_EXT_opacity_micromap)");
                        }
                        ImGui::SliderFloat("Hardware ray distance (m)",
                                           &m_rayTracedSunShadowDistance,
                                           1.0f,
                                           2000.0f);
                        ImGui::SliderFloat("Hardware ray origin bias (m)",
                                           &m_rayTracedSunShadowBias,
                                           0.001f,
                                           0.2f);
                        const char* rtRes[2] = {"Full", "Half"};
                        int rtResIdx = m_rayTracedSunShadowResolutionDivisor == 1 ? 0 : 1;
                        if (ImGui::Combo("Hardware ray resolution", &rtResIdx, rtRes, 2))
                            m_rayTracedSunShadowResolutionDivisor = rtResIdx == 0 ? 1 : 2;
                        // CPU snapshot from the last caster-list change. No readback,
                        // GPU query or extra draw pass is performed by this inspector.
                        if (ImGui::CollapsingHeader("TLAS / BLAS inspector")) {
                            const auto& summary = m_rayTracingDebugSummary;
                            ImGui::Text("Ray-query device: %s",
                                        raySupported ? "supported" : "unsupported");
                            ImGui::Text("GPU TLAS: %s",
                                        m_context->RayTracingWorldReady() ? "ready"
                                                                          : "not built / failed");
                            ImGui::Text("TLAS candidate instances: %zu (rejected: %u)",
                                        summary.instances.size(),
                                        summary.rejected);
                            ImGui::Text("Unique BLAS candidates: %zu", summary.blases.size());
                            ImGui::Text("Triangles: %llu instanced / %llu unique",
                                        (unsigned long long)summary.instancedTriangles,
                                        (unsigned long long)summary.uniqueTriangles);
                            ImGui::TextDisabled(
                                "CPU candidates, not GPU BVH nodes, memory sizes or timings.");
                            if (ImGui::TreeNode("BLAS geometry")) {
                                const size_t shown = std::min<size_t>(summary.blases.size(), 256);
                                for (size_t i = 0; i < shown; ++i) {
                                    const auto& blas = summary.blases[i];
                                    ImGui::BulletText("#%zu: %u triangles, %u TLAS refs%s",
                                                      i,
                                                      blas.triangles,
                                                      blas.references,
                                                      blas.alphaMasked ? " (alpha MASK)" : "");
                                }
                                if (shown < summary.blases.size())
                                    ImGui::TextDisabled("Showing first %zu of %zu BLAS",
                                                        shown,
                                                        summary.blases.size());
                                ImGui::TreePop();
                            }
                            if (ImGui::TreeNode("TLAS instance transforms")) {
                                const size_t shown =
                                    std::min<size_t>(summary.instances.size(), 256);
                                for (size_t i = 0; i < shown; ++i) {
                                    const auto& entry = summary.instances[i];
                                    const auto& src =
                                        m_rayTracingCachedInstances[entry.sourceIndex];
                                    ImGui::BulletText(
                                        "#%zu -> BLAS %u (%u triangles), pos (%.2f, %.2f, %.2f)",
                                        i,
                                        entry.blasIndex,
                                        entry.triangles,
                                        src.transform[3],
                                        src.transform[7],
                                        src.transform[11]);
                                }
                                if (shown < summary.instances.size())
                                    ImGui::TextDisabled("Showing first %zu of %zu instances",
                                                        shown,
                                                        summary.instances.size());
                                ImGui::TreePop();
                            }
                        }
                    }
                    // Diffuse screen-space GI (see ssgi_diffuse.h): one
                    // cosine-hemisphere bounce that REPLACES the diffuse
                    // ambient slice (never an additive stack over the
                    // baked GI). Independent of SSR/AO; off under MSAA.
                    // Default OFF (experimental v1).
                    // Labels are "SSGI diffuse"-prefixed on purpose: the
                    // bare "GI intensity" collided with the baked-GI
                    // slider above (same Lighting section -> one ImGui
                    // ID hash, the second widget misbehaves).
                    ImGui::Checkbox("Diffuse screen-space GI (SSGI)", &m_ssgiDiffuseEnabled);
                    if (m_ssgiDiffuseEnabled) {
                        ImGui::SliderFloat(
                            "SSGI diffuse intensity", &m_ssgiDiffuseIntensity, 0.0f, 4.0f);
                        ImGui::SliderFloat("SSGI diffuse max distance (m)",
                                           &m_ssgiDiffuseMaxDistance,
                                           0.5f,
                                           50.0f);
                        ImGui::SliderFloat(
                            "SSGI diffuse thickness (m)", &m_ssgiDiffuseThickness, 0.01f, 2.0f);
                        ImGui::SliderFloat("SSGI diffuse bilateral sharpness",
                                           &m_ssgiDiffuseBlurSharpness,
                                           0.0f,
                                           32.0f);
                        ImGui::SliderInt(
                            "SSGI diffuse traversal steps", &m_ssgiDiffuseMaxIntersections, 4, 128);
                        ImGui::SliderFloat("SSGI diffuse temporal blend",
                                           &m_ssgiDiffuseTemporalBlend,
                                           0.0f,
                                           0.98f);
                        ImGui::Checkbox("SSGI diffuse temporal filter", &m_ssgiDiffuseTaaEnabled);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "Motion-reprojected accumulation with depth + normal rejection; "
                                "falls back to the spatial-only path without motion vectors.");
                    }
                }

                // ---- Post Processing: exposure, tonemap, bloom, motion
                // blur, DOF, LUT.
                ImGui::SeparatorText("Post Processing");
                {
                    ImGui::Checkbox("Auto-exposure (X)", &m_autoExposureEnabled);
                    {
                        static const char* const kTonemapNames[4] = {
                            "Reinhard", "ACES", "Uncharted2", "GT7"};
                        int tonemapIdx = std::clamp(static_cast<int>(m_tonemapMode), 0, 3);
                        if (ImGui::Combo("Tonemap operator", &tonemapIdx, kTonemapNames, 4)) {
                            m_tonemapMode = static_cast<uint32_t>(std::clamp(tonemapIdx, 0, 3));
                            m_viewportResources.tonemap.desc.tonemapMode = m_tonemapMode;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("GT7: Polyphony Digital SDR (250-nit paper white, "
                                              "ICtCp chroma preserve).");
                    }
                    ImGui::SliderFloat("Manual exposure",
                                       &m_manualExposure,
                                       0.00005f,
                                       0.02f,
                                       "%.5f",
                                       ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat("EV bias", &m_exposureEvBias, -4.0f, 4.0f, "%.1f");
                    ImGui::SliderFloat(
                        "Adapt to dark (/s)", &m_exposureSpeedToDark, 0.1f, 8.0f, "%.2f");
                    ImGui::SliderFloat(
                        "Adapt to bright (/s)", &m_exposureSpeedToBright, 0.1f, 8.0f, "%.2f");
                    ImGui::SliderFloat("Min exposure",
                                       &m_exposureMin,
                                       0.000001f,
                                       0.001f,
                                       "%.6f",
                                       ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat("Max exposure",
                                       &m_exposureMax,
                                       0.005f,
                                       0.1f,
                                       "%.4f",
                                       ImGuiSliderFlags_Logarithmic);
                    if (m_exposureMin > m_exposureMax)
                        std::swap(m_exposureMin, m_exposureMax);
                    ImGui::Checkbox("Bloom (B)", &m_bloomEnabled);
                    if (m_bloomEnabled) {
                        ImGui::SliderFloat(
                            "Bloom intensity", &m_bloomIntensity, 0.0f, 2.0f, "%.2f");
                        ImGui::SliderFloat(
                            "Bloom threshold (HDR)", &m_bloomThreshold, 0.0f, 8000.0f, "%.0f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Pre-exposure HDR (sun-lit diffuse spans ~0-1600);\n"
                                              "knee tracks at 0.25x. Lower until highlights glow.");
                    }
                    ImGui::Checkbox("Motion blur (M)", &m_motionBlurEnabled);
                    if (m_motionBlurEnabled) {
                        ImGui::SliderInt("Blur samples", &m_motionBlurSamples, 1, 32);
                        ImGui::SliderFloat(
                            "Blur max radius (px)", &m_motionBlurMaxRadius, 0.0f, 64.0f);
                        ImGui::SliderFloat("Blur strength", &m_motionBlurStrength, 0.0f, 2.0f);
                        ImGui::Checkbox("Tile dilation (16px tiles, 5x5)",
                                        &m_motionBlurTileDilation);
                    }
                    ImGui::Checkbox("Depth of field", &m_dofEnabled);
                    if (m_dofEnabled) {
                        ImGui::Checkbox("HQ bokeh (48-tap hex gather)", &m_dofHqBokeh);
                        ImGui::SliderFloat("Focus distance (m)", &m_dofFocusDistance, 0.5f, 100.0f);
                        ImGui::SliderFloat("Focus range (m)", &m_dofFocusRange, 0.0f, 20.0f);
                        ImGui::SliderFloat("Low max radius (px)", &m_dofMaxRadius, 0.0f, 64.0f);
                        ImGui::SliderFloat(
                            "Bokeh max radius (px)", &m_dofBokehMaxRadius, 0.0f, 64.0f);
                        ImGui::SliderFloat("Highlight boost", &m_dofHighlightBoost, 1.0f, 4.0f);
                        ImGui::SliderFloat(
                            "Highlight threshold", &m_dofHighlightThreshold, 0.0f, 8.0f);
                    }
                    // Color grading / LUT. The renderer no longer scans a
                    // directory: the runtime asset service supplies decoded
                    // catalog LUT bytes through SetColorLutRgba, so this panel
                    // only reports the active LUT and clears it.
                    if (m_lutFile.empty()) {
                        ImGui::TextDisabled(
                            "Color LUT: off (catalog LUTs load through the runtime asset service)");
                    } else {
                        ImGui::Text("Color LUT: %s", m_lutFile.c_str());
                        ImGui::SameLine();
                        if (ImGui::Button("Clear"))
                            ClearColorLut();
                        ImGui::SliderFloat("LUT intensity", &m_lutIntensity, 0.0f, 1.0f, "%.2f");
                    }
                }

                // ---- Atmosphere: sky, sun, zones, fog.
                ImGui::SeparatorText("Atmosphere");
                {
                    ImGui::Checkbox("Sky dome (K)", &m_skyEnabled);
                    if (m_world) {
                        tour::scene::SceneEnv& se = m_world->scene.environment;
                        auto authored = [&]() { m_world->scene.envAuthored = true; };
                        ImGui::ColorEdit3("Sky color", &se.skyColor.x);
                        ImGui::DragFloat(
                            "Sky intensity (lux)", &se.skyIntensity, 5.0f, 0.0f, 10000.0f, "%.0f");
                        ImGui::Separator();
                        if (ImGui::SliderFloat("Sun azimuth", &se.sunAzimuth, 0.0f, 360.0f))
                            authored();
                        if (ImGui::SliderFloat("Sun elevation", &se.sunElevation, 0.0f, 90.0f))
                            authored();
                        if (ImGui::ColorEdit3("Sun color", &se.sunColor.x))
                            authored();
                        ImGui::DragFloat(
                            "Sun intensity (lux)", &se.sunIntensity, 10.0f, 0.0f, 2e5f, "%.0f");
                        ImGui::Separator();
                        // Fog: the combo is the single source of truth for the
                        // active path (tier 2 = the raymarched volumetrics).
                        const char* tiers[3] = {
                            "Off", "Analytic (exp2, fallback)", "Volumetric (raymarched GI-lit)"};
                        // The scene's resolved env is the all-infinite tile
                        // base zone. A mode edit updates that zone's authored
                        // values; UpdateEnvTiles rebakes on the next frame.
                        if (ImGui::Combo("Fog mode", &se.fogQuality, tiers, 3)) {
                            authored();
                            if (se.fogQuality == 2 && se.volumetrics <= 0.0f)
                                se.volumetrics = std::max(m_volumetricIntensity, 1.0f);
                            if (se.fogQuality == 2)
                                m_volumetricIntensity = se.volumetrics;
                            // A validated headless pin still takes precedence over
                            // the authored tier. Invalid spellings do not pin it.
                            if (!m_headlessPins.volumetric.has_value())
                                m_volumetricEnabled = se.fogQuality == 2;
                        }
                        if (se.fogQuality == 1) {
                            if (ImGui::ColorEdit3("Fog color", &se.fogColor.x))
                                authored();
                            if (ImGui::SliderFloat("Fog density", &se.fogDensity, 0.0f, 1.0f))
                                authored();
                        }
                    } else {
                        m_volumetricEnabled = false;
                    }
                    if (m_world && m_world->scene.environment.fogQuality == 2) {
                        // Re-declared for this block: the se/authored pair above
                        // lives inside the earlier `if (m_world)` scope, which
                        // MSVC leaks but GCC (correctly) does not - the Linux CI
                        // build failed with "'se' was not declared in this scope".
                        tour::scene::SceneEnv& se = m_world->scene.environment;
                        auto authored = [&]() { m_world->scene.envAuthored = true; };
                        static const char* const kVolNames[5] = {"Low (8 steps)",
                                                                 "Medium (16 steps)",
                                                                 "High (32 steps)",
                                                                 "Ultra (64 steps)",
                                                                 "Custom"};
                        int volQ = static_cast<int>(m_volumetricQuality);
                        if (ImGui::Combo("Quality preset", &volQ, kVolNames, 5)) {
                            m_volumetricQuality =
                                static_cast<smvk::VolumetricQuality>(std::clamp(volQ, 0, 4));
                            if (m_volumetricQuality != smvk::VolumetricQuality::Custom) {
                                smvk::VolumetricDesc qd{};
                                smvk::ApplyVolumetricQuality(qd, m_volumetricQuality);
                                m_volumetricSteps = (int)qd.steps;
                            }
                        }
                        if (ImGui::SliderInt("March steps", &m_volumetricSteps, 4, 128))
                            m_volumetricQuality = smvk::VolumetricQuality::Custom;
                        ImGui::SliderFloat(
                            "Fog max distance (m)", &m_volumetricMaxDistance, 5.0f, 200.0f);
                        // The base world's infinite +/- zone owns the
                        // extinction value. The tile bake replaces the pass
                        // density with this record; never edit only the pass.
                        if (ImGui::SliderFloat(
                                "Volumetric density", &se.volumetricDensity, 0.0f, 0.1f, "%.4f")) {
                            authored();
                            m_volumetricDensity = se.volumetricDensity;
                        }
                        if (ImGui::SliderFloat(
                                "Scattering intensity", &se.volumetrics, 0.0f, 4.0f)) {
                            authored();
                            m_volumetricIntensity = se.volumetrics;
                            if (!m_headlessPins.volumetric.has_value())
                                m_volumetricEnabled = se.volumetrics > 0.0f;
                        }
                        ImGui::SliderFloat("Anisotropy g", &m_volumetricAnisotropy, -0.9f, 0.9f);
                        ImGui::SliderFloat("Sun strength", &m_volumetricSun, 0.0f, 4.0f);
                        ImGui::SliderFloat("GI strength", &m_volumetricGi, 0.0f, 4.0f);
                        ImGui::Checkbox("Sun single-scatter", &m_volumetricUseSun);
                        ImGui::Checkbox("Convex volumes in fog GI", &m_volumetricUseGiVolumes);
                        ImGui::SliderFloat(
                            "Dither (banding guard)", &m_volumetricJitter, 0.0f, 1.0f);
                        ImGui::SliderFloat(
                            "Height falloff (1/m)", &m_volumetricHeightFalloff, 0.0f, 1.0f, "%.3f");
                        if (m_volumetricHeightFalloff > 0.0f)
                            ImGui::SliderFloat(
                                "Height base (m)", &m_volumetricHeightBase, -10.0f, 30.0f);
                        ImGui::SliderFloat(
                            "GI refresh (m)", &m_volumetricGiRefresh, 0.1f, 4.0f, "%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "The baked-GI base refresh interval along the march (was hardcoded "
                                "0.75). Ambient radiance is low-frequency - coarser is safe; the "
                                "GI-volume blend weights stay exact per segment.");
                        ImGui::SliderFloat("Field density cadence (m)",
                                           &m_volumetricFieldCadence,
                                           0.25f,
                                           4.0f,
                                           "%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "The world-field fog-density sampling interval: the field is "
                                "fetched at this spacing and lerped between segments (1 m default "
                                "- extinction-grade accuracy).");
                        ImGui::Separator();
                        // Sun occlusion for the fog - the THREE-WAY
                        // mode (never combined: multiplying the two
                        // visibility forms would darken the fog
                        // twice; the screen-space leg composites
                        // additively either way).
                        {
                            const char* fogShadowModes[3] = {
                                "Off", "CSM cascades", "Coarse tile (marked casters)"};
                            int fsm = std::clamp(m_fogSunShadowMode, 0, 2);
                            if (ImGui::Combo("Fog sun shadows", &fsm, fogShadowModes, 3))
                                m_fogSunShadowMode = fsm;
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "CSM: the march's per-segment cascade tap (PCF mode only - "
                                    "under EVSM the raw atlas is stale).\n"
                                    "Coarse: the dedicated sun-oriented tile with ONLY "
                                    "Casts-Volumetric-Shadow-marked large occluders - works under "
                                    "EVSM.\n"
                                    "Mark casters in the editor's node inspector.");
                        }
                        ImGui::Checkbox("Screen-space god rays (Dishonored)", &m_godraysEnabled);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "Sun occlusion mask + radial scattering over the scene depth - "
                                "additive over the CSM leg, and the fog sun-occlusion fallback "
                                "under EVSM. Debug views: TOURMALINE_GODRAYS_DEBUG=1..4.");
                        if (m_godraysEnabled) {
                            ImGui::SliderFloat(
                                "God rays intensity", &m_godraysIntensity, 0.0f, 2.0f);
                            ImGui::SliderInt("God rays samples", &m_godraysSamples, 4, 64);
                            ImGui::SliderFloat(
                                "God rays decay", &m_godraysDecay, 0.85f, 1.0f, "%.3f");
                            ImGui::SliderFloat(
                                "God rays glow radius", &m_godraysGlowOuter, 0.02f, 0.4f);
                            ImGui::SliderFloat("God rays march reach", &m_godraysReach, 0.2f, 1.0f);
                        }
                    }
                }

                // ---- Scene: particles + decals (the live scene tools).
                ImGui::SeparatorText("Scene Effects");
                {
                    ImGui::Checkbox("Enable particles (F)", &m_particlesEnabled);
                    ImGui::SameLine();
                    ImGui::TextDisabled(ParticlesActive() ? "(active)" : "(off)");
                    ImGui::SliderFloat("Gravity Y (m/s^2)", &m_particleDemo.gravityY, -20.0f, 5.0f);
                    const int emitterCount = (int)m_particleDemo.emitters.size();
                    if (emitterCount > 0) {
                        m_particleDemo.selected =
                            std::min(std::max(m_particleDemo.selected, 0), emitterCount - 1);
                        if (ImGui::BeginCombo("Edit emitter",
                                              std::to_string(m_particleDemo.selected).c_str())) {
                            for (int i = 0; i < emitterCount; ++i) {
                                const bool sel = (i == m_particleDemo.selected);
                                if (ImGui::Selectable(std::to_string(i).c_str(), sel))
                                    m_particleDemo.selected = i;
                                if (sel)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        auto& e = m_particleDemo.emitters[m_particleDemo.selected];
                        ImGui::SliderFloat3("Position", &e.position.x, -25.0f, 25.0f);
                        ImGui::SliderFloat("Spawn radius (m)", &e.position.w, 0.0f, 8.0f);
                        ImGui::SliderFloat3("Axis", &e.direction.x, -1.0f, 1.0f);
                        ImGui::SliderFloat("Spread (rad)", &e.direction.w, 0.0f, 3.14f);
                        ImGui::ColorEdit3("Color", &e.color.x);
                        ImGui::SliderFloat("Emissive intensity", &e.color.w, 0.0f, 20.0f);
                        ImGui::SliderFloat("Speed min", &e.velocity.x, 0.0f, 10.0f);
                        ImGui::SliderFloat("Speed max", &e.velocity.y, 0.0f, 10.0f);
                        ImGui::SliderFloat("Drag (1/s)", &e.velocity.z, 0.0f, 4.0f);
                        ImGui::SliderFloat("Gravity scale", &e.velocity.w, -2.0f, 2.0f);
                        ImGui::SliderFloat("Size start (m)", &e.size.x, 0.001f, 3.0f, "%.3f");
                        ImGui::SliderFloat("Size end (m)", &e.size.y, 0.001f, 5.0f, "%.3f");
                        ImGui::SliderFloat("Spin (rad/s)", &e.size.z, 0.0f, 8.0f);
                        ImGui::SliderFloat("Rate (/s)", &e.life.z, 0.0f, 500.0f);
                        ImGui::SliderFloat("Life min (s)", &e.life.x, 0.1f, 10.0f);
                        ImGui::SliderFloat("Life max (s)", &e.life.y, 0.1f, 10.0f);
                        ImGui::SliderFloat("Fade in", &e.fade.x, 0.0f, 0.5f);
                        ImGui::SliderFloat("Fade out", &e.fade.y, 0.0f, 0.9f);
                        ImGui::SliderFloat("Color jitter", &e.fade.z, 0.0f, 1.0f);
                        bool groundDust = e.tail.y > 0.5f;
                        if (ImGui::Checkbox("Alpha-blended dust cloud", &groundDust))
                            e.tail.y = groundDust ? smvk::kParticleRender_Dust : 0.0f;
                        ImGui::SliderFloat("Camera-box wrap (m)",
                                           &e.fade.w,
                                           0.0f,
                                           20.0f,
                                           "%.1f  (0 = world emitter)");
                        const char* shapes[] = {"Cone", "Sphere", "Point", "Ground XZ"};
                        int shape = (int)e.life.w;
                        if (ImGui::Combo("Shape", &shape, shapes, 4))
                            e.life.w = (float)shape;
                    }
                    if (ImGui::Button("Add emitter") &&
                        m_particleDemo.emitters.size() < PARTICLE_MAX_EMITTERS) {
                        m_particleDemo.emitters.push_back(smvk::ParticleEmitterGpu{});
                        m_particleDemo.selected = (int)m_particleDemo.emitters.size() - 1;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Remove") && !m_particleDemo.emitters.empty()) {
                        m_particleDemo.emitters.erase(m_particleDemo.emitters.begin() +
                                                      m_particleDemo.selected);
                        m_particleDemo.selected = std::max(0, m_particleDemo.selected - 1);
                    }
                    // Ambient presets (camera-box emitters): the toggle swaps
                    // the emitter's rate 0 <-> preset. TOURMALINE_DUST / RAIN
                    // do the same headless.
                    {
                        auto presetToggle = [this](const char* label, size_t idx, float rate) {
                            if (idx >= m_particleDemo.emitters.size())
                                return;
                            auto& e = m_particleDemo.emitters[idx];
                            bool on = e.life.z > 0.0f;
                            if (ImGui::Checkbox(label, &on))
                                e.life.z = on ? rate : 0.0f;
                        };
                        presetToggle("Camera dust", 2, 120.0f);
                        ImGui::SameLine();
                        presetToggle("World rain", 3, 900.0f);
                        presetToggle("Ground dust clouds", 4, 18.0f);
                    }
                    ImGui::Separator();
                    ImGui::Checkbox("Show decals", &m_decalsEnabled);
                    ImGui::SameLine();
                    ImGui::TextDisabled(DecalsActive() ? "(active)" : "(off)");
                    if (ImGui::Button("Splat at crosshair (J)"))
                        SpawnDecalAtCrosshair();
                }

                // ---- Geometry & streaming: LODs + texture streaming.
                ImGui::SeparatorText("Geometry & Streaming");
                {
                    ImGui::Checkbox("Enable LODs (L)", &m_lodEnabled);
                    ImGui::SliderFloat("Pixel budget (px)", &m_lodPixelBudget, 0.25f, 8.0f, "%.2f");
                    ImGui::Checkbox("Full-rate LOD updates", &m_lodFullRate);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("On: every node re-evaluates every frame.\n"
                                          "Off: halfrate refresh - decisions up to 1 frame stale.\n"
                                          "Selection is hysteretic either way.");
                    ImGui::Checkbox("Vertex-lit farthest LOD (shader LODs)", &m_lodShaderEnabled);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "The farthest LOD shades vertex-lit (sun + ambient,\n"
                            "no shadows/point lights/specular/textures). F5 shows it.");
                    ImGui::Checkbox("Texture streaming (applies to next scene load)",
                                    &m_textureStreamingEnabled);
                }
                ImGui::EndTabItem();
            }
            // ---------------------------------------------------------
            // Debug: views, overlays, technical/experimental tools and
            // one-shot diagnostics. Nothing here changes the frame
            // permanently; toggles are for bisection.
            // ---------------------------------------------------------
            if (ImGui::BeginTabItem("Debug")) {
                ImGui::SeparatorText("Debug views (F1..F6, F9..F11)");
                {
                    const char* viewNames[] = {
                        "F1 Lit",
                        "F2 Unlit",
                        "F3 Meshlets",
                        "F4 Cascades",
                        "F5 LODs",
                        "F6 Vertex Colors",
                        "F9 UV0 (base)",
                        "UV1 (lightmap atlas)",
                        "F10 Cubemap (IBL raw)",
                        "F11 POM (parallax occlusion)",
                        "SSR mask (receivers)",
                        "Normals (world-space)",
                        "SSGI diffuse: confidence",
                        "SSGI diffuse: raw radiance",
                        "SSGI diffuse: filtered layer",
                        "SSGI diffuse: ambient baseline",
                        "SSGI diffuse: contribution",
                        "Normals (mapped, world-space)",
                        "Motion vectors (V)",
                        "Sun contact shadows (contribution)",
                        "PBR / Material Health",
                        "PBR / Base Color",
                        "PBR / Diffuse Reflectance",
                        "PBR / Roughness",
                        "PBR / Metalness",
                        "PBR / F0 (specular)",
                        "PBR / SSS (naive preview)",
                        "PBR / Hair (naive preview)",
                        "PBR / Water (naive preview)",
                        "Clipping (shadows + highlights)",
                        "Clipping (shadows only)",
                        "Clipping (highlights only)",
                    };
                    // Combo index <-> ViewMode mapping (SsrContribution stays
                    // env-only; UV1 and Normals are combo/env only; the
                    // Ssgi diffuse views likewise — they render through
                    // the SSGI compose's debugView push, not a fragment
                    // swap, so they need the chain enabled; MotionVectors
                    // renders through the tonemap's debug branch).
                    static const ViewMode kComboViews[] = {
                        ViewMode::Lit,
                        ViewMode::Unlit,
                        ViewMode::Meshlets,
                        ViewMode::Cascades,
                        ViewMode::Lods,
                        ViewMode::VertexColors,
                        ViewMode::Uv0,
                        ViewMode::Uv1,
                        ViewMode::Cubemap,
                        ViewMode::Pom,
                        ViewMode::SsrMask,
                        ViewMode::Normals,
                        ViewMode::SsgiConfidence,
                        ViewMode::SsgiRaw,
                        ViewMode::SsgiLayer,
                        ViewMode::SsgiBaseline,
                        ViewMode::SsgiContribution,
                        ViewMode::NormalsMapped,
                        ViewMode::MotionVectors,
                        ViewMode::ContactShadows,
                        ViewMode::PbrHealth,
                        ViewMode::PbrBaseColor,
                        ViewMode::PbrDiffuse,
                        ViewMode::PbrRoughness,
                        ViewMode::PbrMetallic,
                        ViewMode::PbrF0,
                        ViewMode::SssPreview,
                        ViewMode::HairPreview,
                        ViewMode::WaterPreview,
                        // Clipping views: tonemap-pass annotations (the
                        // post-fx chain stays live under them).
                        ViewMode::ClippingCombined,
                        ViewMode::ClippingShadows,
                        ViewMode::ClippingHighlights,
                    };
                    // The combo count follows the arrays (the old hardcoded
                    // 20 once left appended modes existing but unselectable).
                    constexpr int viewCount = static_cast<int>(std::size(kComboViews));
                    int view = 0;
                    for (int i = 0; i < viewCount; ++i)
                        if (kComboViews[i] == m_debugView)
                            view = i;
                    // SetDebugView: the direct selection takes over
                    // (clears the R/F8 + Y overlay views).
                    if (ImGui::Combo("Debug view", &view, viewNames, viewCount))
                        SetDebugView(kComboViews[view]);
                    // Clipping-view thresholds (display-encoded SDR
                    // luma): the sliders only show while a clipping
                    // view is active.
                    if (IsClippingDebugView(m_debugView)) {
                        ImGui::SliderFloat("Shadow clipping threshold",
                                           &m_shadowClipThreshold,
                                           0.0f,
                                           0.1f,
                                           "%.3f");
                        ImGui::SliderFloat("Highlight clipping threshold",
                                           &m_highlightClipThreshold,
                                           0.9f,
                                           1.0f,
                                           "%.3f");
                    }
                    // The HDR SSGI views (raw/layer/baseline/
                    // contribution): one SHARED diagnostic exposure so
                    // switching views preserves relative brightness
                    // (confidence is excluded — its 0..1 read
                    // self-normalizes via 1/exposure).
                    if (IsSsgiHdrDebugView(m_debugView)) {
                        ImGui::SliderFloat(
                            "SSGI debug exposure (+EV)", &m_ssgiDebugEv, 0.0f, 12.0f, "%.1f");
                    }
                    // Motion vectors (V): the tonemap's debug view —
                    // direction -> hue, magnitude -> intensity, jitter
                    // stripped (TRUE motion). Gated on the motion pass
                    // (the old overlay's gate; without it the tonemap
                    // branch silently shows the scene).
                    if (m_passes.motionVectors && m_viewportResources.motionVectors.Valid()) {
                        bool motionView = (m_debugView == ViewMode::MotionVectors);
                        if (ImGui::Checkbox("Motion vectors (V)", &motionView))
                            SetDebugView(motionView ? ViewMode::MotionVectors : ViewMode::Lit);
                        if (motionView)
                            ImGui::SliderFloat(
                                "Motion debug scale", &m_motionDebugScale, 1.0f, 128.0f);
                    }
                    const char* meshletModes[] = {"Random per meshlet", "Cone-cutoff heat"};
                    ImGui::Combo("Meshlet color mode (F3)", &m_debugMeshletMode, meshletModes, 2);
                    if (ImGui::Checkbox("Cone axes (line overlay)", &m_debugCones)) {
                        // Arm/disarm the GPU leg (SetConeDebug lazily creates
                        // the module + the shared line buffer on first arm).
                        smvk::gfx::SetConeDebug(
                            m_gfxRenderer.State(), *m_context, *m_rendergraph, m_debugCones);
                    }
                    ImGui::Checkbox("Wireframe (P)", &m_wireframe);
                    ImGui::Checkbox("Hide transparent (bisect blending)", &m_hideTransparent);
                    if (!m_gfxRenderer.DebugProgramsValid())
                        ImGui::TextDisabled("Debug programs missing - views disabled");
                }

                ImGui::SeparatorText("Overlays");
                {
                    // (The motion-vector visualization moved OUT of the
                    // overlays: it is a debug VIEW now — see the Debug
                    // views section above / ViewMode::MotionVectors. A
                    // view owns the pixel; an overlay stacked over the
                    // scene read as neither one nor the other.)
                    ImGui::Checkbox("Game debug lines", &m_debugDrawEnabled);
                    ImGui::Checkbox("Light points overlay", &m_showLightPoints);
                    ImGui::Checkbox("Light numbers overlay", &m_showLightNumbers);
                    if (m_showLightPoints || m_showLightNumbers) {
                        ImGui::SliderFloat(
                            "Overlay point size (px)", &m_lightPointSize, 2.0f, 16.0f);
                        ImGui::SliderInt("Overlay max labels", &m_maxLightLabels, 0, 1024);
                    }
                    ImGui::Checkbox("Probe points", &m_giShowProbes);
                    if (m_giShowProbes) {
                        ImGui::SliderFloat("Probe overlay distance (m)",
                                           &m_giProbePointMaxDist,
                                           5.0f,
                                           300.0f,
                                           "%.0f m");
                        const char* probeColorModes[] = {
                            "Baked GI (irradiance)",
                            "Visibility (blocked/near/open)",
                            "Lattice (pre-bake)",
                        };
                        int pcm = m_giProbeColorMode;
                        if (ImGui::Combo("Probe color mode", &pcm, probeColorModes, 3))
                            m_giProbeColorMode = pcm;
                    }
                    ImGui::Checkbox("GI volume bounds (wireframe)", &m_giShowVolumeBounds);
                    if (m_regLoaded || m_giVolumesLoaded) {
                        // All five R / F8 stops (including off).
                        // Select Lit so the debug shader is visible.
                        const char* dbgNames[5] = {"Off",
                                                   "Baked irradiance",
                                                   "GI source",
                                                   "Probe distance",
                                                   "Visibility weight sum (wSum)"};
                        int dbgIdx = m_giDebug;
                        if (ImGui::Combo("GI debug view (R / F8)", &dbgIdx, dbgNames, 5)) {
                            m_giDebug = std::clamp(dbgIdx, 0, 4);
                            if (m_giDebug != 0)
                                m_debugView = ViewMode::Lit;
                        }
                        if (m_giVolumesLoaded)
                            ImGui::Checkbox("Convex GI volumes", &m_giVolumesEnabled);
                    }
                    ImGui::Checkbox("Capture locations overlay", &m_cubemapShowProbes);
                    if (m_cubemapShowProbes)
                        ImGui::SliderFloat(
                            "Overlay max dist (m)", &m_cubemapOverlayMaxDist, 5.0f, 300.0f, "%.0f");
                    ImGui::Checkbox("Probe preview (mirror spheres)", &m_cubemapShowBalls);
                    {
                        static const char* const kCubeDbgNames[3] = {
                            "Off",
                            "Cubemap only (no Fresnel)",
                            "Probe weight heat",
                        };
                        int dbgIdx = std::clamp(m_cubemapDebug, 0, 2);
                        if (ImGui::Combo("Cubemap debug", &dbgIdx, kCubeDbgNames, 3))
                            m_cubemapDebug = std::clamp(dbgIdx, 0, 2);
                    }
                }

                ImGui::SeparatorText("Culling");
                {
                    ImGui::Checkbox("Freeze culling view (Ctrl+F)", &m_freezeCullingView);
                    if (ImGui::Button("Dump indirect audit (stderr, stalls)"))
                        m_dumpIndirectAudit = true;
                    ImGui::SameLine();
                    if (ImGui::Button("Dump HZB (stderr, stalls)"))
                        m_debugDumpHzb = true;
                    ImGui::Separator();
                    // Cull-flag toggles write straight into the renderer
                    // config (takes effect next frame). FlipWinding
                    // default-off is pinned by gfxfilter_tests.
                    bool b =
                        (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Degenerate) != 0;
                    if (ImGui::Checkbox("Cull degenerate", &b))
                        m_gfxRenderer.config.cullFlags =
                            b ? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Degenerate
                              : m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Degenerate;
                    b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Backface) != 0;
                    if (ImGui::Checkbox("Cull backface", &b))
                        m_gfxRenderer.config.cullFlags =
                            b ? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Backface
                              : m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Backface;
                    b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Frustum) != 0;
                    if (ImGui::Checkbox("Cull frustum", &b))
                        m_gfxRenderer.config.cullFlags =
                            b ? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Frustum
                              : m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Frustum;
                    b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_SmallPrim) != 0;
                    if (ImGui::Checkbox("Cull small prims", &b))
                        m_gfxRenderer.config.cullFlags =
                            b ? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_SmallPrim
                              : m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_SmallPrim;
                    b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_FlipWinding) != 0;
                    if (ImGui::Checkbox("Flip winding (debug)", &b))
                        m_gfxRenderer.config.cullFlags =
                            b ? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_FlipWinding
                              : m_gfxRenderer.config.cullFlags &
                                    ~smvk::gfx::CullFlagBit_FlipWinding;
                    b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_HzbSkipMasked) !=
                        0;
                    if (ImGui::Checkbox("Exclude MASK from HZB test", &b))
                        m_gfxRenderer.config.cullFlags =
                            b ? m_gfxRenderer.config.cullFlags |
                                    smvk::gfx::CullFlagBit_HzbSkipMasked
                              : m_gfxRenderer.config.cullFlags &
                                    ~smvk::gfx::CullFlagBit_HzbSkipMasked;
                    // HZB occlusion CULL toggle: the occluder-depth fill +
                    // the pyramid are a FIXED COST; the flag gates only the
                    // filter's occlusion TEST.
                    {
                        bool occ = (m_gfxRenderer.config.cullFlags &
                                    smvk::gfx::CullFlagBit_HzbOcclusion) != 0;
                        if (ImGui::Checkbox("HZB occlusion cull", &occ))
                            m_gfxRenderer.config.cullFlags =
                                occ ? (m_gfxRenderer.config.cullFlags |
                                       smvk::gfx::CullFlagBit_HzbOcclusion)
                                    : (m_gfxRenderer.config.cullFlags &
                                       ~smvk::gfx::CullFlagBit_HzbOcclusion);
                    }
                    static bool s_aliasingOn = smvk::TargetHeapAliasingEnabled();
                    if (ImGui::Checkbox("Target-heap memory aliasing (next rebuild)",
                                        &s_aliasingOn))
                        smvk::SetTargetHeapAliasing(s_aliasingOn);
                    ImGui::SliderFloat("Occluder min px", &m_hizOccluderMinPixels, 0.0f, 128.0f);
                    ImGui::SliderFloat(
                        "Occluder min radius m", &m_hizOccluderMinRadius, 0.0f, 5.0f);
                    b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Contribution) != 0;
                    if (ImGui::Checkbox("Contribution cull (T4)", &b))
                        m_gfxRenderer.config.cullFlags =
                            b ? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Contribution
                              : m_gfxRenderer.config.cullFlags &
                                    ~smvk::gfx::CullFlagBit_Contribution;
                    {
                        float mc = m_gfxRenderer.MinContribution();
                        if (ImGui::SliderFloat("Min contribution px^2", &mc, 0.0f, 16.0f))
                            m_gfxRenderer.SetMinContribution(mc);
                    }
                }

                ImGui::SeparatorText("Lighting path");
                {
                    // Deferred direct lighting (tiled/zBin pipeline): switches
                    // the WHOLE direct-light path — on = shade from the
                    // deferred per-tile light lists, off = legacy inline
                    // forward+ path. A/B against each other (counters in
                    // Stats, ~2 frames old).
                    ImGui::Checkbox("Deferred shadows (H)", &m_dsEnabled);
                    ImGui::SameLine();
                    ImGui::TextDisabled(m_passes.deferredShadows
                                            ? (DsActive() ? "(deferred)" : "(inline forward+)")
                                            : "(pass disabled)");
                }

                ImGui::SeparatorText("Tools");
                {
                    // Material override (the SSR blur-bucket test rig):
                    // overwrite every uploaded material's metallic/roughness.
                    ImGui::Checkbox("Override material roughness/metallic",
                                    &m_debugMaterialOverride);
                    if (m_debugMaterialOverride) {
                        ImGui::SliderFloat("Roughness", &m_debugMaterialRoughness, 0.0f, 1.0f);
                        ImGui::SliderFloat("Metallic", &m_debugMaterialMetallic, 0.0f, 1.0f);
                        ImGui::TextDisabled("applies at material upload (UpdateMaterials)");
                    }
                }
#if defined(DS_DEBUG_VIZ)
                // ---- Deferred-shadow debug visualizers (paper 5.3/5.4).
                // Debug builds only (DS_DEBUG_VIZ): Y cycles the combo. ----
                ImGui::SeparatorText("DS visualizers (Y cycles)");
                {
                    const char* vizNames[] = {
                        "Off",
                        "Light count approx (ph2)",
                        "Light count accurate (ph3)",
                        "Light count final (ph4)",
                        "Count diff approx-accurate",
                        "Cull step (light)",
                        "Light list (light)",
                        "Light visibility (light)",
                    };
                    int viz = static_cast<int>(m_dsDebugMode);
                    if (ImGui::Combo("Visualizer", &viz, vizNames, 8)) {
                        m_dsDebugMode = static_cast<DsDebugViz>(viz);
                        // Same snap as the GI combo: the visualizers
                        // render through the lit shader.
                        if (m_dsDebugMode != DsDebugViz::Off)
                            m_debugView = ViewMode::Lit;
                    }
                    const bool vizOn = m_dsDebugMode != DsDebugViz::Off;
                    const bool lightScoped = vizOn && m_dsDebugMode >= DsDebugViz::CullStep;
                    if (vizOn && !DsActive())
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                           "Pipeline off: enable deferred shadows (H) first");
                    if (vizOn)
                        ImGui::Checkbox("Overlay over lit", &m_dsDebugOverlay);
                    if (m_dsDebugMode >= DsDebugViz::CountApprox &&
                        m_dsDebugMode <= DsDebugViz::CountDiff)
                        ImGui::SliderInt("Heat ceiling (lights)",
                                         &m_dsDebugCountCeil,
                                         1,
                                         (int)smvk::kDsMaxLightsPerTile);
                    if (lightScoped) {
                        const int maxLight = std::max(0, (int)m_dsLightCount - 1);
                        ImGui::SliderInt("Debug light (sorted idx)", &m_dsDebugLight, 0, maxLight);
                        if (m_dsDebugLightValid) {
                            ImGui::Text("Light %d: %s  range %.1f m",
                                        m_dsDebugLight,
                                        m_dsDebugLightType == (int)smvk::kForwardPlusSpot ? "spot"
                                                                                          : "point",
                                        m_dsDebugLightRange);
                        }
                    }
                }
#endif
                ImGui::EndTabItem();
            }
            // ---------------------------------------------------------
            // Stats: read-only readback + the technical diagnostics
            // (everything that reports state rather than setting it).
            // Retired-slice counters lag ~2 frames.
            // ---------------------------------------------------------
            if (ImGui::BeginTabItem("Stats")) {
                ImGui::SeparatorText("GPU + frame");
                {
                    ImGui::Text("GPU: %s", smvk::ActiveDeviceName());
                    // Optick scopes for rendergraph passes are dynamically named from
                    // RenderPass::name_. This overview uses those EXACT same names,
                    // with a separate retired-frame Vulkan timestamp pair per pass.
                    // Labels are append-only to avoid distracting row movement when
                    // the graph enables/disables passes between frames.
                    {
                        const auto currentPasses = m_swapchain ? m_swapchain->GetGpuPassTimings()
                                                               : std::vector<smvk::GpuPassTiming>{};
                        static std::vector<std::string> knownPassNames;
                        for (const auto& pass : currentPasses) {
                            if (std::find(knownPassNames.begin(),
                                          knownPassNames.end(),
                                          pass.name) == knownPassNames.end())
                                knownPassNames.push_back(pass.name);
                        }
                        ImGui::TextDisabled(
                            "Rendergraph GPU passes (Optick labels, retired frame)");
                        if (knownPassNames.empty()) {
                            ImGui::TextDisabled(m_stageGpuTimingsSupported
                                                    ? "Waiting for GPU pass timestamps..."
                                                    : "GPU pass timestamps unavailable "
                                                      "(cross-queue support required).");
                        } else {
                            struct PassRow {
                                double ms = 0.0;
                                bool valid = false;
                            };
                            std::vector<PassRow> rows(knownPassNames.size());
                            for (const auto& pass : currentPasses) {
                                if (!pass.valid)
                                    continue;
                                auto it = std::find(
                                    knownPassNames.begin(), knownPassNames.end(), pass.name);
                                if (it == knownPassNames.end())
                                    continue;
                                auto& row = rows[static_cast<size_t>(it - knownPassNames.begin())];
                                row.ms += pass.milliseconds; // repeated Optick names are grouped
                                row.valid = true;
                            }
                            double totalMs = 0.0;
                            for (const auto& row : rows)
                                if (row.valid)
                                    totalMs += std::max(0.0, row.ms);
                            // Compact one-line rows: name on the left, timing right-aligned.
                            // Keep both columns independent of the duration stack.
                            const float lineHeight = ImGui::GetTextLineHeight();
                            const float rowHeight = lineHeight + 4.0f;
                            const float plotHeight = rowHeight * static_cast<float>(rows.size());
                            const float labelWidth = 230.0f;
                            const float valueWidth = ImGui::CalcTextSize("8888.88 ms").x + 6.0f;
                            const float nameWidth = labelWidth - valueWidth - 4.0f;
                            const float plotWidth = 75.0f;
                            static constexpr ImU32 colors[] = {IM_COL32(105, 140, 190, 230),
                                                               IM_COL32(74, 157, 148, 230),
                                                               IM_COL32(146, 124, 190, 230),
                                                               IM_COL32(193, 146, 95, 230),
                                                               IM_COL32(168, 124, 150, 230),
                                                               IM_COL32(118, 156, 108, 230),
                                                               IM_COL32(100, 152, 170, 230),
                                                               IM_COL32(177, 150, 105, 230)};
                            ImGui::BeginChild("##gpu_pass_timeline_scroll",
                                              ImVec2(0.0f, std::min(420.0f, plotHeight + 8.0f)),
                                              false);
                            const ImVec2 origin = ImGui::GetCursorScreenPos();
                            ImDrawList* draw = ImGui::GetWindowDrawList();
                            const ImVec2 plotTop(origin.x + labelWidth + 12.0f, origin.y);
                            draw->AddRectFilled(
                                plotTop,
                                ImVec2(plotTop.x + plotWidth, plotTop.y + plotHeight),
                                IM_COL32(65, 67, 73, 65),
                                3.0f);
                            float cursorY = plotTop.y;
                            for (size_t i = 0; i < rows.size(); ++i) {
                                // Stable index, deterministic color. Neither changes on frame
                                // updates.
                                uint32_t hash = 2166136261u;
                                for (unsigned char c : knownPassNames[i])
                                    hash = (hash ^ c) * 16777619u;
                                const ImU32 color =
                                    colors[hash % (sizeof(colors) / sizeof(colors[0]))];
                                const ImVec2 labelTop(origin.x,
                                                      origin.y + rowHeight * static_cast<float>(i));
                                draw->AddCircleFilled(
                                    ImVec2(labelTop.x + 4.0f, labelTop.y + 8.0f), 3.0f, color);
                                // Clip pass names at the value column, while keeping
                                // full names available on hover in the future.
                                draw->PushClipRect(
                                    ImVec2(labelTop.x + 12.0f, labelTop.y),
                                    ImVec2(labelTop.x + nameWidth, labelTop.y + rowHeight),
                                    true);
                                draw->AddText(ImVec2(labelTop.x + 12.0f, labelTop.y + 2.0f),
                                              ImGui::GetColorU32(ImGuiCol_Text),
                                              knownPassNames[i].c_str());
                                draw->PopClipRect();
                                char value[40];
                                if (rows[i].valid)
                                    snprintf(value, sizeof(value), "%.2f ms", rows[i].ms);
                                else
                                    snprintf(value, sizeof(value), "off");
                                const float valueX = labelTop.x + labelWidth - 4.0f -
                                                     ImGui::CalcTextSize(value).x;
                                draw->AddText(ImVec2(valueX, labelTop.y + 2.0f),
                                              ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                              value);
                                if (rows[i].valid && totalMs > 0.0) {
                                    const float height = static_cast<float>(
                                        std::max(0.0, rows[i].ms) / totalMs * plotHeight);
                                    if (height > 0.0f) {
                                        draw->AddRectFilled(
                                            ImVec2(plotTop.x, cursorY),
                                            ImVec2(plotTop.x + plotWidth, cursorY + height),
                                            color);
                                        cursorY += height;
                                    }
                                }
                            }
                            ImGui::Dummy(ImVec2(labelWidth + plotWidth + 12.0f, plotHeight));
                            ImGui::EndChild();
                            ImGui::TextDisabled(
                                "Vertical stack = pass durations, not queue chronology.");
                            ImGui::TextDisabled(
                                "Async passes may overlap; repeated names are grouped.");
                        }
                    }
                    ImGui::Separator();
                    {
                        const glm::vec3 euler = m_cameraPositioner.getEulerDegrees();
                        const glm::quat q = m_cameraPositioner.getOrientation();
                        ImGui::Text("Camera yaw %.1f pitch %.1f roll %.1f (deg)",
                                    euler.y,
                                    euler.x,
                                    euler.z);
                        ImGui::TextDisabled("quat: %.3f %.3f %.3f %.3f", q.x, q.y, q.z, q.w);
                    }
                    ImGui::Text("Swapchain %ux%u  framesInFlight=%u",
                                fb.swapchain->GetExtent().width,
                                fb.swapchain->GetExtent().height,
                                fb.swapchain->framesInFlight());
                    ImGui::Text("Scene: %ux%u (%.0f%%) -> swap %ux%u",
                                renderExtent.width,
                                renderExtent.height,
                                m_renderScale * 100.0f,
                                swapExtent.width,
                                swapExtent.height);
                    ImGui::Text("UI pass: %ux%u (%.0f%%)",
                                uiExtent.width,
                                uiExtent.height,
                                m_uiScale * 100.0f);
                    const smvk::PresentMode active = fb.swapchain->GetActivePresentMode();
                    ImGui::TextDisabled(
                        "Present: %s%s",
                        smvk::ToString(active),
                        active != m_host.GetPresentMode() ? " (requested unavailable?)" : "");
                    if (m_inputUi && m_inputUi->hasGamepad()) {
                        ImGui::Text("Gamepad: %s", m_inputUi->gamepadName(0).c_str());
                        ImGui::TextDisabled("LX %+.2f LY %+.2f  RX %+.2f RY %+.2f",
                                            m_inputUi->gamepadAxis(0, input::GamepadAxis::LeftX),
                                            m_inputUi->gamepadAxis(0, input::GamepadAxis::LeftY),
                                            m_inputUi->gamepadAxis(0, input::GamepadAxis::RightX),
                                            m_inputUi->gamepadAxis(0, input::GamepadAxis::RightY));
                        float dzMove = m_inputUi->stickDeadzone();
                        float dzLook = m_inputUi->lookDeadzone();
                        float dzTrig = m_inputUi->triggerDeadzone();
                        if (ImGui::SliderFloat("Move deadzone", &dzMove, 0.0f, 0.5f))
                            m_inputUi->setStickDeadzone(dzMove);
                        if (ImGui::SliderFloat("Look deadzone", &dzLook, 0.0f, 0.5f))
                            m_inputUi->setLookDeadzone(dzLook);
                        if (ImGui::SliderFloat("Trigger deadzone", &dzTrig, 0.0f, 0.5f))
                            m_inputUi->setTriggerDeadzone(dzTrig);
                    } else {
                        ImGui::TextDisabled("Gamepad: none connected");
                    }
                }

                ImGui::SeparatorText("Memory + streaming");
                {
                    // GPU memory (VMA budgets, VK_EXT_memory_budget),
                    // throttled to 1 Hz (see RefreshGpuMemoryStats).
                    RefreshGpuMemoryStats(false);
                    auto fmtMiB = [](uint64_t bytes, char* out, size_t n) {
                        const double mib = (double)bytes / (1024.0 * 1024.0);
                        if (mib >= 1024.0)
                            snprintf(out, n, "%.2f GiB", mib / 1024.0);
                        else
                            snprintf(out, n, "%.0f MiB", mib);
                    };
                    if (!m_gpuMemHeaps.empty()) {
                        if (ImGui::SmallButton(m_gpuMemShowAllHeaps ? "heaps: all [-]"
                                                                    : "heaps: details [+]"))
                            m_gpuMemShowAllHeaps = !m_gpuMemShowAllHeaps;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("log to console"))
                            LogGpuMemoryToConsole();
                        ImGui::SameLine();
                        ImGui::Checkbox("auto-log 10s", &m_gpuMemConsoleAutoLog);
                        ImGui::TextDisabled("query %.0f us @1Hz", m_gpuMemQueryUs);
                        const size_t showCount = m_gpuMemShowAllHeaps ? m_gpuMemHeaps.size() : 1;
                        for (size_t i = 0; i < m_gpuMemHeaps.size() && i < showCount; ++i) {
                            size_t idx = i;
                            if (!m_gpuMemShowAllHeaps) {
                                idx = 0;
                                for (size_t k = 0; k < m_gpuMemHeaps.size(); ++k) {
                                    if (m_gpuMemHeaps[k].deviceLocal) {
                                        idx = k;
                                        break;
                                    }
                                }
                            }
                            const auto& h = m_gpuMemHeaps[idx];
                            const float hf =
                                h.budgetBytes > 0
                                    ? (float)((double)h.usageBytes / (double)h.budgetBytes)
                                    : 0.0f;
                            char hu[32], hb[32], hs[32], hov[80];
                            fmtMiB(h.usageBytes, hu, sizeof(hu));
                            fmtMiB(h.budgetBytes, hb, sizeof(hb));
                            fmtMiB(h.heapSizeBytes, hs, sizeof(hs));
                            snprintf(hov,
                                     sizeof(hov),
                                     "heap%zu %s %s/%s %.1f%% (heap %s)",
                                     idx,
                                     h.deviceLocal ? "VRAM" : "host",
                                     hu,
                                     hb,
                                     hf * 100.0f,
                                     hs);
                            ImGui::ProgressBar(hf, ImVec2(-1.0f, 0.0f), hov);
                            if (!m_gpuMemShowAllHeaps)
                                break;
                        }
                    } else {
                        ImGui::TextDisabled("GPU memory: no heap data");
                    }
                    if (!m_textureStreamer)
                        ImGui::TextDisabled("Streaming: off (full-resident)");
                }

                ImGui::SeparatorText("Shadows");
                {
                    const auto& atlas = m_viewportResources.directionalShadows;
                    ImGui::Text("CSM: %u cascades  tile %ux%u  atlas %ux%u  lambda %.2f  far %.1fm",
                                NUM_CASCADES,
                                CascadeTileExtent().width,
                                CascadeTileExtent().height,
                                atlas.gridExtent.width,
                                atlas.gridExtent.height,
                                (double)m_cascadeLambda,
                                (double)CascadeFar());
                    for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
                        ImGui::Text(" cascade %u: [%.2f .. %.2f] depthBias %.5f normalBias %.4f",
                                    i,
                                    (double)m_cascadeSplitsDbg[i],
                                    (double)m_cascadeSplitsDbg[i + 1],
                                    (double)m_cascadeBiasDbg[i],
                                    (double)m_cascadeNormalBiasDbg[i]);
                    }
                    if (m_sdsmEnabled && m_sdsmStatsValid) {
                        ImGui::TextDisabled(
                            "SDSM measured %.2f..%.2f m (pct %.2f..%.2f) -> applied %.2f..%.2f m%s",
                            m_sdsmMin,
                            m_sdsmMax,
                            m_sdsmPctMin,
                            m_sdsmPctMax,
                            m_sdsmAppliedValid ? m_sdsmAppliedMin : 0.0f,
                            m_sdsmAppliedValid ? m_sdsmAppliedMax : 0.0f,
                            m_sdsmCutCooldown > 0 ? " (cut cooldown: heuristic)" : "");
                    }
                    const glm::vec3 toSunDbg = frameEnv.SunDirectionToSun();
                    ImGui::Text("sun toSun: %.2f %.2f %.2f  int %.0f lux",
                                toSunDbg.x,
                                toSunDbg.y,
                                toSunDbg.z,
                                frameEnv.sunIntensity);
                    ImGui::Text(
                        "RenderViews: %u allocated (main=%u, CSM @%u, spots @%u, point faces @%u)",
                        m_renderViews.Count(),
                        m_renderViews.MainViewIndex(),
                        cascadeViewBase,
                        spotViewBase,
                        pointViewBase);
                    ImGui::Text("Spot shadows: %u / %u (budget %d)  atlas %ux%u",
                                m_spotShadowCount,
                                smvk::kMaxSpotShadows,
                                m_spotShadowMax,
                                m_viewportResources.spotShadows.atlasExtent.width,
                                m_viewportResources.spotShadows.atlasExtent.height);
                    ImGui::Text("Point shadows: %u lights / %u (%u face tiles, 6 each)",
                                m_pointShadowLightCount,
                                smvk::kMaxPointShadowLights,
                                m_pointShadowTileCount);
                    if (!m_viewportResources.spotShadows.Valid())
                        ImGui::TextDisabled("Spot atlas: MISSING (spots run unshadowed)");
                    ImGui::Separator();
                    // Deferred shadows (tiled/zBin): counters are
                    // retired-slice peaks (~2 frames old).
                    const smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
                    if (ds.Valid()) {
                        ImGui::Text("Shadow tiles: %ux%u (8px)  TileGroups: %ux%u (64px)",
                                    ds.tileCountX,
                                    ds.tileCountY,
                                    ds.groupCountX,
                                    ds.groupCountY);
                        ImGui::Text("Light list: %u / %u uints (%.1f MB used)",
                                    m_dsStats.listSlots,
                                    smvk::kDsMaxListUInts,
                                    float(m_dsStats.listSlots) * 4.0f / (1024.0 * 1024.0));
                        ImGui::Text("Visibility: %u / %u slots x 64B (%.1f MB used)",
                                    m_dsStats.visSlots,
                                    smvk::kDsMaxVisSlots,
                                    float(m_dsStats.visSlots) * 64.0f / (1024.0 * 1024.0));
                        if (m_dsStats.listSlots >= smvk::kDsMaxListUInts)
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                "Light list SATURATED: tiles clamp (lights drop to fully lit)");
                        if (m_dsStats.visSlots >= smvk::kDsMaxVisSlots)
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                "Visibility SATURATED: partial shadows degrade to fully lit");
                    } else {
                        ImGui::TextDisabled(
                            "Deferred buffers/shaders missing (extent cap?); pipeline off");
                    }
                }

                ImGui::SeparatorText("Post + AA state");
                {
                    const auto& bloom = m_viewportResources.bloom;
                    if (bloom.Valid() && m_bloomEnabled) {
                        ImGui::Text("Bloom: %u mips  base %ux%u  thr %.2f knee %.2f int %.2f",
                                    bloom.MipCount(),
                                    bloom.MipExtent(0).width,
                                    bloom.MipExtent(0).height,
                                    bloom.desc.threshold,
                                    bloom.desc.knee,
                                    bloom.desc.intensity);
                    } else {
                        ImGui::TextDisabled("Bloom: OFF");
                    }
                    float histPeak = 1.0f;
                    for (float v : m_exposureBinsUi)
                        histPeak = std::max(histPeak, v);
                    ImGui::PlotHistogram("##expHist",
                                         m_exposureBinsUi,
                                         (int)smvk::kExposureBinCount,
                                         0,
                                         nullptr,
                                         0.0f,
                                         histPeak,
                                         ImVec2(0, 48));
                    if (m_aaMode == AaMode::Msaa2x || m_aaMode == AaMode::Msaa2xCustom) {
                        if (!m_context->SupportsMSAA2x())
                            ImGui::TextDisabled("MSAA 2x unsupported - running 1x");
                        else
                            ImGui::TextDisabled(m_viewportResources.msaa.Valid()
                                                    ? (m_aaMode == AaMode::Msaa2xCustom
                                                           ? "MSAA custom requested (hardware "
                                                             "fallback if unavailable)"
                                                           : "MSAA 2x -> HDR resolve")
                                                    : "MSAA target missing - running 1x");
                    } else if (m_aaMode == AaMode::Smaa1x || m_aaMode == AaMode::SmaaT2x) {
                        if (!m_viewportResources.smaa.Valid())
                            ImGui::TextDisabled(
                                "SMAA unavailable (shaders/LUTs missing - rebuild?)");
                        else if (!(m_passes.motionVectors &&
                                   m_viewportResources.motionVectors.Valid()))
                            ImGui::TextDisabled("SMAA needs the motion-vector pass - disabled");
                        else if (m_aaMode == AaMode::SmaaT2x)
                            ImGui::TextDisabled(
                                "SMAA T2x: jitter + temporal resolve%s [img=%u par=%u]",
                                m_smaaFrameParams.historyValid ? "" : " (history warming up)",
                                fb.imageIndex,
                                m_smaaFrameParams.parity);
                        else
                            ImGui::TextDisabled("SMAA 1x: edges -> weights -> neighborhood blend");
                    } else {
                        ImGui::TextDisabled("AA: off");
                    }
                    {
                        const char* dlssModeName[] = {"off",
                                                      "performance",
                                                      "balanced",
                                                      "quality",
                                                      "ultra performance",
                                                      "ultra quality",
                                                      "DLAA"};
                        const int modeIdx = static_cast<int>(m_dlssMode);
                        if (m_dlssFrameActive)
                            ImGui::TextDisabled("DLSS %s: HDR %ux%u -> %ux%u%s",
                                                dlssModeName[std::clamp(modeIdx, 0, 6)],
                                                renderExtent.width,
                                                renderExtent.height,
                                                swapExtent.width,
                                                swapExtent.height,
                                                m_dlssReset ? " (history reset)" : "");
                        else if (DlssRequested())
                            ImGui::TextDisabled(
                                "DLSS %s: INACTIVE (%s%s)",
                                dlssModeName[std::clamp(modeIdx, 0, 6)],
                                smvk::DlssAvailable() ? "unsupported adapter" : "runtime missing",
                                m_passes.motionVectors ? "" : " / no motion vectors");
                        else
                            ImGui::TextDisabled("DLSS: off (Streamline %s)",
                                                smvk::DlssAvailable() ? "loaded" : "not loaded");
                    }
                    if (!m_viewportResources.motionBlur.Valid())
                        ImGui::TextDisabled("Motion blur target/shaders missing - sharp HDR");
                    if (!m_viewportResources.dof.Valid())
                        ImGui::TextDisabled("DOF targets/shaders missing - sharp HDR");
                    else
                        ImGui::TextDisabled("DOF: %s focus %.1fm range %.1fm (%s)",
                                            m_dofEnabled ? "ON" : "off",
                                            m_dofFocusDistance,
                                            m_dofFocusRange,
                                            m_dofHqBokeh ? "HQ bokeh" : "low");
                    if (!m_viewportResources.ssr.Valid())
                        ImGui::TextDisabled("Reflection target/shaders missing - no reflections");
                    else if (!m_viewportResources.ssgi.hzb.Valid())
                        ImGui::TextDisabled(
                            "SSR: %s %u steps intensity %.2f hzb:MISSING (chain skips)",
                            m_ssrEnabled ? "ON" : "off",
                            (uint32_t)std::clamp(m_ssrSteps, 1, 128),
                            m_ssrIntensity);
                    else
                        ImGui::TextDisabled(
                            "SSR: %s %u steps intensity %.2f hzb:shared %ux%u %u mips blur:%s",
                            m_ssrEnabled ? "ON" : "off",
                            (uint32_t)std::clamp(m_ssrSteps, 1, 128),
                            m_ssrIntensity,
                            m_viewportResources.ssgi.hzb.width,
                            m_viewportResources.ssgi.hzb.height,
                            m_viewportResources.ssgi.hzb.mipCount,
                            m_ssrBlurMode == 1 ? "separable H+V" : "2D");
                    if (!m_viewportResources.volumetric.Valid())
                        ImGui::TextDisabled("Volumetric target/shaders missing - no fog");
                    else
                        // No "dens" readout: the density is AUTHORED
                        // content (the env table's base zone record
                        // replaces the pass value; the panel slider is
                        // gone) — the live panel knobs are steps +
                        // intensity.
                        ImGui::TextDisabled("Volumetric: %s %s %u steps int %.2f (dens authored)",
                                            m_volumetricEnabled ? "ON" : "off",
                                            smvk::VolumetricQualityName(m_volumetricQuality),
                                            (uint32_t)std::clamp(m_volumetricSteps, 4, 128),
                                            m_volumetricIntensity);
                }

                ImGui::SeparatorText("GI + environment");
                {
                    ImGui::Text("Ambient source: %s (N cycles)", AmbientModeName(m_ambientMode));
                    ImGui::Text("Convex GI volumes: %u  regular grids: %u",
                                (unsigned)m_giVolumes.size(),
                                (unsigned)m_regVolumes.size());
                    const auto& ssgi = m_viewportResources.ssgi;
                    if (m_aoMode != AoMode::None) {
                        if (!ssgi.Valid())
                            ImGui::TextDisabled("SSGI targets/shaders missing - no AO");
                        else if (!ssgi.hzb.Valid())
                            ImGui::TextDisabled(
                                "SSGI AO: targets up but the shared HZB is missing - chain skips");
                        else {
                            ImGui::Text("SSGI AO: half %ux%u (full %ux%u) hzb %ux%u (%u mips)",
                                        ssgi.halfExtent.width,
                                        ssgi.halfExtent.height,
                                        ssgi.fullExtent.width,
                                        ssgi.fullExtent.height,
                                        ssgi.hzb.width,
                                        ssgi.hzb.height,
                                        ssgi.hzb.mipCount);
                        }
                    }
                    // World env tiles (zones): always on; the readout.
                    ImGui::Text(
                        "Zones: baked %u x %u tiles @64 m, %u record(s), grid @ (%.0f, %.0f)%s",
                        (unsigned)m_envTiles.DimX(),
                        (unsigned)m_envTiles.DimZ(),
                        (unsigned)m_envTiles.RecordCount(),
                        m_envTiles.GridMin().x,
                        m_envTiles.GridMin().y,
                        m_envTilesPublished ? "" : " (none this frame - plain env path)");
                    // Cubemap / probe residency.
                    size_t bakedCount = 0, procCount = 0;
                    for (const auto& [name, entry] : m_cubemaps.entries) {
                        if (entry.bindless >= 0 && !entry.texture.empty())
                            (entry.baked ? bakedCount : procCount)++;
                    }
                    size_t streamResident = 0, streamOut = 0, streamFailed = 0;
                    for (const CubemapRow& row : m_cubemapRows) {
                        if (row.failed)
                            ++streamFailed;
                        else if (m_cubemaps.Find(row.entryName))
                            ++streamResident;
                        else
                            ++streamOut;
                    }
                    ImGui::Text("Cubemaps: %u resident / %u rows (baked %u, procedural %u)  "
                                "Probes: %u (light tail)",
                                (unsigned)m_cubemaps.entries.size(),
                                (unsigned)m_cubemapRows.size(),
                                (unsigned)bakedCount,
                                (unsigned)procCount,
                                (unsigned)ProbeEntryCount());
                    if (streamFailed > 0)
                        ImGui::TextDisabled("stream failed rows: %u (see [Cubemap] log)",
                                            (unsigned)streamFailed);
                    {
                        const bool tiled = ProbeTileListsWanted();
                        ImGui::Text("Tiled probes: %s (stride %u shared, tiles %ux%u)",
                                    tiled ? "fresh" : "stale (global cubemap)",
                                    m_viewportResources.forwardPlus.desc.maxLightsPerTile,
                                    m_viewportResources.forwardPlus.tileCountX,
                                    m_viewportResources.forwardPlus.tileCountY);
                    }
                    // The pre-mix selection readout (the plan's Phase 3
                    // observability item; recomputed on the CPU via the SAME
                    // math the weights pass runs).
                    ImGui::Text("Pre-mix: %s (NDF weights, K=4)",
                                m_probePremixEnabled ? "on" : "off (per-pixel walk)");
                    if (m_probePremixEnabled) {
                        constexpr uint32_t kTailCap = smvk::kForwardPlusMaxProbes;
                        const uint32_t n = std::min(ProbeEntryCount(), kTailCap);
                        smvk::probepremix::NdfCandidate cands[kTailCap];
                        const glm::vec3 poi = m_cameraPositioner.cameraPosition_;
                        uint32_t m = 0;
                        for (uint32_t i = 0; i < n; ++i) {
                            const CubemapProbeGpu& pr = m_cubemapProbes[i];
                            const float influence = std::max(pr.posInfluence.w, 0.0f);
                            const float fade = std::max(pr.fadeCubemap.x, 0.0f);
                            const float dist = glm::distance(poi, glm::vec3(pr.posInfluence));
                            float ndf = 1.0f;
                            if (dist <= influence)
                                ndf = 0.0f;
                            else if (fade > 0.0f && dist <= influence + fade)
                                ndf = (dist - influence) / fade;
                            if (ndf >= 1.0f)
                                continue;
                            cands[m].cubemap = static_cast<uint32_t>(pr.fadeCubemap.y);
                            cands[m].ndf = ndf;
                            cands[m].influence = influence;
                            ++m;
                        }
                        float w[kTailCap];
                        smvk::probepremix::ComputeNdfWeights(cands, m, w, kTailCap);
                        struct SlotReadout {
                            float w;
                            uint32_t cube, i;
                        } slots[kTailCap];
                        for (uint32_t i = 0; i < m; ++i)
                            slots[i] = {w[i], cands[i].cubemap, i};
                        std::sort(
                            slots, slots + m, [&](const SlotReadout& a, const SlotReadout& b) {
                                if (a.w != b.w)
                                    return a.w > b.w;
                                if (cands[a.i].influence != cands[b.i].influence)
                                    return cands[a.i].influence > cands[b.i].influence;
                                return a.cube < b.cube;
                            });
                        const uint32_t k = std::min(m, 4u);
                        float sumK = 0.0f;
                        for (uint32_t s = 0; s < k; ++s)
                            sumK += slots[s].w;
                        const float invK = sumK > 1e-6f ? 1.0f / sumK : 0.0f;
                        for (uint32_t s = 0; s < k && invK > 0.0f; ++s) {
                            const bool inner = cands[slots[s].i].ndf <= 0.0f;
                            ImGui::BulletText("slot %u: cubemap %u  w=%.3f  d=%.2f%s",
                                              (unsigned)s,
                                              (unsigned)slots[s].cube,
                                              slots[s].w * invK,
                                              cands[slots[s].i].ndf,
                                              inner ? "  (INNER - exclusive)" : "");
                        }
                    }
                }

                ImGui::SeparatorText("Culling + LODs");
                {
                    const auto& fwd = m_viewportResources.forwardPlus;
                    ImGui::Text("Forward+: %ux%u tiles (%u)  tile %u  max/tile %u",
                                fwd.tileCountX,
                                fwd.tileCountY,
                                fwd.tileCount,
                                FP_TILE_SIZE,
                                fwd.desc.maxLightsPerTile);
                    ImGui::Text("Lights: %u / %u%s",
                                m_fpLightCount,
                                FP_MAX_LIGHTS,
                                m_fpLightCount > FP_MAX_LIGHTS ? "  (CLAMPED!)" : "");
                    ImGui::Text("Decals: %u / %u (cap %u/tile)%s",
                                m_decalFrameCount,
                                FP_MAX_DECALS,
                                FP_MAX_DECALS_PER_TILE,
                                DecalsActive() ? "" : "  (off)");
                    const smvk::gfx::Stats gs = m_gfxRenderer.LastStats();
                    // The cull runs in compute; the kept count lands via the
                    // async counter-word readback (see UpdateReadbacks).
                    const bool gpuKept = m_gfxKeptValid;
                    uint32_t kept = gs.clustersProcessed - gs.clustersCulled;
                    if (gpuKept)
                        kept = std::min(m_gfxKeptBatches, gs.clustersProcessed);
                    ImGui::Text("view: %u draws %u cmds %u tris | clusters %u kept %u culled %u%s",
                                gs.draws,
                                gs.commands,
                                gs.trianglesProcessed,
                                gs.clustersProcessed,
                                kept,
                                gs.clustersProcessed - kept,
                                gpuKept ? " (GPU cull)" : "");
                    ImGui::Text("materials: %u ranges  batches: %u",
                                (unsigned)m_gfxRenderer.LastMaterialRangeCount(),
                                (unsigned)m_gfxRenderer.LastBatchCount());
                    {
                        const bool occOn = (m_gfxRenderer.config.cullFlags &
                                            smvk::gfx::CullFlagBit_HzbOcclusion) != 0;
                        uint32_t hw = 0, hh = 0, hm = 0;
                        const uint32_t gfxSlot = m_gfxRenderer.LastSlot();
                        if (gfxSlot != smvk::RenderViewSet::kInvalidIndex &&
                            gfxSlot < m_gfxRenderer.State().temporals.size()) {
                            const smvk::gfx::HizResources& hz =
                                smvk::gfx::Hiz(m_gfxRenderer.State(), gfxSlot);
                            hw = hz.width;
                            hh = hz.height;
                            hm = hz.mipCount;
                        }
                        const bool depthOk =
                            m_viewportResources.hizDepth.bindlessIndex != UINT32_MAX;
                        const bool cullingFrozen = m_freezeCullingView && m_frozenViewValid;
                        ImGui::Text("hzb: %ux%u/%u mips depthIdx=%s occ=%s cull=%s",
                                    hw,
                                    hh,
                                    hm,
                                    depthOk ? "ok" : "INVALID",
                                    occOn ? "on" : "OFF",
                                    cullingFrozen ? "FROZEN" : "live");
                        if (!occOn)
                            ImGui::TextDisabled("HZB flag OFF: no occlusion culls possible.");
                        else if (hm == 0)
                            ImGui::TextDisabled("HZB empty: occlusion test disabled (extent 0?).");
                        else if (!depthOk)
                            ImGui::TextDisabled("Depth bindless INVALID: HZB reads garbage.");
                    }
                    ImGui::Separator();
                    ImGui::Text("LODs: %u meshes / %u siblings cooked%s",
                                m_lodChainMeshes,
                                m_lodChainLods,
                                m_lodEnabled ? "" : "  (OFF - always LOD0)");
                    if (m_lodEnabled && m_lodChainMeshes == 0)
                        ImGui::TextDisabled(
                            "No cooked LOD chain in this scene - cook with --lods on.");
                    if (!ShaderLodActive())
                        ImGui::TextDisabled("Shader LODs OFF - every level full-lit.");
                }

                ImGui::SeparatorText("Keys");
                ImGui::TextDisabled("[I] %s  [ESC] quit",
                                    m_uiModeHint ? "UI (NORMAL) -> press I for FPS"
                                                 : "FPS (DISABLED) -> press I for UI");
                ImGui::TextDisabled("Keys (toggles): TAB debug panel  M blur  O AO(No/SSAO/SAO)  B "
                                    "bloom  X exposure  V vectors  C map");
                ImGui::TextDisabled("      L LODs  F particles  Ctrl+F freeze cull  G filter  K "
                                    "sky  H shadows  T terrain");
                ImGui::TextDisabled("      J blood splat at crosshair  TOURMALINE_DECALS=0/1");
                ImGui::TextDisabled(
                    "Keys (views): N ambient  R/F8 GI view  F1 lit  F2 unlit  F3 meshlets");
                ImGui::TextDisabled("      F4 cascades  F5 LOD colors  F6 vertex colors  F9 UV0  "
                                    "F10 cubemap  F11 POM");
#if defined(DS_DEBUG_VIZ)
                ImGui::TextDisabled("      Y DS visualizer (%s)",
                                    m_dsDebugMode == DsDebugViz::Off ? "off" : "on");
#endif
                ImGui::TextDisabled("Keys (actions): Z/1-4 AA  P wireframe  F7 capture  ` texture "
                                    "viewer  Ctrl+T stats HUD");
                ImGui::EndTabItem();
            }
            // Export the active cooked-scene camera and replayable settings
            // as a git-tracked stress scenario. This is not displayed by the
            // editor, whose borrowed world has no stable pak entry point.
            if (!m_benchmarkSourceArgs.empty() && ImGui::BeginTabItem("Benchmarks")) {
                ImGui::SeparatorText("Benchmark scenarios");
                ImGui::InputText("Label##bench", m_benchmarkLabel, sizeof(m_benchmarkLabel));
                ImGui::InputText(
                    "Subsystem##bench", m_benchmarkSubsystem, sizeof(m_benchmarkSubsystem));
                ImGui::InputTextMultiline("Notes##bench",
                                          m_benchmarkNotes,
                                          sizeof(m_benchmarkNotes),
                                          ImVec2(-1.0f, 48.0f));
                if (ImGui::Button("Export for benchmark"))
                    ExportBenchmarkScenario();
                ImGui::TextDisabled(
                    "Saves camera and supported run pins to benchmarks/scenarios.json");
                if (!m_benchmarkExportMessage.empty())
                    ImGui::TextWrapped("%s", m_benchmarkExportMessage.c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    } // !m_debugPanelOpen (the TAB toggle / close X above)
}
} // namespace engine

namespace engine {
void WorldRenderer::SelectTexDebugTexture(const smvk::TextureInfo& t) {
    DropTexDebugSelection();
    m_texDebugSelected = t.handle;
    const bool plain2d = t.type == smvk::ImageType::Image2D && !t.isCubemap && t.arrayLayers == 1;
    // Cubemap (or 6*N cube array) preview: per-face 2D views.
    // Cube arrays preview cube 0 (faces 0..5).
    const bool cube = t.isCubemap && smvk::IsCubemapLayerCount(t.arrayLayers);
    if (plain2d && t.bindless >= 0) {
        // The overlay's user-texture binding: the backend's descriptor
        // registration wrapped behind RHI-opaque types (a dead handle
        // returns 0 there - the preview simply doesn't appear).
        m_texDebugPreview = m_uiTextureService.addTexture
                                ? m_uiTextureService.addTexture(t.handle, m_texDebugSampler)
                                : 0;
    } else if (cube) {
        // A dead view (destroyed, generation bumped) returns 0 - that
        // face's preview simply doesn't appear.
        static const char* const kFaceNames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
        const smvk::ImageHandle img = m_context->GetTextureImageHandle(t.handle);
        for (uint32_t face = 0; face < 6 && !img.empty(); ++face) {
            smvk::ImageViewCreateInfo vci{};
            vci.viewType = smvk::ImageViewType::View2D;
            vci.format = smvk::Format::Undefined; // inherit
            vci.subresourceRange = {
                smvk::ImageAspect::Color, 0, smvk::RemainingSubresources, face, 1};
            char name[48];
            std::snprintf(name, sizeof(name), "texviewer face %s", kFaceNames[face]);
            vci.name = name; // consumed at creation (debug names only)
            m_texDebugCubeViews[face] = m_context->CreateImageView(img, vci);
            if (!m_texDebugCubeViews[face].empty())
                m_texDebugCubePreview[face] =
                    m_uiTextureService.addView
                        ? m_uiTextureService.addView(m_texDebugCubeViews[face], m_texDebugSampler)
                        : 0;
        }
    }
}

// Debug-view hint (see the header note): names the active view + its
// color coding. Precedence: the DS visualizers and the GI debug
// views imply m_debugView == Lit (SetDebugView's take-over + the
// cycle snaps), so exactly one layer is named at a time.
void WorldRenderer::DrawDebugViewHint() {
    // The Ssgi* diffuse views are EXCLUDED from
    // DebugVisualizationActive (their chain must stay live to have
    // something to visualize), so they pass the gate below too.
    if (!DebugVisualizationActive() && !IsSsgiDiffuseDebugView(m_debugView) &&
        !IsClippingDebugView(m_debugView))
        return;
    // Resolve the title + the legend lines for the active layer.
    const char* title = nullptr;
    const char* lines[3] = {nullptr, nullptr, nullptr};
#if defined(DS_DEBUG_VIZ)
    if (m_dsDebugMode != DsDebugViz::Off) {
        switch (m_dsDebugMode) {
        case DsDebugViz::CountApprox:
        case DsDebugViz::CountAccurate:
        case DsDebugViz::CountFinal:
        case DsDebugViz::CountDiff:
            title = "DS visualizer: light-count heat";
            lines[0] = "black = 0 lights -> blue/cyan/green/yellow -> red at the ceiling.";
            lines[1] = "8px Shadow Tiles; faint grid = 64px TileGroups.";
            break;
        case DsDebugViz::CullStep:
            title = "DS visualizer: cull step (per light)";
            lines[0] = "RED ph1 mask / ORANGE ph2 zBin (on lit pixels = ERRONEOUS CULL),";
            lines[1] = "BLUE ph4 all-shadow drop, GREEN full pass (brightness = visibility).";
            break;
        case DsDebugViz::LightList:
            title = "DS visualizer: light list";
            lines[0] = "RED = no list / ORANGE mask-only (erroneous) / YELLOW early-only";
            lines[1] = "(shadow-dropped) / GREEN final list (brightness = visibility).";
            break;
        default:
            title = "DS visualizer: light visibility";
            lines[0] = "WHITE lit -> BLACK shadowed; dark blue = not in the final list.";
            break;
        }
    } else
#endif
        if (m_giDebug != 0) {
        switch (m_giDebug) {
        case 1:
            title = "GI debug: baked irradiance (R/F8)";
            lines[0] = "flat irradiance of the active ambient source (N isolates it);";
            lines[1] = "magenta = no source at this pixel.";
            break;
        case 2:
            title = "GI debug: source palette (R/F8)";
            lines[0] = "red = lightmap (deprecated), green = regular grid (legacy),";
            lines[1] = "blue = octree world field, cyan = convex volume,";
            lines[2] = "magenta = none.";
            break;
        case 3:
            title = "GI debug: probe distance (R/F8)";
            lines[0] = "nearest probe's obstacle distance: red = on/in a surface,";
            lines[1] = "yellow = near (< 1 m), green = open.";
            break;
        case 4:
            title = "GI debug: visibility wSum (R/F8)";
            lines[0] = "pre-normalization probe weight total: red = 0 (blocked),";
            lines[1] = "yellow = 0.5, green = 1 (clean / no rejection).";
            lines[2] = "magenta = no GI volume at this pixel.";
            break;
        default:
            break;
        }
    } else {
        switch (m_debugView) {
        case ViewMode::Unlit:
            title = "F2 Unlit";
            lines[0] = "base color only (albedo x vertex tint) - no lighting.";
            break;
        case ViewMode::Meshlets:
            if (m_debugMeshletMode == 0) {
                title = "F3 Meshlets (random)";
                lines[0] = "random hue per meshlet; culled clusters stay visible";
                lines[1] = "(every registered cluster is colored).";
            } else {
                title = "F3 Meshlets (cone heat)";
                lines[0] = "red = degenerate cone (never culls), green = tight cone.";
            }
            break;
        case ViewMode::Cascades:
            title = "F4 Cascades";
            lines[0] = "cascade 0 red, 1 green, 2 blue, 3 yellow, beyond magenta;";
            lines[1] = "brightness = shadow factor (dark = shadowed).";
            break;
        case ViewMode::Lods:
            title = "F5 LODs";
            lines[0] = "LOD0 green, 1 yellow, 2 orange, 3 red, 4+ magenta.";
            break;
        case ViewMode::VertexColors:
            title = "F6 Vertex Colors";
            lines[0] = "layered materials: R/G/B/Y = layer 0..3 blend weights;";
            lines[1] = "legacy materials: raw vertex tint.";
            break;
        case ViewMode::Uv0:
            title = "F9 UV0 (base)";
            lines[0] = "base TEXCOORD0 checkers; magenta glow = huge uv";
            lines[1] = "screen-derivative (a chart corner in the wrong rect).";
            break;
        case ViewMode::Uv1:
            title = "UV1 (lightmap atlas)";
            lines[0] = "atlas uv (chart uv + id); magenta stripes when the uv";
            lines[1] = "leaves [0,1] (transform bug).";
            break;
        case ViewMode::Cubemap:
            title = "F10 Cubemap (IBL raw)";
            lines[0] = "raw probe radiance, no Fresnel/roughness weighting -";
            lines[1] = "shows which probe serves each pixel.";
            break;
        case ViewMode::Pom:
            title = "F11 POM";
            lines[0] = "parallax-marched uv + sun self-shadow;";
            lines[1] = "no occlusion map = plain sampling.";
            break;
        case ViewMode::SsrMask:
            title = "SSR mask (receivers)";
            lines[0] = "green = smooth dielectric, blue = smooth metal,";
            lines[1] = "black = matte (skipped), dimmed = Blend.";
            break;
        case ViewMode::Normals:
            title = "Normals (world space)";
            lines[0] = "world normal as color (n*0.5+0.5): +X red, +Y green, +Z blue.";
            lines[1] = "geometric normal only - no normal-map perturbation";
            lines[2] = "(the mapped view owns those).";
            break;
        case ViewMode::NormalsMapped:
            title = "Normals (mapped, world space)";
            lines[0] = "the SHADING normal as color: vertex normal + the";
            lines[1] = "material's normal map (lit-path tangent-space math);";
            lines[2] = "layered materials fall back to the geometric normal.";
            break;
        case ViewMode::MotionVectors:
            title = "Motion vectors (V)";
            lines[0] = "direction -> hue, magnitude -> intensity;";
            lines[1] = "the temporal jitter is stripped (TRUE motion).";
            break;
        case ViewMode::SsrContribution:
            title = "SSR contribution";
            lines[0] = "the reflection layer's exact add term.";
            break;
        case ViewMode::ContactShadows:
            title = "Sun contact shadows: contribution";
            lines[0] = "R = contact march, G = CSM/EVSM; green = the march's";
            lines[1] = "contribution (cascade missed it), red = the march";
            lines[2] = "missed (tune thickness/res); magenta = not running.";
            break;
        case ViewMode::SsgiConfidence:
            title = "SSGI diffuse: confidence";
            lines[0] = "grayscale hit confidence; expect smooth loss at";
            lines[1] = "screen boundaries (needs SSGI enabled).";
            break;
        case ViewMode::SsgiRaw:
            title = "SSGI diffuse: raw radiance";
            lines[0] = "the pre-filter trace: noisy, recognizable";
            lines[1] = "incoming lighting (raw HDR values).";
            break;
        case ViewMode::SsgiLayer:
            title = "SSGI diffuse: filtered layer";
            lines[0] = "the blur/temporal layer the compose consumes";
            lines[1] = "(converges while the camera holds still).";
            break;
        case ViewMode::SsgiBaseline:
            title = "SSGI diffuse: ambient baseline";
            lines[0] = "the existing probe/sky diffuse ambient being";
            lines[1] = "replaced (max(aoTerm - envSpec, 0) x aoEff).";
            break;
        case ViewMode::SsgiContribution:
            title = "SSGI diffuse: contribution";
            lines[0] = "the SIGNED s x (ssgi - baseline) delta:";
            lines[1] = "green = brightens, red = dimmer than the baked";
            lines[2] = "ambient it replaces, black = no change.";
            break;
        case ViewMode::ClippingCombined:
        case ViewMode::ClippingShadows:
        case ViewMode::ClippingHighlights:
            title = m_debugView == ViewMode::ClippingShadows ? "Clipping: shadows"
                    : m_debugView == ViewMode::ClippingHighlights
                        ? "Clipping: highlights"
                        : "Clipping: shadows + highlights";
            lines[0] = "BLUE = display luma at/below the shadow threshold,";
            lines[1] = "RED = at/above the highlight threshold (sliders in";
            lines[2] = "the Debug panel; in-range pixels keep their color).";
            break;
        default:
            return; // Lit with no overlays - nothing to hint
        }
    }
    // Top-center, semi-transparent, click-through: pure annotation
    // (never steals input from the scene). Fixed narrow width so the
    // legends wrap at a readable measure.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, 10.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.60f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    if (!ImGui::Begin("##tourmaline_dbgview_hint",
                      nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TextColored(ImVec4(0.62f, 0.90f, 0.66f, 1.0f), "%s", title);
    for (const char* l : lines) {
        if (!l)
            break;
        ImGui::TextDisabled("%s", l);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void WorldRenderer::DrawHudOverlay() {
    if (!m_showHud)
        return;
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (!ImGui::Begin("##tourmaline_hud",
                      nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                          ImGuiWindowFlags_NoInputs)) {
        ImGui::End();
        return;
    }
    // GPU: the ACTIVE Vulkan device's name (captured at the device
    // pick — smvk::ActiveDeviceName(); never hardcoded/guessed), above
    // the perf lines so the hardware context leads the readout.
    ImGui::Text("GPU: %s", smvk::ActiveDeviceName());
    // Frame rate + CPU/GPU times (same sources as the old panel header).
    const float fps = ImGui::GetIO().Framerate;
    if (m_gpuTimingValid)
        ImGui::Text("%.0f FPS (%.1f ms) CPU %.2f GPU %.2f",
                    (double)fps,
                    fps > 1e-6f ? 1000.0f / fps : 0.0,
                    m_cpuMsSmooth,
                    m_gpuMsSmooth);
    else
        ImGui::Text("%.0f FPS (%.1f ms) CPU %.2f GPU ... (warming up)",
                    (double)fps,
                    fps > 1e-6f ? 1000.0f / fps : 0.0,
                    m_cpuMsSmooth);
    // GPU memory (VMA budgets; throttled to 1 Hz inside). DEVICE_LOCAL
    // (VRAM) heaps ONLY: summing every heap double-counts host budgets -
    // a 16 GiB card would read ~30 GiB "total". Fallback: if no heap
    // reports deviceLocal (integrated/uniform-memory GPUs), sum all.
    RefreshGpuMemoryStats(false);
    if (!m_gpuMemHeaps.empty()) {
        uint64_t totalUsed = 0, totalBudget = 0;
        bool anyDeviceLocal = false;
        for (const auto& h : m_gpuMemHeaps) {
            if (!h.deviceLocal)
                continue;
            anyDeviceLocal = true;
            totalUsed += h.usageBytes;
            totalBudget += h.budgetBytes;
        }
        if (!anyDeviceLocal) {
            for (const auto& h : m_gpuMemHeaps) {
                totalUsed += h.usageBytes;
                totalBudget += h.budgetBytes;
            }
        }
        const double usedMiB = (double)totalUsed / 1048576.0;
        const double budgetMiB = (double)totalBudget / 1048576.0;
        ImGui::Text("%s %.0f/%.0f MiB (%.0f%%)",
                    anyDeviceLocal ? "VRAM" : "GPU mem",
                    usedMiB,
                    budgetMiB,
                    totalBudget > 0 ? 100.0 * usedMiB / budgetMiB : 0.0);
    } else {
        ImGui::TextDisabled("GPU mem n/a");
    }
    // Camera pose (same sources as the Stats tab camera rows).
    {
        const glm::vec3& p = m_cameraPositioner.cameraPosition_;
        const glm::vec3 fwd = m_cameraPositioner.getForward();
        ImGui::Text("cam %.2f %.2f %.2f", p.x, p.y, p.z);
        ImGui::Text("fwd %.3f %.3f %.3f", fwd.x, fwd.y, fwd.z);
    }
    // Filter culls (last filtered view; zeros when inactive). GPU mode
    // (the default with VK_EXT_mesh_shader): the cull runs in COMPUTE
    // (gfxbatchcull) - the CPU stat only counts candidates, so the kept
    // number lands via the async counter-word readback (~a few frames
    // stale) and culled = candidates - kept.
    {
        const smvk::gfx::Stats fs = m_gfxRenderer.LastStats();
        const bool gpuKept = m_gfxKeptValid;
        uint32_t kept = fs.clustersProcessed - fs.clustersCulled;
        if (gpuKept)
            kept = std::min(m_gfxKeptBatches, fs.clustersProcessed);
        // 6 numeric + 1 string specs = 7 args EXACTLY (a missing arg
        // reads stack garbage as the %s pointer - an AV in the CRT's
        // strlen; this exact bug crashed every frame-0 run).
        ImGui::Text("filter draws %u cmd %u tris %u | cl %u kept %u culled %u%s",
                    fs.draws,
                    fs.commands,
                    fs.trianglesProcessed,
                    fs.clustersProcessed,
                    kept,
                    fs.clustersProcessed - kept,
                    gpuKept ? " (GPU)" : "");
    }
    // LOD decisions this frame (World-level stats: every leg - filter,
    // prepass, shadow, direct, motion - counts once at its first query;
    // far = far-shading draws counted at the Main direct leg).
    ImGui::Text("LOD %u draws (%u sw, %llu far)",
                m_world ? m_world->LodStatDraws() : 0u,
                m_world ? m_world->LodStatSwitched() : 0u,
                (unsigned long long)m_lodShaderDraws);
    // Exposure (applied + target).
    ImGui::Text("exp %.5f (tgt %.5f)", (double)FrameExposure(), (double)m_exposureTarget);
    // Texture streaming residency.
    if (m_textureStreamer) {
        const smvk::TextureStreamingStats tstats = m_textureStreamer->GetStats();
        const double resMiB = (double)tstats.residentBytes / 1048576.0;
        const double fullMiB = (double)tstats.fullBytes / 1048576.0;
        ImGui::Text("stream %u tex %.0f/%.0f MiB (%.0f%%) +%u -%u",
                    tstats.textureCount,
                    resMiB,
                    fullMiB,
                    fullMiB > 0.0 ? 100.0 * (1.0 - resMiB / fullMiB) : 0.0,
                    tstats.upgradesLastUpdate,
                    tstats.downgradesLastUpdate);
    } else {
        ImGui::TextDisabled("stream off (full-resident)");
    }
    ImGui::End();
}

// World Map viewer (C): floating top-down orthographic canvas.
// Background = the baked terrain's CPU heightfield downsampled to a
// 256px preview (built once per terrain), else an empty grid over
// the scene bounds (or +/-2048 m with no world). Overlays read live
// scene state every frame (cheap: markers only, capped counts).
// LATER layers slot in as one bool + one draw block each (shadow
// cascade boxes, terrain LOD heat, env zone weights).
void WorldRenderer::BuildWorldMapHeight() {
    DropWorldMapHeight();
    if (!m_terrain.valid || m_terrain.hfield.tiles.empty())
        return;
    const smvk::terrain::TerrainHField& hf = m_terrain.hfield;
    constexpr int N = 256;
    const float minX = m_terrain.boundsMin.x;
    const float minZ = m_terrain.boundsMin.z;
    const float sizeX = m_terrain.boundsMax.x - minX;
    const float sizeZ = m_terrain.boundsMax.z - minZ;
    if (!(sizeX > 0.0f) || !(sizeZ > 0.0f))
        return;
    std::vector<uint8_t> px(size_t(N) * size_t(N) * 4);
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            const float x = minX + (float(i) + 0.5f) / float(N) * sizeX;
            const float z = minZ + (float(j) + 0.5f) / float(N) * sizeZ;
            const float t = std::clamp(smvk::terrain::SampleH01(hf, x, z), 0.0f, 1.0f);
            // Hypsometric ramp (low green -> brown -> gray -> snow).
            glm::vec3 c;
            if (t < 0.4f)
                c = glm::mix(
                    glm::vec3(0.09f, 0.18f, 0.12f), glm::vec3(0.35f, 0.43f, 0.24f), t / 0.4f);
            else if (t < 0.7f)
                c = glm::mix(glm::vec3(0.35f, 0.43f, 0.24f),
                             glm::vec3(0.55f, 0.47f, 0.35f),
                             (t - 0.4f) / 0.3f);
            else
                c = glm::mix(glm::vec3(0.55f, 0.47f, 0.35f),
                             glm::vec3(0.92f, 0.92f, 0.94f),
                             (t - 0.7f) / 0.3f);
            uint8_t* o = px.data() + (size_t(j) * size_t(N) + size_t(i)) * 4;
            o[0] = uint8_t(c.r * 255.0f);
            o[1] = uint8_t(c.g * 255.0f);
            o[2] = uint8_t(c.b * 255.0f);
            o[3] = 255;
        }
    }
    smvk::ImageCreateInfo ci;
    ci.type = smvk::ImageType::Image2D;
    ci.extent = smvk::Extent3D{uint32_t(N), uint32_t(N), 1};
    ci.format = smvk::Format::RGBA8_UNorm;
    ci.mipLevels = 1;
    ci.name = "worldmap/height";
    m_worldMapHeightTex = m_context->CreateTexture(ci, px.data());
    if (m_worldMapHeightTex.empty()) {
        smvk::OnResourceExhaustion("worldmap/height");
        return;
    }
    if (m_worldMapSampler == UINT32_MAX) {
        smvk::SamplerCreateInfo sci{};
        sci.magFilter = smvk::Filter::Linear;
        sci.minFilter = smvk::Filter::Linear;
        sci.mipmapMode = smvk::MipmapMode::Nearest;
        sci.addressU = smvk::AddressMode::ClampToEdge;
        sci.addressV = smvk::AddressMode::ClampToEdge;
        sci.addressW = smvk::AddressMode::ClampToEdge;
        m_worldMapSampler = m_context->GetOrCreateSampler(sci);
    }
    m_worldMapHeightId = m_uiTextureService.addTexture
                             ? m_uiTextureService.addTexture(m_worldMapHeightTex, m_worldMapSampler)
                             : 0;
}

void WorldRenderer::DropWorldMapHeight() {
    if (m_worldMapHeightId != 0) {
        // Deferred retire (see the overlay contract): the binding's
        // descriptor set is referenced by the draw data of the frame
        // that last drew the map background (+ in-flight submissions) —
        // the immediate free bound a freed set (device lost).
        if (m_uiTextureService.retire)
            m_uiTextureService.retire(m_worldMapHeightId);
        m_worldMapHeightId = 0;
    }
    m_worldMapHeightTex = {};
}

void WorldRenderer::DrawWorldMapWindow() {
    ImGui::SetNextWindowSize(ImVec2(640.0f, 580.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("World Map (C)", &m_worldMapWindow)) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox("Env volumes", &m_worldMapEnvVolumes);
    ImGui::SameLine();
    ImGui::Checkbox("GI volumes", &m_worldMapGiVolumes);
    ImGui::SameLine();
    ImGui::Checkbox("Probes", &m_worldMapProbes);
    ImGui::SameLine();
    ImGui::Checkbox("Lights", &m_worldMapLights);
    ImGui::SameLine();
    ImGui::Checkbox("Camera", &m_worldMapCamera);
    ImGui::SameLine();
    if (ImGui::Button("Fit"))
        m_worldMapScale = 0.0f; // fit on next draw below
    // Streaming layers (the WorldFields + WorldPartition debug — see
    // the draw blocks below): field residency, the fog density
    // reference heat, the partition interest cells.
    ImGui::Checkbox("Fields", &m_worldMapFields);
    ImGui::SameLine();
    ImGui::Checkbox("Fog heat (proc. ref)", &m_worldMapFogHeat);
    ImGui::SameLine();
    ImGui::Checkbox("Cells", &m_worldMapCells);
    // World extent: terrain bounds, else scene bounds, else default.
    float minX = -2048.0f, minZ = -2048.0f;
    float sizeX = 4096.0f, sizeZ = 4096.0f;
    const bool hasH = m_terrain.valid;
    if (hasH) {
        minX = m_terrain.boundsMin.x;
        minZ = m_terrain.boundsMin.z;
        sizeX = m_terrain.boundsMax.x - minX;
        sizeZ = m_terrain.boundsMax.z - minZ;
    } else if (m_world) {
        const ::tour::Aabb b = m_world->ComputeSceneBounds();
        if (b.mx.x >= b.mn.x && b.mx.y >= b.mn.y && b.mx.z >= b.mn.z &&
            (b.mx.x > b.mn.x || b.mx.z > b.mn.z)) {
            minX = b.mn.x;
            minZ = b.mn.z;
            sizeX = std::max(b.mx.x - b.mn.x, 1.0f);
            sizeZ = std::max(b.mx.z - b.mn.z, 1.0f);
        }
    }
    // Preview lifecycle: build once per terrain, drop without one.
    if (hasH && m_worldMapHeightId == 0)
        BuildWorldMapHeight();
    if (!hasH && m_worldMapHeightId != 0)
        DropWorldMapHeight();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cw = std::max(avail.x, 50.0f);
    // Three footer lines: layer counts, probe visibility/controls,
    // and streaming stats.
    const float footer = 3.0f * ImGui::GetTextLineHeightWithSpacing();
    const float ch = std::max(avail.y - footer, 50.0f);
    if (m_worldMapScale <= 0.0f) {
        m_worldMapScale = std::min(cw, ch) / std::max(sizeX, sizeZ);
        m_worldMapCenter = glm::vec2(minX + sizeX * 0.5f, minZ + sizeZ * 0.5f);
    }
    m_worldMapScale = std::clamp(m_worldMapScale, 0.004f, 50.0f);
    const float s = m_worldMapScale;
    const float vx0 = m_worldMapCenter.x - cw / (2.0f * s);
    const float vz0 = m_worldMapCenter.y - ch / (2.0f * s);
    // Background: heightfield image (UV = view rect) or empty grid.
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    if (hasH && m_worldMapHeightId != 0) {
        const ImVec2 uv0((vx0 - minX) / sizeX, (vz0 - minZ) / sizeZ);
        const ImVec2 uv1((vx0 + cw / s - minX) / sizeX, (vz0 + ch / s - minZ) / sizeZ);
        ImGui::Image(m_worldMapHeightId, ImVec2(cw, ch), uv0, uv1);
    } else {
        ImDrawList* bg = ImGui::GetWindowDrawList();
        bg->AddRectFilled(c0, ImVec2(c0.x + cw, c0.y + ch), IM_COL32(12, 14, 18, 255));
    }
    ImGui::SetCursorScreenPos(c0);
    ImGui::InvisibleButton("##worldmap_canvas", ImVec2(cw, ch));
    const ImVec2 o = ImGui::GetItemRectMin();
    // Navigate: drag pans (grab-world), wheel zooms cursor-anchored.
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        m_worldMapCenter.x -= d.x / s;
        m_worldMapCenter.y -= d.y / s;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
    }
    if (ImGui::IsItemHovered()) {
        const float wh = ImGui::GetIO().MouseWheel;
        if (wh != 0.0f) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float ns = std::clamp(s * std::pow(1.1f, wh), 0.004f, 50.0f);
            // Keep the world point under the cursor fixed.
            const float wx = vx0 + (m.x - o.x) / s;
            const float wz = vz0 + (m.y - o.y) / s;
            m_worldMapCenter.x = wx - (m.x - o.x - cw * 0.5f) / ns;
            m_worldMapCenter.y = wz - (m.y - o.y - ch * 0.5f) / ns;
            m_worldMapScale = ns;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(o, ImVec2(o.x + cw, o.y + ch), true);
    if (!hasH || m_worldMapHeightId == 0) {
        // Empty grid: 100 m lines world-aligned + origin axes.
        for (float gx = std::ceil(vx0 / 100.0f) * 100.0f; gx < vx0 + cw / s; gx += 100.0f) {
            const float mx = o.x + (gx - vx0) * s;
            const bool axis = std::abs(gx) < 1e-3f;
            dl->AddLine(ImVec2(mx, o.y),
                        ImVec2(mx, o.y + ch),
                        axis ? IM_COL32(120, 140, 160, 255) : IM_COL32(40, 46, 56, 255));
        }
        for (float gz = std::ceil(vz0 / 100.0f) * 100.0f; gz < vz0 + ch / s; gz += 100.0f) {
            const float mz = o.y + (gz - vz0) * s;
            const bool axis = std::abs(gz) < 1e-3f;
            dl->AddLine(ImVec2(o.x, mz),
                        ImVec2(o.x + cw, mz),
                        axis ? IM_COL32(120, 140, 160, 255) : IM_COL32(40, 46, 56, 255));
        }
        dl->AddText(
            ImVec2(o.x + 6.0f, o.y + 4.0f), IM_COL32(140, 150, 165, 255), "grid (no terrain)");
    }
    const auto WX = [&](float x) { return o.x + (x - vx0) * s; };
    const auto WZ = [&](float z) { return o.y + (z - vz0) * s; };
    const auto toSrgb8 = [](float c) {
        c = std::min(std::max(c, 0.0f), 1.0f);
        return (int)std::lround(std::pow(c, 1.0f / 2.2f) * 255.0f);
    };
    // ---- WorldFields layers (Engine/WorldFields; field id 0 = the
    // fog density field). LOD palette: 0 cyan, 1 green, 2 amber. ----
    const bool fogFieldLive = m_worldFields.GpuBinding().Valid();
    const glm::vec3 mapEye = m_cameraPositioner.cameraPosition_;
    constexpr uint32_t kMapFogFieldId = 0;
    if (m_worldMapFogHeat && fogFieldLive) {
        // Fog density heat — SOURCE-AWARE (the review's finding): the
        // layer evaluates the PROCEDURAL REFERENCE, which is exactly
        // what the bricks encode ONLY while the fog field's installed
        // source is g_fogDensitySource. A cooked/alternate source
        // would make this evaluation a LIE — the heatmap would
        // disagree with the fog actually rendered — so the layer
        // disables itself and says why, instead of silently showing
        // the wrong function.
        const bool proceduralReference = FogFieldUsesProceduralReference();
        if (!proceduralReference) {
            dl->AddText(ImVec2(o.x + 6.0f, o.y + 4.0f),
                        IM_COL32(220, 160, 90, 255),
                        "fog heat: source is not the procedural reference "
                        "(layer disabled)");
        } else {
            // The procedural reference sampled on a world-quantized
            // ~24 px grid at the camera's height (the function the
            // bricks encode — the layer label keeps that explicit).
            const float worldCell = std::max(24.0f / s, 1.0f);
            const float x1 = vx0 + cw / s;
            const float z1 = vz0 + ch / s;
            for (float gz = std::floor(vz0 / worldCell) * worldCell; gz < z1; gz += worldCell)
                for (float gx = std::floor(vx0 / worldCell) * worldCell; gx < x1; gx += worldCell) {
                    const float d =
                        WorldRenderer::ProceduralFogDensity01(glm::vec3(gx, mapEye.y, gz));
                    if (d <= 0.02f)
                        continue;
                    // (density^0.6 keeps the low range visible; blue ->
                    // white ramp, alpha carries the value)
                    const float t = std::pow(std::min(d, 1.0f), 0.6f);
                    const int r = 60 + int(t * 180.0f);
                    const int g = 150 + int(t * 90.0f);
                    const int b = 230;
                    dl->AddRectFilled(
                        ImVec2(WX(gx), WZ(gz)),
                        ImVec2(WX(std::min(gx + worldCell, x1)), WZ(std::min(gz + worldCell, z1))),
                        IM_COL32(r, g, b, int(t * 96.0f)));
                }
        }
    }
    if (m_worldMapCells) {
        // ---- WorldPartition layer: the streaming interest(s) —
        // position + velocity lead arrow — and the 256 m root-cell /
        // 64 m subcell grid under the interest radius. ----
        const worldpartition::StreamingInterest* interests = m_worldPartition.Interests();
        const size_t interestCount = m_worldPartition.InterestCount();
        for (size_t i = 0; i < interestCount; ++i) {
            const worldpartition::StreamingInterest& in = interests[i];
            const float ir = float(std::max(in.radiusMeters, 0.0));
            const double cLoX = std::floor((in.position.x - ir) / worldpartition::kRootCellSize) *
                                worldpartition::kRootCellSize;
            const double cLoZ = std::floor((in.position.z - ir) / worldpartition::kRootCellSize) *
                                worldpartition::kRootCellSize;
            for (double cz = cLoZ; cz <= in.position.z + ir; cz += worldpartition::kRootCellSize)
                for (double cx = cLoX; cx <= in.position.x + ir;
                     cx += worldpartition::kRootCellSize) {
                    dl->AddRect(ImVec2(WX(float(cx)), WZ(float(cz))),
                                ImVec2(WX(float(cx + worldpartition::kRootCellSize)),
                                       WZ(float(cz + worldpartition::kRootCellSize))),
                                IM_COL32(120, 130, 165, 110),
                                1.0f);
                    // Subcells (64 m) only when they read (>6 px).
                    if (s * float(worldpartition::kSubcellSize) >= 6.0f) {
                        for (int sz = 0; sz < 4; ++sz)
                            for (int sx = 0; sx < 4; ++sx) {
                                const float scx =
                                    float(cx) + float(sx) * float(worldpartition::kSubcellSize);
                                const float scz =
                                    float(cz) + float(sz) * float(worldpartition::kSubcellSize);
                                dl->AddRect(ImVec2(WX(scx), WZ(scz)),
                                            ImVec2(WX(scx + float(worldpartition::kSubcellSize)),
                                                   WZ(scz + float(worldpartition::kSubcellSize))),
                                            IM_COL32(120, 130, 165, 38),
                                            1.0f);
                            }
                    }
                }
            // Interest: position dot + the velocity lead vector
            // (the prefetch rule the clipmap centers follow).
            const ImVec2 p(WX(float(in.position.x)), WZ(float(in.position.z)));
            dl->AddCircleFilled(p, 5.0f, IM_COL32(255, 255, 255, 220));
            const double vl = glm::length(in.velocity);
            if (vl > 0.1) {
                const glm::dvec3 lead = in.velocity * double(0.5);
                dl->AddLine(p,
                            ImVec2(p.x + float(lead.x) * s, p.y + float(lead.z) * s),
                            IM_COL32(255, 235, 130, 220),
                            2.0f);
            }
        }
    }
    uint32_t nFieldBricks = 0, nFieldWindows = 0, nFieldInFlight = 0;
    if (m_worldMapFields && fogFieldLive) {
        // One-shot headless smoke evidence: the capture composite is
        // scene-only BY DESIGN (a grading reference — the UI layers
        // never appear in exports), so this log is the proof the
        // fields layer drew with live data.
        static bool s_loggedFirstDraw = false;
        if (!s_loggedFirstDraw) {
            s_loggedFirstDraw = true;
            const worldfields::WorldFieldStats st = m_worldFields.Stats();
            std::fprintf(stderr,
                         "[WorldMap] fields layer live: %u residents,"
                         " %u in-flight, %u pool free, %.1f MiB atlas\n",
                         st.residentBricks,
                         st.inFlightBricks,
                         st.freeSlots,
                         st.atlasBytes / (1024.0 * 1024.0));
        }
        // ---- Clipmap windows: one outline per LOD (bright -> dim),
        // then resident brick footprints (filled, LOD palette) and
        // in-flight generations (1 px outline — the streaming edge). ----
        const auto lodColor = [](uint32_t lod, int alpha) -> ImU32 {
            static constexpr int kPal[3][3] = {{90, 200, 235}, {110, 210, 120}, {235, 180, 90}};
            const int* k = kPal[lod < 3 ? lod : 2];
            return IM_COL32(k[0], k[1], k[2], alpha);
        };
        constexpr uint32_t kMaxDebugWindows = 8;
        worldfields::WorldFieldSystem::WorldFieldDebugWindow windows[kMaxDebugWindows];
        const uint32_t windowTotal =
            m_worldFields.CollectDebugWindows(kMapFogFieldId, windows, kMaxDebugWindows);
        nFieldWindows = std::min(windowTotal, kMaxDebugWindows);
        // Per-LOD world brick edge (lookup for the brick footprints).
        float brickEdge[worldfields::kMaxFieldLods] = {};
        glm::vec3 fieldOrigin(0.0f);
        for (uint32_t w = 0; w < nFieldWindows; ++w) {
            const auto& win = windows[w];
            if (win.lod < worldfields::kMaxFieldLods)
                brickEdge[win.lod] = float(worldfields::kBrickInteriorRes) * win.voxelSizeMeters;
            fieldOrigin = glm::vec3(win.originMeters[0], win.originMeters[1], win.originMeters[2]);
            const float edge = float(worldfields::kBrickInteriorRes) * win.voxelSizeMeters;
            const float wx0 = fieldOrigin.x + float(win.minX) * edge;
            const float wz0 = fieldOrigin.z + float(win.minZ) * edge;
            const float wx1 = fieldOrigin.x + float(win.minX + int32_t(win.dimX)) * edge;
            const float wz1 = fieldOrigin.z + float(win.minZ + int32_t(win.dimZ)) * edge;
            // Window outline (alpha falls off with LOD: the fallback).
            dl->AddRect(ImVec2(WX(wx0), WZ(wz0)),
                        ImVec2(WX(wx1), WZ(wz1)),
                        lodColor(win.lod, int(150 - 40 * int(win.lod))),
                        1.5f);
            // LOD tag at the window corner (when it fits).
            char tag[8];
            std::snprintf(tag, sizeof(tag), "L%u", win.lod);
            dl->AddText(ImVec2(WX(wx0) + 3.0f, WZ(wz0) + 3.0f), lodColor(win.lod, 190), tag);
        }
        // Resident bricks (cull: outside the view or < 2.5 px).
        m_worldMapBrickScratch.resize(4096);
        const uint32_t brickTotal = m_worldFields.CollectDebugBricks(
            kMapFogFieldId, m_worldMapBrickScratch.data(), uint32_t(m_worldMapBrickScratch.size()));
        nFieldBricks = std::min(brickTotal, uint32_t(m_worldMapBrickScratch.size()));
        const float x1 = vx0 + cw / s;
        const float z1 = vz0 + ch / s;
        for (uint32_t b = 0; b < nFieldBricks; ++b) {
            const auto& key = m_worldMapBrickScratch[b];
            const float edge = brickEdge[key.lod];
            if (edge <= 0.0f)
                continue; // no established window at that LOD
            const float bx0 = fieldOrigin.x + float(key.x) * edge;
            const float bz0 = fieldOrigin.z + float(key.z) * edge;
            const float px = edge * s;
            if (px < 2.5f || bx0 + edge < vx0 || bx0 > x1 || bz0 + edge < vz0 || bz0 > z1)
                continue;
            dl->AddRectFilled(ImVec2(WX(bx0), WZ(bz0)),
                              ImVec2(WX(bx0 + edge), WZ(bz0 + edge)),
                              lodColor(key.lod, int(56 - 12 * int(key.lod))));
        }
        // In-flight (the async work pool's current jobs: 1 px outline).
        worldfields::WorldFieldSystem::WorldFieldDebugBrick inFlight[64];
        const uint32_t inFlightTotal =
            m_worldFields.CollectDebugInFlight(kMapFogFieldId, inFlight, 64);
        nFieldInFlight = std::min(inFlightTotal, 64u);
        for (uint32_t b = 0; b < nFieldInFlight; ++b) {
            const auto& key = inFlight[b];
            const float edge = brickEdge[key.lod];
            if (edge <= 0.0f)
                continue;
            const float bx0 = fieldOrigin.x + float(key.x) * edge;
            const float bz0 = fieldOrigin.z + float(key.z) * edge;
            if (bx0 + edge < vx0 || bx0 > x1 || bz0 + edge < vz0 || bz0 > z1)
                continue;
            dl->AddRect(ImVec2(WX(bx0), WZ(bz0)),
                        ImVec2(WX(bx0 + edge), WZ(bz0 + edge)),
                        lodColor(key.lod, 160),
                        1.0f);
        }
    }
    uint32_t nZones = 0, nZonesOff = 0, nGiVolumes = 0, nProbes = 0, nLights = 0, nDecals = 0;
    // ---- Env volumes: OBB XZ footprints + inf-face arrows ---------
    if (m_worldMapEnvVolumes && m_world) {
        const ::Scene& scene = m_world->scene;
        for (const EnvRow& envRow : scene.tables.envs) {
            const NodeId node = envRow.node;
            const tour::scene::SceneEnv& env = envRow.environment;
            if (node >= scene.globalTransform.size())
                continue;
            ++nZones;
            const glm::mat4& g = scene.globalTransform[node];
            const glm::vec2 c(g[3].x, g[3].z);
            const glm::vec2 ax(g[0].x, g[0].z);
            const glm::vec2 az(g[2].x, g[2].z);
            uint32_t infMask = 0u;
            bool volDisabled = false;
            if (const EnvVolumeRow* vit = FindEnvVolume(scene, node)) {
                infMask = vit->volume.infMask;
                volDisabled = !vit->volume.enabled;
            }
            if (volDisabled)
                ++nZonesOff;
            const float pri = ::ZonePriority(scene, node, env);
            ImU32 col = pri < 1.0f    ? IM_COL32(110, 190, 110, 230)
                        : pri < 10.0f ? IM_COL32(230, 210, 90, 230)
                                      : IM_COL32(235, 130, 60, 230);
            if (volDisabled)
                col = IM_COL32(110, 110, 110, 140);
            const bool isActive = scene.activeEnvironment == node;
            if (isActive)
                col = IM_COL32(255, 255, 255, 255);
            const ImVec2 p[4] = {
                ImVec2(WX(c.x - ax.x - az.x), WZ(c.y - ax.y - az.y)),
                ImVec2(WX(c.x + ax.x - az.x), WZ(c.y + ax.y - az.y)),
                ImVec2(WX(c.x + ax.x + az.x), WZ(c.y + ax.y + az.y)),
                ImVec2(WX(c.x - ax.x + az.x), WZ(c.y - ax.y + az.y)),
            };
            dl->AddQuad(p[0], p[1], p[2], p[3], col, isActive ? 3.0f : 1.5f);
            // Infinite X/Z faces: arrows from the edge midpoint
            // outward (Y-only infinities show as a Y tag below).
            const uint32_t faceBits[4] = {kEnvInfNegX, kEnvInfPosX, kEnvInfNegZ, kEnvInfPosZ};
            const glm::vec2 edgeMid[4] = {c - ax, c + ax, c - az, c + az};
            const glm::vec2 edgeDir[4] = {-ax, ax, -az, az};
            for (int f = 0; f < 4; ++f) {
                if ((infMask & faceBits[f]) == 0)
                    continue;
                glm::vec2 d = edgeDir[f];
                const float l = glm::length(d);
                if (!(l > 1e-6f))
                    continue;
                d /= l;
                const ImVec2 m(WX(edgeMid[f].x), WZ(edgeMid[f].y));
                const ImVec2 tip(m.x + d.x * 30.0f, m.y + d.y * 30.0f);
                dl->AddLine(m, tip, col, 2.0f);
                dl->AddLine(
                    tip,
                    ImVec2(tip.x - d.x * 8.0f - d.y * 5.0f, tip.y - d.y * 8.0f + d.x * 5.0f),
                    col,
                    2.0f);
                dl->AddLine(
                    tip,
                    ImVec2(tip.x - d.x * 8.0f + d.y * 5.0f, tip.y - d.y * 8.0f - d.x * 5.0f),
                    col,
                    2.0f);
            }
            if (s > 0.8f) {
                char label[24];
                std::snprintf(label,
                              sizeof(label),
                              "%u%s",
                              node,
                              (infMask & (kEnvInfNegY | kEnvInfPosY)) != 0 ? " Yinf" : "");
                dl->AddText(ImVec2(WX(c.x) + 4.0f, WZ(c.y) - 8.0f), col, label);
            }
        }
    }
    // ---- GI volumes -----------------------------------------------
    if (m_worldMapGiVolumes && m_world) {
        const ::Scene& scene = m_world->scene;
        for (const GiVolumeRow& giRow : scene.tables.giVolumes) {
            const NodeId node = giRow.node;
            if (node >= scene.globalTransform.size())
                continue;
            ++nGiVolumes;
            const glm::mat4& g = scene.globalTransform[node];
            const glm::vec2 c(g[3].x, g[3].z);
            const glm::vec2 ax(g[0].x, g[0].z);
            const glm::vec2 az(g[2].x, g[2].z);
            dl->AddQuad(ImVec2(WX(c.x - ax.x - az.x), WZ(c.y - ax.y - az.y)),
                        ImVec2(WX(c.x + ax.x - az.x), WZ(c.y + ax.y - az.y)),
                        ImVec2(WX(c.x + ax.x + az.x), WZ(c.y + ax.y + az.y)),
                        ImVec2(WX(c.x - ax.x + az.x), WZ(c.y - ax.y + az.y)),
                        IM_COL32(60, 200, 255, 230),
                        1.5f);
            if (s > 0.8f) {
                char label[16];
                std::snprintf(label, sizeof(label), "%u", node);
                dl->AddText(
                    ImVec2(WX(c.x) + 4.0f, WZ(c.y) - 8.0f), IM_COL32(60, 200, 255, 230), label);
            }
        }
        // BAKED runtime volumes (the loaded set the renderer actually
        // composites). The authored quads above only exist while the
        // AUTHORING scene is live — a cooked pak world carries NO
        // authored nodes, so the layer used to draw nothing exactly
        // where the GI actually is. The lattice AABB footprint is the
        // hull's XZ extent (axis-aligned by construction).
        for (const GiVolumeResources& res : m_giVolumes) {
            const smvk::gi::GiVolume& vol = res.volume;
            if (vol.dims[0] == 0 || vol.dims[1] == 0 || vol.dims[2] == 0)
                continue;
            ++nGiVolumes;
            const glm::vec3 mn = vol.gridMin;
            const glm::vec3 mx =
                vol.gridMin +
                glm::vec3(float(vol.dims[0] - 1), float(vol.dims[1] - 1), float(vol.dims[2] - 1)) *
                    vol.spacing;
            dl->AddRect(ImVec2(WX(mn.x), WZ(mn.z)),
                        ImVec2(WX(mx.x), WZ(mx.z)),
                        IM_COL32(60, 200, 255, 140),
                        0.0f,
                        0,
                        1.0f);
            if (s > 0.8f && !vol.def.name.empty())
                dl->AddText(ImVec2(WX(mn.x) + 4.0f, WZ(mn.z) + 2.0f),
                            IM_COL32(60, 200, 255, 160),
                            vol.def.name.c_str());
        }
    }
    // ---- GI lattice dots (convex GI volumes) + cubemap rings -------
    uint32_t giOnScreen = 0, giMapMarkers = 0;
    if (m_worldMapProbes) {
        // The loaded CONVEX GI volumes' lattices — the runtime's
        // remaining baked GI (the legacy octree never loads: stage 1
        // of the retirement). Total 3D sites counted; a column's Y
        // stack folds into its bin (the density coloring carries the
        // multiplicity).
        // NOTE: these are lattice SITES — not proof every sample is
        // valid or inside the convex hull.
        //
        // BIN SIZE TRACKS THE LATTICE PITCH (the sponza_3 report):
        // the original fixed ~6px bin moired against a 0.5 m lattice
        // at fit zoom (~8.4 px pitch) — two regular grids beat against
        // each other and 279 of 2644 covered bins came up EMPTY, so
        // the layer read as "holes in a grid pattern" (regular gaps
        // that MOVED with zoom — the aliasing signature, not data: the
        // lattice itself has a column at every (lx, lz) site inside
        // the footprints; even inside-solid probes count). Now the
        // bin is the lattice pitch clamped to [6, 32] px: pixel-scale
        // when zoomed out (many columns per bin, the density colors
        // carry them), EXACT lattice-scale in between (one dot per
        // column), and past 32 px the honest gaps between discrete
        // probes show — you are looking between 0.5 m-spaced sites.
        float minSpacing = std::numeric_limits<float>::max();
        for (const GiVolumeResources& res : m_giVolumes) {
            const smvk::gi::GiVolume& vol = res.volume;
            if (vol.dims[0] == 0 || vol.dims[1] == 0 || vol.dims[2] == 0)
                continue;
            minSpacing = std::min(minSpacing, vol.spacing);
        }
        const float kBinMinPx = 6.0f, kBinMaxPx = 32.0f;
        const float binPx = minSpacing < std::numeric_limits<float>::max()
                                ? std::clamp(minSpacing * s, kBinMinPx, kBinMaxPx)
                                : kBinMinPx;
        const int cols = std::max(1, int(std::ceil(cw / binPx)));
        const int rows = std::max(1, int(std::ceil(ch / binPx)));
        std::vector<uint32_t> occupancy(size_t(cols) * size_t(rows), 0u);
        // Per-bin min 6-axis distance (the coplanarity check):
        // the worst-case across each XZ column's Y stack.
        std::vector<float> visMin(size_t(cols) * size_t(rows), 1e30f);
        for (const GiVolumeResources& res : m_giVolumes) {
            const smvk::gi::GiVolume& vol = res.volume;
            if (vol.dims[0] == 0 || vol.dims[1] == 0 || vol.dims[2] == 0)
                continue;
            nProbes += vol.dims[0] * vol.dims[1] * vol.dims[2];
            for (uint32_t lz = 0; lz < vol.dims[2]; ++lz)
                for (uint32_t lx = 0; lx < vol.dims[0]; ++lx) {
                    const float wx = vol.gridMin.x + float(lx) * vol.spacing;
                    const float wz = vol.gridMin.z + float(lz) * vol.spacing;
                    const float x = WX(wx) - o.x;
                    const float y = WZ(wz) - o.y;
                    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0f || x >= cw || y < 0.0f ||
                        y >= ch)
                        continue;
                    ++giOnScreen; // XZ columns in view
                    const int bx = std::min(cols - 1, int(x / binPx));
                    const int by = std::min(rows - 1, int(y / binPx));
                    occupancy[size_t(by) * size_t(cols) + size_t(bx)] += vol.dims[1];
                    // Per-column min 6-axis distance (the coplanarity
                    // check): the worst-case across the Y stack. The vis
                    // data is the CPU-side array (coeffs were freed, the
                    // vis stays). 0 = blocked, cap = open.
                    if (res.volume.vis.size() == size_t(res.volume.VoxelCount()) * 6) {
                        float colMin = 1e30f;
                        for (uint32_t iy = 0; iy < vol.dims[1]; ++iy) {
                            const size_t vIdx = (size_t(lz) * vol.dims[1] + iy) * vol.dims[0] + lx;
                            for (uint32_t k = 0; k < 6; ++k)
                                colMin = std::min(colMin, res.volume.vis[vIdx * 6 + k]);
                        }
                        if (colMin < 1e29f) {
                            const size_t bi = (size_t(by) * size_t(cols) + size_t(bx));
                            visMin[bi] = std::min(visMin[bi], colMin);
                        }
                    }
                }
        }
        for (int by = 0; by < rows; ++by) {
            for (int bx = 0; bx < cols; ++bx) {
                const uint32_t n = occupancy[size_t(by) * size_t(cols) + size_t(bx)];
                if (!n)
                    continue;
                ++giMapMarkers;
                const float density = std::min(1.0f, std::log2(1.0f + float(n)) / 6.0f);
                const ImVec2 pt(o.x + std::min((float(bx) + 0.5f) * binPx, cw - 0.5f),
                                o.y + std::min((float(by) + 0.5f) * binPx, ch - 0.5f));
                // Color by the per-bin probe status (the coplanarity
                // check): red = any probe blocked/inside geometry (vis
                // < 0.05 m), yellow = near-surface (< 1.0 m), green =
                // open (>= 1.0 m). The density carries the radius.
                const float vm = visMin[size_t(by) * size_t(cols) + size_t(bx)];
                ImU32 dotCol;
                if (vm < 0.05f)
                    dotCol = IM_COL32(255, 60, 60, 220);
                else if (vm < 1.0f)
                    dotCol = IM_COL32(255, 220, 60, 220);
                else
                    dotCol = IM_COL32(90, 230, 90, 220);
                // Radius rides the bin so one-dot-per-column reads as
                // the lattice's own pitch (not a sub-bin speck).
                dl->AddCircleFilled(pt, std::min(0.34f * binPx, 3.0f) + 1.2f * density, dotCol);
            }
        }
        // One-shot headless evidence (the capture is scene-only — the
        // grading reference never shows ImGui layers, so this log is
        // the proof the lattice layer drew with live data): the
        // convex-volume lattice sites + the drawn XZ bins.
        static bool s_loggedGiLattice = false;
        if (!s_loggedGiLattice) {
            s_loggedGiLattice = true;
            std::fprintf(stderr,
                         "[WorldMap] GI lattice layer live: %u 3D sites across "
                         "%u convex volume(s), %u XZ cols in view, %u dots\n",
                         nProbes,
                         uint32_t(m_giVolumes.size()),
                         giOnScreen,
                         giMapMarkers);
        }
        for (const auto& pr : m_cubemapProbes) {
            const glm::vec3 pp(pr.posInfluence.x, pr.posInfluence.y, pr.posInfluence.z);
            ++nProbes;
            dl->AddCircleFilled(ImVec2(WX(pp.x), WZ(pp.z)), 3.0f, IM_COL32(240, 220, 80, 230));
            const float r = pr.posInfluence.w * s;
            if (r > 4.0f && r < 4000.0f)
                dl->AddCircle(ImVec2(WX(pp.x), WZ(pp.z)), r, IM_COL32(240, 220, 80, 120));
        }
    }
    // ---- Lights (diamonds = spots) + decals (squares) -------------
    if (m_worldMapLights) {
        uint32_t drawn = 0;
        for (const auto& l : m_debugLights) {
            if (++drawn > 512)
                break;
            ++nLights;
            const ImU32 col =
                IM_COL32(toSrgb8(l.color.r), toSrgb8(l.color.g), toSrgb8(l.color.b), 220);
            const ImVec2 pt(WX(l.position.x), WZ(l.position.z));
            if (l.type == 1) // spot
                dl->AddQuadFilled(ImVec2(pt.x, pt.y - 5.0f),
                                  ImVec2(pt.x + 5.0f, pt.y),
                                  ImVec2(pt.x, pt.y + 5.0f),
                                  ImVec2(pt.x - 5.0f, pt.y),
                                  col);
            else
                dl->AddCircleFilled(pt, 4.0f, col);
        }
        if (m_world) {
            uint32_t dd = 0;
            for (const DecalRow& decalRow : m_world->scene.tables.decals) {
                const NodeId node = decalRow.node;
                if (++dd > 256)
                    break;
                if (node >= m_world->scene.globalTransform.size())
                    continue;
                ++nDecals;
                const glm::vec3 p(m_world->scene.globalTransform[node][3]);
                dl->AddRectFilled(ImVec2(WX(p.x) - 3.0f, WZ(p.z) - 3.0f),
                                  ImVec2(WX(p.x) + 3.0f, WZ(p.z) + 3.0f),
                                  IM_COL32(230, 80, 220, 220));
            }
        }
    }
    // ---- Camera wedge ---------------------------------------------
    if (m_worldMapCamera) {
        const glm::vec3 cp = m_cameraPositioner.getPosition();
        glm::vec3 fw = m_cameraPositioner.getForward();
        glm::vec2 f2(fw.x, fw.z);
        if (!(glm::length(f2) > 1e-6f))
            f2 = glm::vec2(0.0f, -1.0f);
        f2 = glm::normalize(f2);
        const ImVec2 pp(WX(cp.x), WZ(cp.z));
        const float len = 60.0f, spread = 0.35f;
        const float cs = std::cos(spread), sn = std::sin(spread);
        const glm::vec2 d1(f2.x * cs - f2.y * sn, f2.x * sn + f2.y * cs);
        const glm::vec2 d2(f2.x * cs + f2.y * sn, -f2.x * sn + f2.y * cs);
        dl->AddLine(
            pp, ImVec2(pp.x + d1.x * len, pp.y + d1.y * len), IM_COL32(255, 255, 255, 230), 2.0f);
        dl->AddLine(
            pp, ImVec2(pp.x + d2.x * len, pp.y + d2.y * len), IM_COL32(255, 255, 255, 230), 2.0f);
        dl->AddCircleFilled(pp, 4.0f, IM_COL32(255, 255, 255, 255));
    }
    dl->PopClipRect();
    ImGui::TextDisabled("zones %u (%u off)  gi volumes %u  lights %u  decals %u",
                        nZones,
                        nZonesOff,
                        nGiVolumes,
                        nLights,
                        nDecals);
    ImGui::TextDisabled("GI lattice %u sites (convex %u + regular %u loaded)  "
                        "XZ cols in view %u  map dots %u  1px = %.1fm  drag / wheel",
                        nProbes,
                        uint32_t(m_giVolumes.size()),
                        uint32_t(m_regVolumes.size()),
                        giOnScreen,
                        giMapMarkers,
                        1.0f / s);
    // Streaming footer (the WorldFields + partition line): residents,
    // in-flight, pool pressure, atlas bytes, the per-frame churn. Dark
    // when the system is off.
    {
        const worldfields::WorldFieldStats st = m_worldFields.Stats();
        const worldfields::WorldFieldGpuBinding bind = m_worldFields.GpuBinding();
        char line[192];
        if (bind.Valid()) {
            std::snprintf(line,
                          sizeof(line),
                          "fields: %u live  %u bricks (win %u / inflight %u)"
                          "  pool %u free  %.1f MiB atlas  uploads %u/frame"
                          "  evict %u  skipped %u",
                          bind.fieldCount,
                          st.residentBricks,
                          nFieldWindows,
                          st.inFlightBricks,
                          st.freeSlots,
                          st.atlasBytes / (1024.0 * 1024.0),
                          st.pendingUploads,
                          st.evictions,
                          st.requestsSkipped);
        } else {
            std::snprintf(line, sizeof(line), "fields: off (TOURMALINE_WORLD_FIELDS=1)");
        }
        ImGui::TextDisabled("%s", line);
    }
    ImGui::End();
}

// App-owned ImGui widgets, called through WorldRenderer::SetSettingsUi.
void WorldRenderer::DrawAppDebugUi(const DebugUiFrame& frame) {
    if (!frame.fb || !frame.env)
        return;
    DrawAppSettingsUi(frame);

    // Text-only stats HUD (top-left overlay, Ctrl+T): drawn after
    // the panel; the panel is a fixed Right side bar, so the two
    // never overlap.
    DrawHudOverlay();

    // Debug-view hint (top-center, auto): the active view's name +
    // its color coding while a debug visualization is on.
    DrawDebugViewHint();

    // Frame-time histogram (F12): SEPARATE UI from the debug panel -
    // a full-width bottom strip (see profiler::DrawFrameHistogramUi).
    // Zero cost while disabled: one flag check.
    if (smvk::profiler::FrameHistogramEnabled())
        smvk::profiler::DrawFrameHistogramUi();
    // Texture inspector (aux-texture debug view): SEPARATE floating
    // window, never part of the debug panel - it only opens on the
    // backtick shortcut (keybind::TEXTURE_VIEWER_TOGGLE). Closing (X
    // or backtick) drops the preview binding + selection.
    // ---- Texture inspector (aux-texture debug view) --------
    // Lists the LIVE textures (one row per pool texture; the
    // metadata: debug name, extent, format, mips/layers, the
    // bindless slot - NO content filter: every texture is one
    // click away) and previews a clicked one through the
    // ImGui Vulkan backend's user-texture bindings. Plain 2D
    // single-layer textures preview directly; CUBEMAPS preview
    // through six per-face View2D bindings (+X -X +Y -Y +Z -Z,
    // 3x2 grid - the ImGui backend samples Texture2D only, so
    // the cube's own default view cannot serve as the preview);
    // cube arrays preview cube 0. Other arrays/3D list only.
    // Staleness: a selected handle dropping out of the listing
    // (destroyed, generation bumped - UpdateTexture's recreate
    // path) drops the preview + face views.
    // Selection is shared by CLICK and ARROW-KEY NAV: ImGui moves
    // the keyboard-nav cursor onto rows with the arrow keys and
    // the focused row previews IMMEDIATELY (no Enter - the nav
    // branch below treats focus as selection). Click still works;
    // Enter still activates (both funnel into the same select).
    // TMP-DIAG (TOURMALINE_TEXVIEW_REPRO=<frame>): the deterministic
    // headless replay of the texture-viewer X-close device-lost crash —
    // open at N, select the first previewable live texture at N+2, then
    // at N+8 drop the selection RIGHT AFTER the body rendered the
    // preview (the exact X-close sequence: the close-click frame's
    // body still renders once — Begin flips the flag mid-frame — so
    // the frame's own draw data still binds the freed descriptor set
    // when the ImGui pass renders it). Pre-fix this faulted the device
    // (vkWaitForFences -4, tourmaline_fatal.log); post-fix (the
    // deferred RetireUserTexture) the run must survive to the bench
    // exit. The drop stage lives at the X-close check below (after the
    // body). Remove with the investigation.
    {
        static const int reproFrame = [] {
            const char* e = std::getenv("TOURMALINE_TEXVIEW_REPRO");
            return e ? std::atoi(e) : -1;
        }();
        static int reproStage = 0;
        if (reproFrame >= 0) {
            if (reproStage == 0 && (int)m_frameCounter >= reproFrame) {
                m_texDebugWindow = true;
                reproStage = 1;
            } else if (reproStage == 1 && (int)m_frameCounter >= reproFrame + 2) {
                for (const smvk::TextureInfo& t : m_context->ListTextureInfos()) {
                    const bool plain2d =
                        t.type == smvk::ImageType::Image2D && !t.isCubemap && t.arrayLayers == 1;
                    const bool cube = t.isCubemap && smvk::IsCubemapLayerCount(t.arrayLayers);
                    if (plain2d || cube) {
                        SelectTexDebugTexture(t);
                        break;
                    }
                }
                reproStage = 2;
            } else if (reproStage == 2 && (int)m_frameCounter >= reproFrame + 8) {
                // The X-close drop, AFTER this frame's body rendered the
                // preview (see above): the stage-3 call lives at the
                // close-check site below.
            }
        }
    }
    if (m_texDebugWindow) {
        ImGui::SetNextWindowSize(ImVec2(560.0f, 760.0f), ImGuiCond_Appearing);
        ImGui::Begin("Texture Viewer", &m_texDebugWindow);
        // Preview sampler: clamp + linear, created once.
        if (m_texDebugSampler == UINT32_MAX) {
            smvk::SamplerCreateInfo sci{};
            sci.magFilter = smvk::Filter::Linear;
            sci.minFilter = smvk::Filter::Linear;
            sci.mipmapMode = smvk::MipmapMode::Nearest;
            sci.addressU = smvk::AddressMode::ClampToEdge;
            sci.addressV = smvk::AddressMode::ClampToEdge;
            sci.addressW = smvk::AddressMode::ClampToEdge;
            m_texDebugSampler = m_context->GetOrCreateSampler(sci);
        }
        std::vector<smvk::TextureInfo> texs = m_context->ListTextureInfos();
        // Common-format label (debug shorthand; see the format
        // legend at the bottom of this window). BC compressed
        // formats included: pak CONTENT textures are BC (the
        // cook transcodes), so the content rows would otherwise
        // all read "(other)".
        auto fmtName = [](smvk::Format f) -> const char* {
            switch (f) {
            case smvk::Format::BC1_RGB_UNorm:
                return "BC1_RGB";
            case smvk::Format::BC1_RGB_SRGB:
                return "BC1_RGB_sRGB";
            case smvk::Format::BC1_RGBA_UNorm:
                return "BC1_RGBA";
            case smvk::Format::BC1_RGBA_SRGB:
                return "BC1_RGBA_sRGB";
            case smvk::Format::BC2_UNorm:
                return "BC2";
            case smvk::Format::BC2_SRGB:
                return "BC2_sRGB";
            case smvk::Format::BC3_UNorm:
                return "BC3";
            case smvk::Format::BC3_SRGB:
                return "BC3_sRGB";
            case smvk::Format::BC4_UNorm:
                return "BC4_UN";
            case smvk::Format::BC4_SNorm:
                return "BC4_SN";
            case smvk::Format::BC5_UNorm:
                return "BC5_UN";
            case smvk::Format::BC5_SNorm:
                return "BC5_SN";
            case smvk::Format::BC6H_UFloat:
                return "BC6H_UF";
            case smvk::Format::BC6H_SFloat:
                return "BC6H_SF";
            case smvk::Format::BC7_UNorm:
                return "BC7";
            case smvk::Format::BC7_SRGB:
                return "BC7_sRGB";
            case smvk::Format::RGBA8_UNorm:
                return "RGBA8_UN";
            case smvk::Format::RGBA8_SRGB:
                return "RGBA8_SRGB";
            case smvk::Format::RGBA16_Float:
                return "RGBA16F";
            case smvk::Format::RGBA32_Float:
                return "RGBA32F";
            case smvk::Format::R16_Float:
                return "R16F";
            case smvk::Format::R32_Float:
                return "R32F";
            case smvk::Format::RG16_Float:
                return "RG16F";
            case smvk::Format::D32_Float:
                return "D32F";
            default:
                return "(other)";
            }
        };
        // Selection staleness: the handle must still be live.
        bool selAlive = m_texDebugSelected.empty();
        ImGui::BeginChild("##texlist", ImVec2(0.0f, 220.0f), true);
        for (const smvk::TextureInfo& t : texs) {
            if (!m_texDebugSelected.empty() && m_texDebugSelected == t.handle)
                selAlive = true;
            const bool plain2d =
                t.type == smvk::ImageType::Image2D && !t.isCubemap && t.arrayLayers == 1;
            // Cubemap (or 6*N cube array) preview: per-face 2D
            // views. Cube arrays preview cube 0 (faces 0..5).
            const bool cube = t.isCubemap && smvk::IsCubemapLayerCount(t.arrayLayers);
            const bool previewable = plain2d || cube;
            char label[320];
            std::snprintf(label,
                          sizeof(label),
                          "%s%s %ux%u %s mips=%u layers=%u bindless=%d",
                          t.name.empty() ? "(unnamed)" : t.name.data(),
                          cube ? (t.arrayLayers > 6 ? " [cube array: cube 0 preview]"
                                                    : " [cube 6-face preview]")
                               : (previewable ? "" : " [no 2D preview]"),
                          t.extent.width,
                          t.extent.height,
                          fmtName(t.format),
                          t.mipLevels,
                          t.arrayLayers,
                          t.bindless);
            if (ImGui::Selectable(label, m_texDebugSelected == t.handle)) {
                // Click (or Enter/gamepad activate): preview
                // immediately. See SelectTexDebugTexture for the
                // shared selection body.
                SelectTexDebugTexture(t);
            } else if (ImGui::IsItemFocused() && m_texDebugSelected != t.handle && previewable) {
                // ARROW-KEY NAV: ImGui moved the keyboard-nav
                // cursor onto this row - treat focus as
                // selection so the preview displays immediately
                // (no Enter). The != guard makes this a no-op on
                // the already-selected row (focus persists on it
                // every frame); previewable keeps 3D/arrays
                // non-selectable by nav.
                SelectTexDebugTexture(t);
            }
        }
        ImGui::EndChild();
        if (!m_texDebugSelected.empty() && !selAlive) {
            // Died under the selection (generation bumped):
            // drop the preview + face views, never sample a
            // dead view.
            DropTexDebugSelection();
            ImGui::TextDisabled("selected texture was destroyed "
                                "(generation bumped) - selection cleared");
        }
        if (m_texDebugSelected.empty()) {
            ImGui::TextDisabled("%zu live textures - click one to inspect.", texs.size());
        } else {
            // Preview: fit the window width, keep the aspect.
            // Raw sample - HDR formats read blown/crushed
            // (inspect, not grade).
            const smvk::TextureInfo* sel = nullptr;
            for (const smvk::TextureInfo& t : texs)
                if (t.handle == m_texDebugSelected) {
                    sel = &t;
                    break;
                }
            if (sel) {
                // The texture NAME, prominent (the list rows
                // truncate long load paths; the preview states
                // what is actually on screen). Arena-owned copy
                // (NUL-terminated), so the (begin, end) form needs
                // no NUL reliance at all.
                ImGui::TextUnformatted(sel->name.empty() ? "(unnamed)" : sel->name.data(),
                                       sel->name.empty() ? "(unnamed)"
                                                         : sel->name.data() + sel->name.size());
                if (sel->isCubemap) {
                    // Cubemap: the 3x2 per-face grid (+X -X +Y -Y
                    // +Z -Z, row-major). One image per face - a
                    // face whose view died (its texture streamed
                    // a recreation) shows a blank cell (binding 0
                    // renders the backend's null texture).
                    static const char* const kFaceNames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
                    const float gap = 6.0f;
                    const float cell = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
                    for (uint32_t face = 0; face < 6; ++face) {
                        if (face > 0 && (face % 3) != 0)
                            ImGui::SameLine(0.0f, gap);
                        ImGui::BeginGroup();
                        if (m_texDebugCubePreview[face] != 0) {
                            ImGui::Image(m_texDebugCubePreview[face], ImVec2(cell, cell));
                        } else {
                            // Dead binding (the view died after the
                            // Add - never hand a null descriptor to
                            // the backend): placeholder keeps the grid
                            // aligned.
                            ImGui::Dummy(ImVec2(cell, cell));
                            ImGui::TextDisabled("(view died)");
                        }
                        ImGui::TextDisabled("%s", kFaceNames[face]);
                        ImGui::EndGroup();
                    }
                    ImGui::TextDisabled("%ux%u cube (raw sample - HDR "
                                        "formats read unexposed; cube arrays show "
                                        "cube 0)",
                                        sel->extent.width,
                                        sel->extent.height);
                    // Probe row: world position + partition info.
                    // Matched by the library entry's TEXTURE HANDLE
                    // (exact - the bindless slot could alias after
                    // churn); entryName is the library key, so at
                    // most one row matches. (The Holder converts
                    // to Handle - extract BEFORE comparing: the
                    // implicit conversion on the left side of the
                    // member operator trips MSVC's rewritten-
                    // candidate rules.)
                    for (const CubemapRow& row : m_cubemapRows) {
                        const auto* entry = m_cubemaps.Find(row.entryName);
                        if (!entry || entry->texture.empty())
                            continue;
                        const smvk::TextureHandle entryTex = entry->texture;
                        if (entryTex != sel->handle)
                            continue;
                        ImGui::Text("probe %s @ (%.1f, %.1f, %.1f)"
                                    "  influence %.1fm  fade %.1fm  %s",
                                    row.entryName.c_str(),
                                    row.position.x,
                                    row.position.y,
                                    row.position.z,
                                    row.influenceRadius,
                                    row.fadeRadius,
                                    row.pinned ? "pinned (always resident)"
                                               : "streamed (distance residency)");
                        break;
                    }
                    // No row matched: the global/skybox capture
                    // (no manifest row) - nothing to place.
                } else if (m_texDebugPreview != 0) {
                    const float w = ImGui::GetContentRegionAvail().x;
                    const float aspect = float(std::max(sel->extent.height, 1u)) /
                                         float(std::max(sel->extent.width, 1u));
                    ImGui::Image(m_texDebugPreview, ImVec2(w, w * aspect));
                    ImGui::TextDisabled("%ux%u (raw sample - HDR "
                                        "formats read unexposed)",
                                        sel->extent.width,
                                        sel->extent.height);
                }
                // Material use (2D only - cubemaps serve IBL, not
                // material slots). Materials store the texture POOL
                // index (TextureHandle::index() - NOT the bindless
                // slot; the two diverge once textures churn, see
                // GetTextureBindlessIndex), so match on that.
                if (!sel->isCubemap && m_world) {
                    const int poolIdx = static_cast<int>(sel->handle.index());
                    uint32_t useBase = 0, useMetRough = 0, useNormal = 0, useOcc = 0, useEmis = 0,
                             useMats = 0;
                    const size_t slotCount = m_world->materialLocations.size();
                    for (size_t i = 0; i < slotCount; ++i) {
                        const smvk::Material* mat = m_world->GetMaterial((uint32_t)i);
                        if (!mat)
                            continue;
                        bool uses = false;
                        if (mat->values.textures.x == poolIdx) {
                            ++useBase;
                            uses = true;
                        }
                        if (mat->values.textures.y == poolIdx) {
                            ++useMetRough;
                            uses = true;
                        }
                        if (mat->values.textures.z == poolIdx) {
                            ++useNormal;
                            uses = true;
                        }
                        if (mat->values.textures.w == poolIdx) {
                            ++useOcc;
                            uses = true;
                        }
                        if (mat->values.textures2.x == poolIdx) {
                            ++useEmis;
                            uses = true;
                        }
                        if (uses)
                            ++useMats;
                    }
                    if (useMats == 0) {
                        ImGui::TextDisabled("no material uses this texture (aux)");
                    } else {
                        // Channel names as users author them:
                        // diffuse (baseColor), metallicRoughness
                        // (G=roughness B=metallic), normal,
                        // occlusion, emissive.
                        char usage[256];
                        int off = 0;
                        const auto append = [&](const char* chan, uint32_t n) {
                            if (!n || off >= (int)sizeof(usage) - 1)
                                return;
                            off += std::snprintf(usage + off,
                                                 sizeof(usage) - off,
                                                 "%s%s x%u",
                                                 off ? ", " : "",
                                                 chan,
                                                 n);
                        };
                        append("diffuse", useBase);
                        append("metallicRoughness", useMetRough);
                        append("normal", useNormal);
                        append("occlusion", useOcc);
                        append("emissive", useEmis);
                        ImGui::Text("material use (%u material%s): %s",
                                    useMats,
                                    useMats == 1 ? "" : "s",
                                    usage);
                    }
                }
            }
        }
        // Format legend: the row labels above are debug shorthand.
        if (ImGui::CollapsingHeader("Format legend")) {
            ImGui::TextDisabled("RGBA8_UN    8-bit/channel linear LDR - vertex-color\n"
                                "            charts, engine scratch\n"
                                "RGBA8_SRGB  8-bit sRGB-encoded color - imported\n"
                                "            albedo/diffuse, decal tints\n"
                                "RGBA16F     16-bit float HDR - GI volumes, cubemaps,\n"
                                "            bloom, SSR targets\n"
                                "RGBA32F     32-bit float HDR - light/debug buffers\n"
                                "R16F/RG16F/R32F  1-2 channel float - depth meters,\n"
                                "            velocity, SH taps, exposure\n"
                                "D32F        depth attachment (32-bit float)\n"
                                "BC1/BC3/BC5/BC7  block-compressed pak CONTENT\n"
                                "            (cook transcodes): albedo / alpha-smooth\n"
                                "            / normal / roughness-metallic packed\n"
                                "BC4/BC6H    1-channel UN/SN + HDR float blocks\n"
                                "            (rare in this content)\n"
                                "(other)     everything else (numeric formats,\n"
                                "            packed 101010/111110, ...)\n"
                                "_sRGB suffix = sRGB variant of the same layout");
        }
        ImGui::End();
    }
    // Closed via the title-bar X: the flag flipped above, drop the
    // binding + selection so nothing samples a dead view.
    if (!m_texDebugWindow && (m_texDebugPreview != 0 || !m_texDebugSelected.empty()))
        DropTexDebugSelection();
    // TMP-DIAG (TOURMALINE_TEXVIEW_REPRO): the replay's drop stage —
    // the exact X-close position: AFTER this frame's body rendered the
    // preview, BEFORE the ImGui pass renders the frame's draw data (the
    // window stays open in the replay; only the drop matters — the
    // freed set + the frame's own referencing draw data). Remove with
    // the investigation.
    {
        static const int reproFrame = [] {
            const char* e = std::getenv("TOURMALINE_TEXVIEW_REPRO");
            return e ? std::atoi(e) : -1;
        }();
        static int reproStage = 0;
        if (reproFrame >= 0 && reproStage == 2 && (int)m_frameCounter >= reproFrame + 8) {
            DropTexDebugSelection();
            reproStage = 3;
        }
    }

    // ---- VRAM usage viewer (Ctrl+M) --------------------------
    // Floating tool window like the texture inspector (never part
    // of the debug panel): one row per GPU memory HEAP - the
    // device-reported size, the VMA budget (VK_EXT_memory_budget:
    // what the OS says is usable), the USAGE (every Vulkan
    // allocation on the heap) + a bar, and the VMA block stats
    // (allocationBytes = committed inside blocks; blockBytes =
    // the blocks themselves, so block - allocation = space wasted
    // inside partially-used blocks). Totals sum DEVICE_LOCAL
    // heaps ONLY (the HUD's rule: summing every heap
    // double-counts host budgets - a 16 GiB card would read ~30
    // GiB "total"; integrated/uniform-memory GPUs fall back to
    // summing all when nothing reports deviceLocal). Data
    // refreshes at 1 Hz (RefreshGpuMemoryStats throttles inside)
    // and only while the window is open - zero cost closed.
    if (m_vramWindow) {
        ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_Appearing);
        ImGui::Begin("VRAM Usage", &m_vramWindow);
        RefreshGpuMemoryStats(false);
        if (m_gpuMemHeaps.empty()) {
            ImGui::TextDisabled("GPU memory info unavailable\n"
                                "(needs a device + an initialized VMA allocator)");
        } else {
            auto mib = [](uint64_t b) { return double(b) / 1048576.0; };
            // Per-class accumulators (device-local vs host; the
            // totals block below reads them after the table).
            uint64_t vramUsed = 0, vramBudget = 0, vramSize = 0;
            uint64_t hostUsed = 0, hostBudget = 0, hostSize = 0;
            if (ImGui::BeginTable(
                    "vram_heaps", 7, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Heap");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("Budget");
                ImGui::TableSetupColumn("Usage");
                ImGui::TableSetupColumn("Committed");
                ImGui::TableSetupColumn("Blocks");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < m_gpuMemHeaps.size(); ++i) {
                    const smvk::GpuMemoryHeap& h = m_gpuMemHeaps[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(std::to_string(i).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(h.deviceLocal ? "VRAM (device)" : "HOST");
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f MiB", mib(h.heapSizeBytes));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f MiB", mib(h.budgetBytes));
                    ImGui::TableNextColumn();
                    {
                        // Row usage bar (budget-relative; the
                        // bar text carries the exact MiB + %).
                        const float frac = h.budgetBytes > 0
                                               ? float(double(h.usageBytes) / double(h.budgetBytes))
                                               : 0.0f;
                        char overlay[48];
                        std::snprintf(overlay,
                                      sizeof(overlay),
                                      "%.0f MiB (%.0f%%)",
                                      mib(h.usageBytes),
                                      100.0 * frac);
                        ImGui::ProgressBar(
                            frac, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), overlay);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f MiB", mib(h.allocationBytes));
                    ImGui::TableNextColumn();
                    // Blocks + the wasted-inside-blocks delta
                    // (block - committed): the pool's internal
                    // free space (large values = fragmentation /
                    // over-allocated pools).
                    ImGui::Text("%.0f MiB (+%.0f)",
                                mib(h.blockBytes),
                                mib(h.blockBytes > h.allocationBytes
                                        ? h.blockBytes - h.allocationBytes
                                        : 0));
                    if (h.deviceLocal) {
                        vramUsed += h.usageBytes;
                        vramBudget += h.budgetBytes;
                        vramSize += h.heapSizeBytes;
                    } else {
                        hostUsed += h.usageBytes;
                        hostBudget += h.budgetBytes;
                        hostSize += h.heapSizeBytes;
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            // Totals: the HUD's device-local-only rule (the
            // fallback to sum-all mirrors it exactly, so the two
            // displays can never disagree).
            {
                bool anyDeviceLocal = false;
                for (const auto& h : m_gpuMemHeaps)
                    anyDeviceLocal = anyDeviceLocal || h.deviceLocal;
                uint64_t used = anyDeviceLocal ? vramUsed : vramUsed + hostUsed;
                uint64_t budget = anyDeviceLocal ? vramBudget : vramBudget + hostBudget;
                uint64_t size = anyDeviceLocal ? vramSize : vramSize + hostSize;
                const float frac = budget > 0 ? float(double(used) / double(budget)) : 0.0f;
                char overlay[48];
                std::snprintf(overlay,
                              sizeof(overlay),
                              "%.0f / %.0f MiB (%.0f%%)",
                              (double(used)) / 1048576.0,
                              (double(budget)) / 1048576.0,
                              100.0 * frac);
                ImGui::TextUnformatted(anyDeviceLocal ? "VRAM (device-local heaps)" : "GPU memory");
                ImGui::ProgressBar(frac, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), overlay);
                if (anyDeviceLocal && hostBudget > 0) {
                    ImGui::TextDisabled("HOST heaps (not VRAM): %.0f / %.0f MiB",
                                        (double(hostUsed)) / 1048576.0,
                                        (double(hostBudget)) / 1048576.0);
                }
                // ---- Breakdown: EXACT ledger + estimates ----------
                // IMMEDIATE MODE: everything below recomputes from
                // the live state every frame the window is open
                // (zero cost closed). Two data sources:
                //   EXACT - the RMI allocation ledger (see the
                //            smvk.h block): VmaAllocationInfo::size
                //            captured at every create, stored on
                //            the pool entries; per-category sums
                //            + per-item rows via immediate pool
                //            walks.
                //   ESTIMATE (cross-check) - the mip-chain sums by
                //            format bpp + the static-heap residency
                //            formula (36 B/vert + 4 B/index): keeps
                //            the ledger honest (pool slack, BC
                //            tail rounding) and covers the heap-
                //            placed/untracked entries the ledger
                //            skips by design.
                ImGui::Separator();
                ImGui::TextUnformatted("Breakdown (exact ledger, estimates as cross-check)");
                {
                    // ---- shared texture pass (joins below) ----
                    // One walk: the estimate (cross-check) + the
                    // handle join table for the cluster
                    // attribution + the top-texture list.
                    std::vector<smvk::TextureInfo> texs = m_context->ListTextureInfos();
                    std::unordered_map<uint32_t, const smvk::TextureInfo*> texByIdx;
                    // Per-texel bytes by format (the texture
                    // viewer's format set + sane defaults).
                    auto formatBpp = [](smvk::Format f) -> double {
                        switch (f) {
                        case smvk::Format::BC1_RGB_UNorm:
                        case smvk::Format::BC1_RGB_SRGB:
                        case smvk::Format::BC1_RGBA_UNorm:
                        case smvk::Format::BC1_RGBA_SRGB:
                        case smvk::Format::BC4_UNorm:
                        case smvk::Format::BC4_SNorm:
                            return 0.5; // 8 B / 4x4 block
                        case smvk::Format::BC2_UNorm:
                        case smvk::Format::BC2_SRGB:
                        case smvk::Format::BC3_UNorm:
                        case smvk::Format::BC3_SRGB:
                        case smvk::Format::BC5_UNorm:
                        case smvk::Format::BC5_SNorm:
                        case smvk::Format::BC6H_UFloat:
                        case smvk::Format::BC6H_SFloat:
                        case smvk::Format::BC7_UNorm:
                        case smvk::Format::BC7_SRGB:
                            return 1.0; // 16 B / 4x4 block
                        case smvk::Format::RGBA16_Float:
                            return 8.0;
                        case smvk::Format::RGBA32_Float:
                            return 16.0;
                        case smvk::Format::R16_Float:
                            return 2.0;
                        case smvk::Format::RG16_Float:
                            return 4.0;
                        case smvk::Format::R32_Float:
                            return 4.0;
                        case smvk::Format::D32_Float:
                            return 4.0;
                        default:
                            return 4.0; // RGBA8-family + unknown
                        }
                    };
                    // Mip-chain estimate (arrayLayers counts cube
                    // FACES; BC per-texel figures fold the 4x4
                    // blocks).
                    uint64_t texEstBytes = 0;
                    uint64_t texLargest = 0;
                    for (const smvk::TextureInfo& t : texs) {
                        texByIdx[t.handle.index()] = &t;
                        uint64_t bytes = 0;
                        for (uint32_t m = 0; m < t.mipLevels; ++m) {
                            const uint32_t w = std::max(t.extent.width >> m, 1u);
                            const uint32_t h = std::max(t.extent.height >> m, 1u);
                            const uint32_t d = std::max(t.extent.depth >> m, 1u);
                            bytes += uint64_t(double(w * h * d) * formatBpp(t.format));
                        }
                        bytes *= t.arrayLayers;
                        texEstBytes += bytes;
                        texLargest = std::max(texLargest, bytes);
                    }
                    // ---- EXACT ledger (per-category sums) ----
                    const auto ledger = m_context->GetMemoryLedger();
                    const auto allocs = m_context->GetMemoryAllocInfos();
                    uint64_t ledgerTotal = 0;
                    for (uint32_t c = 0; c < smvk::kMemoryCategoryCount; ++c)
                        ledgerTotal += ledger[c];
                    // ---- ownership walk (clusters own meshes +
                    // textures) --------------------------------
                    // Geometry: the residency ESTIMATE per unique
                    // mesh (36 B/vert + 4 B/index — the packed
                    // SoA heap layout; instances share pool
                    // entries). Textures: EXACT ledger bytes via
                    // the cluster's registered handles (fallback
                    // to the estimate for untracked backings).
                    // Per-cluster item lists feed the drill-down.
                    struct Owner {
                        uint64_t geo = 0;
                        uint64_t tex = 0;
                        size_t meshes = 0;
                        std::string label;
                        uint32_t color = 0;
                        std::vector<std::pair<uint64_t, const smvk::TextureInfo*>> texList;
                        std::vector<std::pair<uint64_t, std::string>> meshList;
                    };
                    std::vector<Owner> owners;
                    uint64_t geoBytes = 0;
                    size_t meshCount = 0;
                    std::unordered_map<const void*, size_t> clusterIdx;
                    std::vector<const smvk::Cluster*> clusterPtrs;
                    if (m_world) {
                        std::set<std::pair<const void*, uint32_t>> seenMesh;
                        std::set<uint32_t> seenTex;
                        auto uriFilePart = [](std::string uri) {
                            const size_t slash = uri.rfind('/');
                            if (slash != std::string::npos)
                                uri = uri.substr(slash + 1);
                            const size_t hashMark = uri.find('#');
                            if (hashMark != std::string::npos)
                                uri = uri.substr(0, hashMark);
                            return uri;
                        };
                        for (const auto& loc : m_world->meshLocations) {
                            if (!loc.cluster || !loc.handle.valid())
                                continue;
                            size_t ci;
                            const auto cit = clusterIdx.find(loc.cluster);
                            if (cit == clusterIdx.end()) {
                                ci = owners.size();
                                clusterIdx[loc.cluster] = ci;
                                clusterPtrs.push_back(loc.cluster);
                                Owner o{};
                                std::string file =
                                    uriFilePart(loc.cluster->GetMeshUri(loc.handle).Str());
                                o.label = file.empty() ? "cluster " + std::to_string(ci) : file;
                                // Random-but-STABLE color:
                                // pointer hash -> golden-ratio
                                // hue (integer degrees, no float
                                // drift), fixed s/v.
                                const uintptr_t ptr = reinterpret_cast<uintptr_t>(loc.cluster);
                                const uint64_t mixed =
                                    (uint64_t(ptr >> 4)) * 11400714819323198485ull;
                                const float hue = float((mixed >> 32) % 360ull) / 360.0f;
                                float r, g, b;
                                ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.85f, r, g, b);
                                o.color = IM_COL32(int(r * 255.0f + 0.5f),
                                                   int(g * 255.0f + 0.5f),
                                                   int(b * 255.0f + 0.5f),
                                                   255);
                                owners.push_back(std::move(o));
                            } else
                                ci = cit->second;
                            // Geometry: unique meshes only.
                            if (!seenMesh.insert({loc.cluster, loc.handle.index()}).second)
                                continue;
                            const smvk::StaticMesh* m = loc.cluster->staticMeshes.get(loc.handle);
                            if (!m)
                                continue;
                            ++meshCount;
                            const uint64_t bytes =
                                36ull * m->geometry.vertexCount + 4ull * m->geometry.indexCount;
                            owners[ci].geo += bytes;
                            owners[ci].meshes += 1;
                            geoBytes += bytes;
                            owners[ci].meshList.emplace_back(
                                bytes, uriFilePart(loc.cluster->GetMeshUri(loc.handle).Str()));
                        }
                        // Texture attribution per cluster (the
                        // registered URIs -> live texture handles;
                        // shared images attribute to the FIRST).
                        for (size_t ci = 0; ci < clusterPtrs.size(); ++ci) {
                            for (const auto& th : clusterPtrs[ci]->ListTextureUris()) {
                                const smvk::TextureHandle& handle = th.second;
                                if (!handle.valid())
                                    continue;
                                const auto it = texByIdx.find(handle.index());
                                if (it == texByIdx.end() || it->second == nullptr)
                                    continue;
                                if (!seenTex.insert(handle.index()).second)
                                    continue; // shared: first wins
                                const smvk::TextureInfo* t = it->second;
                                // EXACT ledger bytes (0 = untracked
                                // backing -> the estimate).
                                uint64_t bytes = t->allocBytes;
                                if (bytes == 0) {
                                    uint64_t est = 0;
                                    for (uint32_t m = 0; m < t->mipLevels; ++m) {
                                        const uint32_t w = std::max(t->extent.width >> m, 1u);
                                        const uint32_t h = std::max(t->extent.height >> m, 1u);
                                        const uint32_t d = std::max(t->extent.depth >> m, 1u);
                                        est += uint64_t(double(w * h * d) * formatBpp(t->format));
                                    }
                                    bytes = est * t->arrayLayers;
                                }
                                owners[ci].tex += bytes;
                                owners[ci].texList.emplace_back(bytes, t);
                            }
                        }
                    }
                    // + the filter's cluster-ordered index copy
                    // (not attributable to one cluster — it stays
                    // in the geometry total but outside the rows).
                    uint64_t filterIdxBytes = 0;
                    if (smvk::gfx::Valid(m_gfxRenderer.gfx))
                        filterIdxBytes = 4ull * m_gfxRenderer.gfx.filterIndices.size();
                    geoBytes += filterIdxBytes;
                    const uint64_t heapUsed = vramUsed + hostUsed;
                    // ---- category table (EXACT) ---------------
                    {
                        static const char* const kCatNames[smvk::kMemoryCategoryCount] = {
                            "Image content (textures)",
                            "Image targets (attachments)",
                            "Vertex buffers",
                            "Index buffers",
                            "Uniforms",
                            "Storage (gfx rings, HZB, draws)",
                            "Indirect",
                            "Staging / transfer",
                        };
                        auto row = [](const char* label, uint64_t bytes, uint64_t total) {
                            const float frac =
                                total > 0 ? float(double(bytes) / double(total)) : 0.0f;
                            char overlay[64];
                            std::snprintf(overlay,
                                          sizeof(overlay),
                                          "%.0f MiB (%.0f%%)",
                                          (double(bytes)) / 1048576.0,
                                          100.0 * frac);
                            ImGui::TextUnformatted(label);
                            ImGui::ProgressBar(
                                frac, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), overlay);
                        };
                        for (uint32_t c = 0; c < smvk::kMemoryCategoryCount; ++c)
                            if (ledger[c] > 0)
                                row(kCatNames[c], ledger[c], heapUsed);
                        row("Ledger total (exact)", ledgerTotal, heapUsed);
                        // Untracked: everything the ledger skips
                        // by design + VMA block slack. The heap
                        // usage minus the exact ledger.
                        const uint64_t untracked =
                            ledgerTotal < heapUsed ? heapUsed - ledgerTotal : 0;
                        row("Untracked (staging rings, heap-placed"
                            " targets, VMA block slack)",
                            untracked,
                            heapUsed);
                        // Cross-check lines (estimates): the
                        // texture estimate vs the exact image
                        // categories; the geometry formula.
                        ImGui::TextDisabled(
                            "  cross-check: textures est %.0f MiB"
                            " vs exact %.0f | largest est %.0f MiB"
                            " | geometry est %.0f MiB"
                            " (%zu meshes: 36 B/v + 4 B/i%s)",
                            (double(texEstBytes)) / 1048576.0,
                            (double(ledger[size_t(smvk::MemoryCategory::ImageContent)] +
                                    ledger[size_t(smvk::MemoryCategory::ImageTarget)])) /
                                1048576.0,
                            (double(texLargest)) / 1048576.0,
                            (double(geoBytes)) / 1048576.0,
                            meshCount,
                            filterIdxBytes > 0 ? " + filter index copy" : "");
                    }
                    // ---- render targets map (the ask: name every
                    // ImageTarget-category image, exact bytes;
                    // heap-placed/untracked get the computed
                    // estimate marked ~). Scrollable, uncapped:
                    // the target set is ~30 rows — the blob above
                    // becomes an addressable list.
                    ImGui::Separator();
                    ImGui::TextUnformatted("Render targets (exact; ~ = untracked est)");
                    {
                        auto fmtShort = [](smvk::Format f) -> const char* {
                            switch (f) {
                            case smvk::Format::BC1_RGB_UNorm:
                            case smvk::Format::BC1_RGB_SRGB:
                            case smvk::Format::BC1_RGBA_UNorm:
                            case smvk::Format::BC1_RGBA_SRGB:
                                return "BC1";
                            case smvk::Format::BC2_UNorm:
                            case smvk::Format::BC2_SRGB:
                                return "BC2";
                            case smvk::Format::BC3_UNorm:
                            case smvk::Format::BC3_SRGB:
                                return "BC3";
                            case smvk::Format::BC4_UNorm:
                            case smvk::Format::BC4_SNorm:
                                return "BC4";
                            case smvk::Format::BC5_UNorm:
                            case smvk::Format::BC5_SNorm:
                                return "BC5";
                            case smvk::Format::BC6H_UFloat:
                            case smvk::Format::BC6H_SFloat:
                                return "BC6H";
                            case smvk::Format::BC7_UNorm:
                            case smvk::Format::BC7_SRGB:
                                return "BC7";
                            case smvk::Format::RGBA8_UNorm:
                            case smvk::Format::RGBA8_SRGB:
                                return "RGBA8";
                            case smvk::Format::RGBA16_Float:
                                return "RGBA16F";
                            case smvk::Format::RGBA32_Float:
                                return "RGBA32F";
                            case smvk::Format::R16_Float:
                                return "R16F";
                            case smvk::Format::RG16_Float:
                                return "RG16F";
                            case smvk::Format::R32_Float:
                                return "R32F";
                            case smvk::Format::D32_Float:
                                return "D32F";
                            default:
                                return "fmt?";
                            }
                        };
                        struct TargetRow {
                            const smvk::MemoryAllocInfo* a;
                            uint64_t sortBytes; // max(exact, est)
                            uint64_t est;
                        };
                        std::vector<TargetRow> rows;
                        for (const auto& a : allocs) {
                            if (!a.isImage || a.category != smvk::MemoryCategory::ImageTarget)
                                continue;
                            // Estimate (the untracked backings
                            // AND a live cross-check on the
                            // tracked ones).
                            uint64_t est = 0;
                            for (uint32_t m = 0; m < a.mipLevels; ++m) {
                                const uint32_t w = std::max(a.extent.width >> m, 1u);
                                const uint32_t h = std::max(a.extent.height >> m, 1u);
                                const uint32_t d = std::max(a.extent.depth >> m, 1u);
                                est += uint64_t(double(w * h * d) * formatBpp(a.format));
                            }
                            est *= a.arrayLayers;
                            rows.push_back({&a, std::max(a.bytes, est), est});
                        }
                        std::sort(
                            rows.begin(), rows.end(), [](const TargetRow& x, const TargetRow& y) {
                                return x.sortBytes > y.sortBytes;
                            });
                        ImGui::BeginChild("##targets", ImVec2(0.0f, 200.0f), true);
                        for (const TargetRow& r : rows) {
                            const smvk::MemoryAllocInfo& a = *r.a;
                            const uint64_t show = a.bytes > 0 ? a.bytes : r.est;
                            ImGui::Text("%s%s: %.1f MiB  %ux%u %s"
                                        " x%u%s mips=%u layers=%u",
                                        a.bytes > 0 ? "" : "~",
                                        a.name.Str().c_str(),
                                        (double(show)) / 1048576.0,
                                        a.extent.width,
                                        a.extent.height,
                                        fmtShort(a.format),
                                        uint32_t(a.samples),
                                        a.bytes > 0 && a.bytes != r.est ? " (est differs)" : "",
                                        a.mipLevels,
                                        a.arrayLayers);
                        }
                        ImGui::EndChild();
                    }
                    // ---- top lists (immediate, exact where the
                    // ledger covers; geometry is the formula) ----
                    {
                        auto mibStr = [](uint64_t b) {
                            char buf[32];
                            std::snprintf(buf, sizeof(buf), "%.1f", (double(b)) / 1048576.0);
                            return std::string(buf);
                        };
                        // Top-16 textures by EXACT bytes (estimate
                        // fallback sorts after exact — same-key
                        // sort, the column marks which).
                        ImGui::Separator();
                        ImGui::TextUnformatted("Top textures (exact; est fallback)");
                        {
                            std::vector<std::pair<uint64_t, const smvk::TextureInfo*>> list;
                            list.reserve(texs.size());
                            for (const smvk::TextureInfo& t : texs) {
                                uint64_t b = t.allocBytes;
                                if (b == 0) {
                                    // untracked: estimate.
                                    uint64_t est = 0;
                                    for (uint32_t m = 0; m < t.mipLevels; ++m) {
                                        const uint32_t w = std::max(t.extent.width >> m, 1u);
                                        const uint32_t h = std::max(t.extent.height >> m, 1u);
                                        const uint32_t d = std::max(t.extent.depth >> m, 1u);
                                        est += uint64_t(double(w * h * d) * formatBpp(t.format));
                                    }
                                    b = est * t.arrayLayers;
                                }
                                list.emplace_back(b, &t);
                            }
                            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                                return a.first > b.first;
                            });
                            const size_t cap = std::min<size_t>(list.size(), 16);
                            for (size_t k = 0; k < cap; ++k) {
                                const auto& [bytes, t] = list[k];
                                ImGui::BulletText("%s%s: %s MiB  %ux%u mips=%u"
                                                  " layers=%u",
                                                  t->allocBytes > 0 ? "" : "~",
                                                  t->name.Str().c_str(),
                                                  mibStr(bytes).c_str(),
                                                  t->extent.width,
                                                  t->extent.height,
                                                  t->mipLevels,
                                                  t->arrayLayers);
                            }
                            if (list.size() > cap)
                                ImGui::TextDisabled("  + %zu more", list.size() - cap);
                        }
                        // Top-16 meshes (residency formula).
                        ImGui::Separator();
                        ImGui::TextUnformatted("Top meshes (36 B/v + 4 B/i)");
                        {
                            std::vector<const std::pair<uint64_t, std::string>*> list;
                            for (const Owner& o : owners)
                                for (const auto& m : o.meshList)
                                    list.push_back(&m);
                            std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
                                return a->first > b->first;
                            });
                            const size_t cap = std::min<size_t>(list.size(), 16);
                            for (size_t k = 0; k < cap; ++k)
                                ImGui::BulletText("%s: %s MiB",
                                                  list[k]->second.c_str(),
                                                  mibStr(list[k]->first).c_str());
                            if (list.size() > cap)
                                ImGui::TextDisabled("  + %zu more", list.size() - cap);
                        }
                        // Top-12 buffers (EXACT, from the ledger
                        // detail rows).
                        ImGui::Separator();
                        ImGui::TextUnformatted("Top buffers (exact)");
                        {
                            std::vector<const smvk::MemoryAllocInfo*> list;
                            for (const auto& a : allocs)
                                if (!a.isImage)
                                    list.push_back(&a);
                            std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
                                return a->bytes > b->bytes;
                            });
                            const size_t cap = std::min<size_t>(list.size(), 12);
                            for (size_t k = 0; k < cap; ++k)
                                ImGui::BulletText("%s: %s MiB",
                                                  list[k]->name.Str().c_str(),
                                                  mibStr(list[k]->bytes).c_str());
                            if (list.size() > cap)
                                ImGui::TextDisabled("  + %zu more", list.size() - cap);
                        }
                    }
                    // ---- Ownership rows + stacked color bar ----
                    // Cluster rows are TreeNodes: expanding one
                    // drills into ITS top textures (exact) +
                    // meshes (formula).
                    if (!owners.empty()) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Ownership by cluster (random colors,"
                                               " expand for detail)");
                        std::vector<size_t> order(owners.size());
                        for (size_t i = 0; i < order.size(); ++i)
                            order[i] = i;
                        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                            return owners[a].geo + owners[a].tex > owners[b].geo + owners[b].tex;
                        });
                        uint64_t ownedTotal = 0;
                        for (const Owner& o : owners)
                            ownedTotal += o.geo + o.tex;
                        if (ownedTotal > 0) {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            const ImVec2 p0 = ImGui::GetCursorScreenPos();
                            const float w = ImGui::GetContentRegionAvail().x;
                            const float h = 14.0f;
                            float x = p0.x;
                            for (size_t i : order) {
                                const float seg = float(double(owners[i].geo + owners[i].tex) /
                                                        double(ownedTotal)) *
                                                  w;
                                if (seg < 0.75f)
                                    continue;
                                dl->AddRectFilled(ImVec2(x, p0.y),
                                                  ImVec2(x + seg, p0.y + h),
                                                  owners[i].color,
                                                  3.0f);
                                x += seg;
                            }
                            dl->AddRect(
                                p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(255, 255, 255, 70), 3.0f);
                            ImGui::Dummy(ImVec2(w, h + 6.0f));
                            if (ImGui::IsItemHovered()) {
                                char tip[128];
                                std::snprintf(tip,
                                              sizeof(tip),
                                              "%.0f MiB attributed across"
                                              " %zu cluster(s)",
                                              (double(ownedTotal)) / 1048576.0,
                                              owners.size());
                                ImGui::SetTooltip("%s", tip);
                            }
                        }
                        const size_t cap = std::min<size_t>(order.size(), 12);
                        for (size_t k = 0; k < cap; ++k) {
                            const Owner& o = owners[order[k]];
                            ImGui::PushID(int(k));
                            ImGui::ColorButton("##own",
                                               ImVec4(float(o.color & 0xFF) / 255.0f,
                                                      float((o.color >> 8) & 0xFF) / 255.0f,
                                                      float((o.color >> 16) & 0xFF) / 255.0f,
                                                      1.0f),
                                               ImGuiColorEditFlags_NoTooltip |
                                                   ImGuiColorEditFlags_NoPicker,
                                               ImVec2(10.0f, 10.0f));
                            ImGui::SameLine();
                            const std::string nodeLabel = o.label + "##own";
                            const bool open = ImGui::TreeNode(nodeLabel.c_str());
                            ImGui::SameLine();
                            ImGui::Text("%.0f MiB"
                                        " (geo %.0f + tex %.0f,"
                                        " %zu meshes)",
                                        (double(o.geo + o.tex)) / 1048576.0,
                                        (double(o.geo)) / 1048576.0,
                                        (double(o.tex)) / 1048576.0,
                                        o.meshes);
                            if (open) {
                                ImGui::Indent();
                                // Drill-down: this cluster's top
                                // textures + meshes (sorted desc,
                                // capped 6 each).
                                {
                                    auto texTop = o.texList;
                                    std::sort(texTop.begin(),
                                              texTop.end(),
                                              [](const auto& a, const auto& b) {
                                                  return a.first > b.first;
                                              });
                                    const size_t n = std::min<size_t>(texTop.size(), 6);
                                    for (size_t j = 0; j < n; ++j)
                                        ImGui::BulletText("%s: %.1f MiB",
                                                          texTop[j].second->name.Str().c_str(),
                                                          (double(texTop[j].first)) / 1048576.0);
                                    if (texTop.size() > n)
                                        ImGui::TextDisabled("  + %zu more texture(s)",
                                                            texTop.size() - n);
                                }
                                {
                                    auto meshTop = o.meshList;
                                    std::sort(meshTop.begin(),
                                              meshTop.end(),
                                              [](const auto& a, const auto& b) {
                                                  return a.first > b.first;
                                              });
                                    const size_t n = std::min<size_t>(meshTop.size(), 6);
                                    for (size_t j = 0; j < n; ++j)
                                        ImGui::BulletText("%s: %.1f MiB",
                                                          meshTop[j].second.c_str(),
                                                          (double(meshTop[j].first)) / 1048576.0);
                                    if (meshTop.size() > n)
                                        ImGui::TextDisabled("  + %zu more mesh(es)",
                                                            meshTop.size() - n);
                                }
                                ImGui::Unindent();
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }
                        if (order.size() > cap) {
                            ImGui::TextDisabled("  + %zu more"
                                                " cluster(s)",
                                                order.size() - cap);
                        }
                    }
                }
            }
            ImGui::TextDisabled("Budget/usage: VMA + VK_EXT_memory_budget (the OS's\n"
                                "usable figure - can sit under the heap size when\n"
                                "other processes hold memory). Committed/Blocks:\n"
                                "VMA block statistics. Refresh: 1 Hz (Ctrl+M, the\n"
                                "HUD's VRAM line reads the same members).");
        }
        ImGui::End();
    }

    // ---- World Map viewer (C) --------------------------------
    // Floating top-down orthographic canvas (see DrawWorldMapWindow):
    // heightfield background + volume/probe/light/camera layers.
    if (m_worldMapWindow)
        DrawWorldMapWindow();

    // Forward+ light debug overlay: project the CPU light copy
    // (m_debugLights, filled in UpdateForwardPlusLights) through the
    // UNJITTERED viewProj into the ImGui foreground draw list. It
    // rides the UI pass and composites over the tonemapped scene.
    // The jittered frame matrix would buzz every marker +/-0.25 px
    // per frame (the UI layer has no temporal resolve). No depth
    // test: occluded / behind-camera lights stay visible (debug).
    if ((m_showLightPoints || m_showLightNumbers) && !m_debugLights.empty()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float r = std::max(m_lightPointSize, 1.0f);
        const auto toSrgb8 = [](float c) {
            c = std::min(std::max(c, 0.0f), 1.0f);
            return (int)std::lround(std::pow(c, 1.0f / 2.2f) * 255.0f);
        };
        uint32_t labels = 0;
        const uint32_t maxLabels = (uint32_t)std::max(0, m_maxLightLabels);
        for (uint32_t i = 0; i < m_debugLights.size(); ++i) {
            const DebugLight& l = m_debugLights[i];
            const glm::vec4 clip = m_unjitteredViewProj * glm::vec4(l.position, 1.0f);
            if (clip.w <= 1e-4f)
                continue; // behind the camera / degenerate
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f)
                continue; // off-screen
            // viewProj is post-Y-flip (Vulkan framebuffer y-down), so
            // NDC maps straight onto ImGui's y-down display space.
            const ImVec2 pt((ndc.x * 0.5f + 0.5f) * disp.x, (ndc.y * 0.5f + 0.5f) * disp.y);
            const ImU32 col =
                IM_COL32(toSrgb8(l.color.r), toSrgb8(l.color.g), toSrgb8(l.color.b), 220);
            if (l.type == (int)smvk::kForwardPlusSpot) {
                // Spots: diamond marker so cones read differently.
                dl->AddQuadFilled(ImVec2(pt.x, pt.y - r),
                                  ImVec2(pt.x + r, pt.y),
                                  ImVec2(pt.x, pt.y + r),
                                  ImVec2(pt.x - r, pt.y),
                                  col);
            } else {
                dl->AddCircleFilled(pt, r, col);
            }
            if (m_showLightNumbers && labels < maxLabels) {
                char label[16];
                snprintf(label, sizeof(label), "%u", i);
                dl->AddText(
                    ImVec2(pt.x + r + 2.0f + 1.0f, pt.y - r + 1.0f), IM_COL32(0, 0, 0, 200), label);
                dl->AddText(ImVec2(pt.x + r + 2.0f, pt.y - r), IM_COL32_WHITE, label);
                ++labels;
            }
        }
    }
    // GI PROBE SPHERES: RETIRED HERE — the 3D successor lives in the
    // rendergraph (Rendergraph/giprobe.h, the GiProbes pass at the HDR
    // tail — instanced icospheres colored by each probe's baked
    // directional GI, depth-tested against the scene). The old ImGui
    // foreground dots (no depth test, colored by obstacle distance)
    // died with this block; the checkbox + the distance slider below
    // now drive the pass.

    // GI volume bounding-box wireframe (m_giShowVolumeBounds): AABB/OBB
    // edges through the UNJITTERED viewProj into the ImGui foreground
    // list, so they ride on top of the current view with no depth test
    // (debug intent: authoring bounds against walls/roofs). Sources:
    // regular OBB volumes (green), convex grid AABBs (priority hue,
    // like the editor LIGHTING preview; the legacy octree bounds box
    // died with the .sggi loader). Edges whose both endpoints project
    // (in front of the camera) draw; partially behind-camera boxes
    // clip per-edge (may drop the crossing segment).
    if (m_giShowVolumeBounds && (m_regLoaded || m_giVolumesLoaded)) {
        ImDrawList* bdl = ImGui::GetForegroundDrawList();
        const ImVec2 bdisp = ImGui::GetIO().DisplaySize;
        const auto projectBox = [&](const glm::vec3& w, ImVec2& out) {
            const glm::vec4 clip = m_unjitteredViewProj * glm::vec4(w, 1.0f);
            if (clip.w <= 1e-4f)
                return false; // behind the camera
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (std::abs(ndc.x) > 1.2f || std::abs(ndc.y) > 1.2f || ndc.z < -1.0f || ndc.z > 1.0f)
                return false; // off-screen / outside depth range
            // viewProj is post-Y-flip (Vulkan y-down): NDC maps
            // straight onto ImGui's y-down display space.
            out.x = (ndc.x * 0.5f + 0.5f) * bdisp.x;
            out.y = (ndc.y * 0.5f + 0.5f) * bdisp.y;
            return true;
        };
        const auto drawBoxEdges = [&](const glm::vec3 q[8], ImU32 col) {
            // q: mn/mx corners (x fast, then y, then z):
            // 0..3 = z-min ring, 4..7 = z-max ring.
            static const int kEdges[12][2] = {
                {0, 1},
                {1, 2},
                {2, 3},
                {3, 0},
                {4, 5},
                {5, 6},
                {6, 7},
                {7, 4},
                {0, 4},
                {1, 5},
                {2, 6},
                {3, 7},
            };
            for (const auto& e : kEdges) {
                ImVec2 a, b;
                if (projectBox(q[e[0]], a) && projectBox(q[e[1]], b))
                    bdl->AddLine(a, b, col, 1.5f);
            }
        };
        const auto boxCorners = [](const glm::vec3& mn, const glm::vec3& mx, glm::vec3 q[8]) {
            q[0] = {mn.x, mn.y, mn.z};
            q[1] = {mx.x, mn.y, mn.z};
            q[2] = {mx.x, mx.y, mn.z};
            q[3] = {mn.x, mx.y, mn.z};
            q[4] = {mn.x, mn.y, mx.z};
            q[5] = {mx.x, mn.y, mx.z};
            q[6] = {mx.x, mx.y, mx.z};
            q[7] = {mn.x, mx.y, mx.z};
        };
        uint32_t boxesDrawn = 0;
        if (m_regLoaded) {
            for (size_t vi = 0; vi < m_regVolumes.size(); ++vi) {
                const auto& obb = m_regVolumes[vi].volume.obb;
                const glm::vec3 ex = obb.axisX * obb.halfExtents.x;
                const glm::vec3 ey = obb.axisY * obb.halfExtents.y;
                const glm::vec3 ez = obb.axisZ * obb.halfExtents.z;
                glm::vec3 q[8];
                q[0] = obb.center - ex - ey - ez;
                q[1] = obb.center + ex - ey - ez;
                q[2] = obb.center + ex + ey - ez;
                q[3] = obb.center - ex + ey - ez;
                q[4] = obb.center - ex - ey + ez;
                q[5] = obb.center + ex - ey + ez;
                q[6] = obb.center + ex + ey + ez;
                q[7] = obb.center - ex + ey + ez;
                drawBoxEdges(q, IM_COL32(70, 230, 90, 220));
                ++boxesDrawn;
                ImVec2 c;
                if (projectBox(obb.center, c)) {
                    char lab[32];
                    snprintf(lab, sizeof(lab), "reg%u", (unsigned)vi);
                    bdl->AddText(ImVec2(c.x + 5.0f, c.y - 8.0f), IM_COL32(70, 230, 90, 220), lab);
                }
            }
        }
        if (m_giVolumesLoaded) {
            for (size_t vi = 0; vi < m_giVolumes.size(); ++vi) {
                const auto& vol = m_giVolumes[vi].volume;
                const glm::vec3 mn = vol.gridMin;
                const glm::vec3 mx =
                    vol.gridMin + glm::vec3(vol.dims[0], vol.dims[1], vol.dims[2]) * vol.spacing;
                glm::vec3 q[8];
                boxCorners(mn, mx, q);
                const float hue = std::fmod(float(vol.def.priority) * 0.13f, 1.0f);
                ImColor ic = ImColor::HSV(hue, 0.7f, 1.0f);
                ic.Value.w = 220.0f / 255.0f;
                drawBoxEdges(q, (ImU32)ic);
                ++boxesDrawn;
                ImVec2 c;
                if (projectBox((mn + mx) * 0.5f, c)) {
                    char lab[64];
                    snprintf(lab, sizeof(lab), "cv%u p%u", (unsigned)vi, vol.def.priority);
                    bdl->AddText(ImVec2(c.x + 5.0f, c.y - 8.0f), (ImU32)ic, lab);
                }
            }
        }
        char bcap[96];
        snprintf(bcap, sizeof(bcap), "GI volumes: %u bounds", boxesDrawn);
        bdl->AddText(ImVec2(8.0f + 1.0f, 36.0f + 1.0f), IM_COL32(0, 0, 0, 200), bcap);
        bdl->AddText(ImVec2(8.0f, 36.0f), IM_COL32_WHITE, bcap);
    }

    // Reflection-probe location overlay (cubemap panel toggle): one
    // marker per cached probe row - center dot + index, solid ring =
    // full-weight influence, dim ring = fade extent. Bright green =
    // camera inside influence, yellow = in fade, gray = out of range:
    // fly to the chrome ball and the covering probe lights up. Answers
    // "is there a cubemap near X?" at a glance, including embedded
    // probes (markers inside walls) the bake should have culled.
    if (m_cubemapShowProbes && !m_cubemapProbes.empty()) {
        ImDrawList* cdl = ImGui::GetForegroundDrawList();
        const ImVec2 cdisp = ImGui::GetIO().DisplaySize;
        const glm::vec3 ccamPos = m_cameraPositioner.cameraPosition_;
        const float cmaxDist = std::max(m_cubemapOverlayMaxDist, 1.0f);
        const float cmaxDist2 = cmaxDist * cmaxDist;
        uint32_t cdrawn = 0;
        for (uint32_t ci = 0; ci < m_cubemapProbes.size() && cdrawn < 64; ++ci) {
            const CubemapProbeGpu& pr = m_cubemapProbes[ci];
            const glm::vec3 ppos(pr.posInfluence.x, pr.posInfluence.y, pr.posInfluence.z);
            const float influence = pr.posInfluence.w;
            const float fade = pr.fadeCubemap.x;
            const glm::vec3 dc = ppos - ccamPos;
            if (glm::dot(dc, dc) > cmaxDist2)
                continue;
            const glm::vec4 cclip = m_unjitteredViewProj * glm::vec4(ppos, 1.0f);
            if (cclip.w <= 1e-4f)
                continue;
            const glm::vec3 cndc = glm::vec3(cclip) / cclip.w;
            if (std::abs(cndc.x) > 1.0f || std::abs(cndc.y) > 1.0f)
                continue;
            const ImVec2 cpt((cndc.x * 0.5f + 0.5f) * cdisp.x, (cndc.y * 0.5f + 0.5f) * cdisp.y);
            const float cdist = std::sqrt(glm::dot(dc, dc));
            ImU32 col;
            if (cdist <= influence)
                col = IM_COL32(80, 255, 120, 230);
            else if (fade > 0.0f && cdist <= influence + fade)
                col = IM_COL32(255, 220, 80, 230);
            else
                col = IM_COL32(120, 140, 160, 160);
            // Influence ring: project a world-X offset of one radius.
            const glm::vec4 eclip =
                m_unjitteredViewProj *
                glm::vec4(ppos + glm::vec3(std::max(influence, 0.01f), 0.0f, 0.0f), 1.0f);
            if (eclip.w > 1e-4f) {
                const glm::vec3 endc = glm::vec3(eclip) / eclip.w;
                const ImVec2 ept((endc.x * 0.5f + 0.5f) * cdisp.x,
                                 (endc.y * 0.5f + 0.5f) * cdisp.y);
                const float rPx = std::sqrt((ept.x - cpt.x) * (ept.x - cpt.x) +
                                            (ept.y - cpt.y) * (ept.y - cpt.y));
                if (rPx > 2.0f && rPx < 4000.0f) {
                    cdl->AddCircle(cpt, rPx, col & IM_COL32(255, 255, 255, 90), 48);
                    if (fade > 0.0f) {
                        const float fr = rPx * (influence + fade) / std::max(influence, 1e-3f);
                        if (fr < 4000.0f)
                            cdl->AddCircle(cpt, fr, col & IM_COL32(255, 255, 255, 45), 48);
                    }
                }
            }
            cdl->AddCircleFilled(cpt, 3.5f, col);
            char clab[16];
            snprintf(clab, sizeof(clab), "C%u", ci);
            cdl->AddText(ImVec2(cpt.x + 6.0f, cpt.y - 7.0f), col, clab);
            ++cdrawn;
        }
        char ccap[160];
        snprintf(ccap,
                 sizeof(ccap),
                 "Cubemap probes: %u drawn (<= %.0fm, %s)",
                 cdrawn,
                 cmaxDist,
                 ProbeTileListsWanted() ? "light-tail tiling fresh"
                                        : "tiling STALE (global cubemap)");
        cdl->AddText(ImVec2(8.0f + 1.0f, 58.0f + 1.0f), IM_COL32(0, 0, 0, 200), ccap);
        cdl->AddText(ImVec2(8.0f, 58.0f), IM_COL32_WHITE, ccap);
    }

#if defined(DS_DEBUG_VIZ)
    // DS visualizer: selected-light marker. White ring + "DS#<idx>" at
    // the light's captured POST-SORT position (m_debugLights above holds
    // the pre-sort order, which does not match dsDebugLight's index
    // space). Drawn only while a light-scoped mode is active.
    if (m_dsDebugMode >= DsDebugViz::CullStep && m_dsDebugLightValid && DsActive()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const glm::vec4 clip = m_unjitteredViewProj * glm::vec4(m_dsDebugLightPos, 1.0f);
        if (clip.w > 1e-4f) {
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f) {
                const ImVec2 pt((ndc.x * 0.5f + 0.5f) * disp.x, (ndc.y * 0.5f + 0.5f) * disp.y);
                dl->AddCircle(
                    pt, std::max(m_lightPointSize, 4.0f) + 5.0f, IM_COL32_WHITE, 24, 2.0f);
                char label[32];
                snprintf(label, sizeof(label), "DS#%d", m_dsDebugLight);
                dl->AddText(
                    ImVec2(pt.x + 1.0f, pt.y - 26.0f + 1.0f), IM_COL32(0, 0, 0, 200), label);
                dl->AddText(ImVec2(pt.x, pt.y - 26.0f), IM_COL32_WHITE, label);
            }
        }
    }
#endif

    // Host-installed extra UI (SetExtraUi): app-owned overlays
    // (the sponza invaders HUD) draw between the panel/tool
    // windows and the pass recording, inside the same ImGui
    // frame — same target, same composite.
    if (m_extraUi)
        m_extraUi();
}
} // namespace engine
