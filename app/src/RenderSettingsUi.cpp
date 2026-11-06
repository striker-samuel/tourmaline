// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// This UI is compiled only into the standalone App host, not the renderer.
// WorldRenderer exposes renderer state through a host-supplied UI callback.
#include <Engine/Renderer/WorldRenderer.h>
#include <Engine/Renderer/gfx_filter.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/RendererLimits.h>
#include <Engine/Rendergraph/volumetric.h>
#include <Engine/App/world.h>
#include <Engine/App/app.h>
#include <Engine/RHI/resource_manager.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
static constexpr uint32_t FP_MAX_LIGHTS =
	engine::renderer::limits::MaxSimultaneousLights;
static constexpr uint32_t FP_MAX_DECALS = 256;
static constexpr uint32_t FP_MAX_DECALS_PER_TILE = 16;
static constexpr uint32_t PARTICLE_MAX_EMITTERS = 16;
void WorldRenderer::DrawAppSettingsUi(const DebugUiFrame& frame) {
    if (!frame.fb || !frame.env) return;
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
			ImGui::Begin("Tourmaline - Debug", &m_debugPanelOpen,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
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
						const char* modeNames[] = { "VSync FIFO (strict)", "VSync Mailbox (smooth)",
							"Uncapped Immediate", "VSync FIFO relaxed (no 30fps lock)" };
						int modeIdx = static_cast<int>(m_host.GetPresentMode());
						if (ImGui::Combo("Present mode", &modeIdx, modeNames, 4))
							SetPresentMode(static_cast<smvk::PresentMode>(modeIdx));
						ImGui::SliderFloat("Render scale", &m_renderScale,
							RENDER_SCALE_MIN, RENDER_SCALE_MAX, "%.2fx");
						ImGui::SliderFloat("UI res scale", &m_uiScale,
							UI_SCALE_MIN, UI_SCALE_MAX, "%.2fx");
						ImGui::SliderFloat("UI DPI scale", &m_uiDpiScale,
							UI_DPI_MIN, UI_DPI_MAX, "%.2fx");
						if (ImGui::IsItemEdited() && m_uiOverlay)
							m_uiOverlay->ApplyDpiScale(m_uiDpiScale);
						// Conventional order for the combo; indices remap to the
						// sl-aligned DlssMode enum values.
						const char* dlssNames[] = { "Off", "Ultra Performance", "Performance",
							"Balanced", "Quality", "Ultra Quality", "DLAA" };
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
								renderExtent.width, renderExtent.height,
								swapExtent.width, swapExtent.height,
								m_dlssPhase, m_dlssPhaseCount);
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
						const char* aaNames[] = { "Off", "MSAA 2x", "SMAA 1x", "SMAA T2x", "MSAA 2x Custom Resolve" };
						int aaIdx = static_cast<int>(m_aaMode);
						ImGui::BeginDisabled(m_dlssFrameActive);
						if (ImGui::Combo("Antialiasing (Z / 1-4)", &aaIdx, aaNames, 5))
							m_aaMode = static_cast<AaMode>(aaIdx);
						ImGui::EndDisabled();
						// Transparency technique for Blend materials.
						const char* transpModes[] = { "Dithered (default)", "Alpha blend" };
						int transpIdx = (m_transparentMode == smvk::TransparentMode::Blended) ? 1 : 0;
						if (ImGui::Combo("Transparency", &transpIdx, transpModes, 2))
							m_transparentMode = (transpIdx == 1)
							? smvk::TransparentMode::Blended
							: smvk::TransparentMode::Dithered;
						// Texture filtering: applies to the bindless samplers
						// (slots 0 + 2) via in-place recreate.
						const char* filtNames[] = { "Trilinear", "Anisotropic 2x",
							"Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x" };
						int filtIdx = static_cast<int>(m_textureFiltering);
						if (ImGui::Combo("Texture filtering", &filtIdx, filtNames, 5))
							SetTextureFiltering(static_cast<TextureFiltering>(
								std::clamp(filtIdx, 0, 4)));
					}

					// ---- Shadows: directional cascades, local-light tiles
					// and the filtering mode.
					ImGui::SeparatorText("Shadows");
					{
						// Tile resolutions (atlases recreate once on change via
						// the per-frame Ensure; one-time hitch).
						const uint32_t resOptions[] = { 512, 1024, 2048, 4096 };
						const char* resNames[] = { "512", "1024", "2048", "4096" };
						int resIdx = 1; // canon = 1024
						for (int i = 0; i < 4; ++i)
							if (resOptions[i] == m_cascadeResolution)
								resIdx = i;
						if (ImGui::Combo("Cascade resolution", &resIdx, resNames, 4))
							m_cascadeResolution = resOptions[std::clamp(resIdx, 0, 3)];
						// Local-light tile resolution (the SHARED spot + point
						// atlas; PCF depth + the EVSM moment twin).
						const uint32_t spotOptions[] = { 256, 512, 1024 };
						const char* spotNames[] = { "256", "512 (PS5)", "1024" };
						int spotIdx = 1; // canon = 512
						for (int i = 0; i < 3; ++i)
							if (spotOptions[i] == m_spotShadowResolution)
								spotIdx = i;
						if (ImGui::Combo("Spot/point tile resolution", &spotIdx, spotNames, 3))
							m_spotShadowResolution = spotOptions[std::clamp(spotIdx, 0, 2)];
						ImGui::SliderFloat("Split lambda", &m_cascadeLambda, 0.0f, 1.0f, "%.2f");
						ImGui::Checkbox("SDSM (fit splits to measured depth)", &m_sdsmEnabled);
						ImGui::SliderFloat("Shadow distance (m, 0 = scene far)", &m_cascadeMaxDistance, 0.0f, 500.0f, "%.0f");
						ImGui::Text("Effective far: %.1f m (scene far %.1f m)",
							(double)CascadeFar(), (double)m_sceneFarClip);
						ImGui::SliderFloat("Depth bias scale", &m_cascadeDepthBiasScale, 0.0f, 4.0f, "%.2f");
						ImGui::SliderFloat("Normal bias scale", &m_cascadeNormalBiasScale, 0.0f, 4.0f, "%.2f");
						ImGui::Separator();
						ImGui::Checkbox("Spot shadows (per-light flag)", &m_spotShadowsEnabled);
						int spotMax = m_spotShadowMax;
						if (ImGui::SliderInt("Shadow spots max", &spotMax, 0, (int)smvk::kMaxSpotShadows))
							m_spotShadowMax = spotMax;
						ImGui::SliderFloat("Shadow max distance (m)", &m_spotShadowMaxDistance, 5.0f, 120.0f);
						// Point lights: six cube-face tiles per selected light in
						// the SAME atlas (PCF or the EVSM moments).
						ImGui::Checkbox("Point shadows (per-light flag)", &m_pointShadowsEnabled);
						int pointMax = m_pointShadowMax;
						if (ImGui::SliderInt("Shadow points max", &pointMax, 0, (int)smvk::kMaxPointShadowLights))
							m_pointShadowMax = pointMax;
						ImGui::SliderFloat("Point shadow max distance (m)", &m_pointShadowMaxDistance, 5.0f, 120.0f);
						ImGui::Separator();
						// EVSM: RGBA32F 2xMSAA moments + async separable blur,
						// 1-frame-lag pipelined. Off = PCF depth atlases.
						ImGui::Checkbox("EVSM enabled", &m_evsmEnabled);
						ImGui::SameLine();
						ImGui::TextDisabled(EvsmActive() ? "(active)" : "(PCF fallback)");
						int evsmPipe = (m_evsmPipeline == smvk::EvsmPipelineMode::SameFrame) ? 1 : 0;
						if (ImGui::Combo("Pipeline", &evsmPipe, "One-frame lag\0Same-frame\0")) {
							m_evsmPipeline = (evsmPipe == 1)
								? smvk::EvsmPipelineMode::SameFrame
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
						static const char* const kAoNames[3] = { "No AO", "SSAO", "SAO" };
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
						ImGui::SliderFloat("Cubemap intensity", &m_cubemapIntensity, 0.0f, 4.0f, "%.2f");
						ImGui::SliderFloat("Cubemap LOD bias", &m_cubemapLodBias, -2.0f, 4.0f, "%.2f");
						if (!m_cubemapRows.empty()) {
							int streamBudget = int(m_cubemapStreamBudget);
							if (ImGui::SliderInt("Stream budget (nearest rows)",
								&streamBudget, 0, 64))
								m_cubemapStreamBudget =
								uint32_t(std::clamp(streamBudget, 0, 64));
						}
						ImGui::Checkbox("Screen-space reflections (SSR)", &m_ssrEnabled);
						if (m_ssrEnabled) {
							ImGui::Checkbox("Static SDF off-screen reflections (experimental)",
								&m_staticSdfReflectionsEnabled);
							if (m_staticSdfReflectionsEnabled)
								ImGui::TextDisabled("GPU SDF: %u static opaque instances (baked sidecars only)",
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
							const char* blurModes[2] = {
								"2D bilateral (baseline)",
								"Separable H+V (fast)"
							};
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
							ImGui::SliderFloat("Contact distance (m)", &m_contactShadowMaxDistance, 0.05f, 2.0f);
							ImGui::SliderFloat("Contact thickness (m)", &m_contactShadowThickness, 0.005f, 0.2f);
							ImGui::SliderFloat("Contact start bias (m)", &m_contactShadowStartBias, 0.0f, 0.05f);
							ImGui::SliderInt("Contact steps", &m_contactShadowSteps, 4, 32);
							const char* contactRes[2] = { "Full", "Half" };
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
                                ImGui::SetTooltip("VK_KHR_acceleration_structure + VK_KHR_ray_query not available");
                            ImGui::EndDisabled();
                        } else if (m_rayTracedSunShadowsEnabled) {
                            if (m_context->SupportsOpacityMicromaps()) {
                                ImGui::Checkbox("Opacity micromaps (alpha MASK foliage)",
                                    &m_opacityMicromapsEnabled);
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("VK_EXT_opacity_micromap: GPU-baked 16 microtriangles per masked triangle. Rebuilds BLAS/TLAS on changes.");
                            } else {
                                ImGui::TextDisabled("Opacity micromaps unsupported (VK_EXT_opacity_micromap)");
                            }
                            ImGui::SliderFloat("Hardware ray distance (m)",
                                &m_rayTracedSunShadowDistance, 1.0f, 2000.0f);
                            ImGui::SliderFloat("Hardware ray origin bias (m)",
                                &m_rayTracedSunShadowBias, 0.001f, 0.2f);
                            const char* rtRes[2] = { "Full", "Half" };
                            int rtResIdx = m_rayTracedSunShadowResolutionDivisor == 1 ? 0 : 1;
                            if (ImGui::Combo("Hardware ray resolution", &rtResIdx, rtRes, 2))
                                m_rayTracedSunShadowResolutionDivisor = rtResIdx == 0 ? 1 : 2;
                            // CPU snapshot from the last caster-list change. No readback,
                            // GPU query or extra draw pass is performed by this inspector.
                            if (ImGui::CollapsingHeader("TLAS / BLAS inspector")) {
                                const auto& summary = m_rayTracingDebugSummary;
                                ImGui::Text("Ray-query device: %s", raySupported ? "supported" : "unsupported");
                                ImGui::Text("GPU TLAS: %s",
                                    m_context->RayTracingWorldReady() ? "ready" : "not built / failed");
                                ImGui::Text("TLAS candidate instances: %zu (rejected: %u)",
                                    summary.instances.size(), summary.rejected);
                                ImGui::Text("Unique BLAS candidates: %zu", summary.blases.size());
                                ImGui::Text("Triangles: %llu instanced / %llu unique",
                                    (unsigned long long)summary.instancedTriangles,
                                    (unsigned long long)summary.uniqueTriangles);
                                ImGui::TextDisabled("CPU candidates, not GPU BVH nodes, memory sizes or timings.");
                                if (ImGui::TreeNode("BLAS geometry")) {
                                    const size_t shown = std::min<size_t>(summary.blases.size(), 256);
                                    for (size_t i = 0; i < shown; ++i) {
                                        const auto& blas = summary.blases[i];
                                        ImGui::BulletText("#%zu: %u triangles, %u TLAS refs%s",
                                            i, blas.triangles, blas.references,
                                            blas.alphaMasked ? " (alpha MASK)" : "");
                                    }
                                    if (shown < summary.blases.size())
                                        ImGui::TextDisabled("Showing first %zu of %zu BLAS", shown, summary.blases.size());
                                    ImGui::TreePop();
                                }
                                if (ImGui::TreeNode("TLAS instance transforms")) {
                                    const size_t shown = std::min<size_t>(summary.instances.size(), 256);
                                    for (size_t i = 0; i < shown; ++i) {
                                        const auto& entry = summary.instances[i];
                                        const auto& src = m_rayTracingCachedInstances[entry.sourceIndex];
                                        ImGui::BulletText("#%zu -> BLAS %u (%u triangles), pos (%.2f, %.2f, %.2f)",
                                            i, entry.blasIndex, entry.triangles,
                                            src.transform[3], src.transform[7], src.transform[11]);
                                    }
                                    if (shown < summary.instances.size())
                                        ImGui::TextDisabled("Showing first %zu of %zu instances", shown, summary.instances.size());
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
							ImGui::SliderFloat("SSGI diffuse intensity", &m_ssgiDiffuseIntensity, 0.0f, 4.0f);
							ImGui::SliderFloat("SSGI diffuse max distance (m)", &m_ssgiDiffuseMaxDistance, 0.5f, 50.0f);
							ImGui::SliderFloat("SSGI diffuse thickness (m)", &m_ssgiDiffuseThickness, 0.01f, 2.0f);
							ImGui::SliderFloat("SSGI diffuse bilateral sharpness", &m_ssgiDiffuseBlurSharpness, 0.0f, 32.0f);
							ImGui::SliderInt("SSGI diffuse traversal steps", &m_ssgiDiffuseMaxIntersections, 4, 128);
							ImGui::SliderFloat("SSGI diffuse temporal blend", &m_ssgiDiffuseTemporalBlend, 0.0f, 0.98f);
							ImGui::Checkbox("SSGI diffuse temporal filter", &m_ssgiDiffuseTaaEnabled);
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("Motion-reprojected accumulation with depth + normal rejection; falls back to the spatial-only path without motion vectors.");
						}
					}

					// ---- Post Processing: exposure, tonemap, bloom, motion
					// blur, DOF, LUT.
					ImGui::SeparatorText("Post Processing");
					{
						ImGui::Checkbox("Auto-exposure (X)", &m_autoExposureEnabled);
						{
							static const char* const kTonemapNames[4] =
							{ "Reinhard", "ACES", "Uncharted2", "GT7" };
							int tonemapIdx = std::clamp(static_cast<int>(m_tonemapMode), 0, 3);
							if (ImGui::Combo("Tonemap operator", &tonemapIdx, kTonemapNames, 4)) {
								m_tonemapMode = static_cast<uint32_t>(std::clamp(tonemapIdx, 0, 3));
								m_viewportResources.tonemap.desc.tonemapMode = m_tonemapMode;
							}
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("GT7: Polyphony Digital SDR (250-nit paper white, ICtCp chroma preserve).");
						}
						ImGui::SliderFloat("Manual exposure", &m_manualExposure,
							0.00005f, 0.02f, "%.5f", ImGuiSliderFlags_Logarithmic);
						ImGui::SliderFloat("EV bias", &m_exposureEvBias, -4.0f, 4.0f, "%.1f");
						ImGui::SliderFloat("Adapt to dark (/s)", &m_exposureSpeedToDark, 0.1f, 8.0f, "%.2f");
						ImGui::SliderFloat("Adapt to bright (/s)", &m_exposureSpeedToBright, 0.1f, 8.0f, "%.2f");
						ImGui::SliderFloat("Min exposure", &m_exposureMin,
							0.000001f, 0.001f, "%.6f", ImGuiSliderFlags_Logarithmic);
						ImGui::SliderFloat("Max exposure", &m_exposureMax,
							0.005f, 0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);
						if (m_exposureMin > m_exposureMax)
							std::swap(m_exposureMin, m_exposureMax);
						ImGui::Checkbox("Bloom (B)", &m_bloomEnabled);
						if (m_bloomEnabled) {
							ImGui::SliderFloat("Bloom intensity", &m_bloomIntensity, 0.0f, 2.0f, "%.2f");
							ImGui::SliderFloat("Bloom threshold (HDR)", &m_bloomThreshold, 0.0f, 8000.0f, "%.0f");
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("Pre-exposure HDR (sun-lit diffuse spans ~0-1600);\n"
									"knee tracks at 0.25x. Lower until highlights glow.");
						}
						ImGui::Checkbox("Motion blur (M)", &m_motionBlurEnabled);
						if (m_motionBlurEnabled) {
							ImGui::SliderInt("Blur samples", &m_motionBlurSamples, 1, 32);
							ImGui::SliderFloat("Blur max radius (px)", &m_motionBlurMaxRadius, 0.0f, 64.0f);
							ImGui::SliderFloat("Blur strength", &m_motionBlurStrength, 0.0f, 2.0f);
							ImGui::Checkbox("Tile dilation (16px tiles, 5x5)", &m_motionBlurTileDilation);
						}
						ImGui::Checkbox("Depth of field", &m_dofEnabled);
						if (m_dofEnabled) {
							ImGui::Checkbox("HQ bokeh (48-tap hex gather)", &m_dofHqBokeh);
							ImGui::SliderFloat("Focus distance (m)", &m_dofFocusDistance, 0.5f, 100.0f);
							ImGui::SliderFloat("Focus range (m)", &m_dofFocusRange, 0.0f, 20.0f);
							ImGui::SliderFloat("Low max radius (px)", &m_dofMaxRadius, 0.0f, 64.0f);
							ImGui::SliderFloat("Bokeh max radius (px)", &m_dofBokehMaxRadius, 0.0f, 64.0f);
							ImGui::SliderFloat("Highlight boost", &m_dofHighlightBoost, 1.0f, 4.0f);
							ImGui::SliderFloat("Highlight threshold", &m_dofHighlightThreshold, 0.0f, 8.0f);
						}
						// Color grading / LUT.
						const char* current = m_lutFile.empty() ? "Off" : m_lutFile.c_str();
						if (ImGui::BeginCombo("Color LUT", current)) {
							if (ImGui::Selectable("Off", m_lutFile.empty())) {
								m_lutFile.clear();
								m_lutTexture = {};
								m_lutBindless = UINT32_MAX;
							}
							for (const std::string& f : m_lutFiles) {
								const bool selected = (m_lutFile == f);
								if (ImGui::Selectable(f.c_str(), selected))
									LoadLut(f);
							}
							ImGui::EndCombo();
						}
						if (!m_lutFile.empty())
							ImGui::SliderFloat("LUT intensity", &m_lutIntensity, 0.0f, 1.0f, "%.2f");
						ImGui::SameLine();
						if (ImGui::Button("Rescan"))
							ScanLutFiles();
					}

					// ---- Atmosphere: sky, sun, zones, fog.
					ImGui::SeparatorText("Atmosphere");
					{
						ImGui::Checkbox("Sky dome (K)", &m_skyEnabled);
						if (m_world) {
							tour::scene::SceneEnv& se = m_world->scene.environment;
							auto authored = [&]() { m_world->scene.envAuthored = true; };
							ImGui::ColorEdit3("Sky color", &se.skyColor.x);
							ImGui::DragFloat("Sky intensity (lux)", &se.skyIntensity,
								5.0f, 0.0f, 10000.0f, "%.0f");
							ImGui::Separator();
							if (ImGui::SliderFloat("Sun azimuth", &se.sunAzimuth, 0.0f, 360.0f)) authored();
							if (ImGui::SliderFloat("Sun elevation", &se.sunElevation, 0.0f, 90.0f)) authored();
							if (ImGui::ColorEdit3("Sun color", &se.sunColor.x)) authored();
							ImGui::DragFloat("Sun intensity (lux)", &se.sunIntensity,
								10.0f, 0.0f, 2e5f, "%.0f");
							ImGui::Separator();
							// Fog: the combo is the single source of truth for the
							// active path (tier 2 = the raymarched volumetrics).
							const char* tiers[3] = {
								"Off", "Analytic (exp2, fallback)", "Volumetric (raymarched GI-lit)"
							};
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
								if (ImGui::ColorEdit3("Fog color", &se.fogColor.x)) authored();
								if (ImGui::SliderFloat("Fog density", &se.fogDensity, 0.0f, 1.0f)) authored();
							}
						}
						else {
							m_volumetricEnabled = false;
						}
						if (m_world && m_world->scene.environment.fogQuality == 2) {
							// Re-declared for this block: the se/authored pair above
							// lives inside the earlier `if (m_world)` scope, which
							// MSVC leaks but GCC (correctly) does not - the Linux CI
							// build failed with "'se' was not declared in this scope".
							tour::scene::SceneEnv& se = m_world->scene.environment;
							auto authored = [&]() { m_world->scene.envAuthored = true; };
							static const char* const kVolNames[5] = {
								"Low (8 steps)", "Medium (16 steps)", "High (32 steps)",
								"Ultra (64 steps)", "Custom"
							};
							int volQ = static_cast<int>(m_volumetricQuality);
							if (ImGui::Combo("Quality preset", &volQ, kVolNames, 5)) {
								m_volumetricQuality = static_cast<smvk::VolumetricQuality>(std::clamp(volQ, 0, 4));
								if (m_volumetricQuality != smvk::VolumetricQuality::Custom) {
									smvk::VolumetricDesc qd{};
									smvk::ApplyVolumetricQuality(qd, m_volumetricQuality);
									m_volumetricSteps = (int)qd.steps;
								}
							}
							if (ImGui::SliderInt("March steps", &m_volumetricSteps, 4, 128))
								m_volumetricQuality = smvk::VolumetricQuality::Custom;
							ImGui::SliderFloat("Fog max distance (m)", &m_volumetricMaxDistance, 5.0f, 200.0f);
							// The base world's infinite +/- zone owns the
							// extinction value. The tile bake replaces the pass
							// density with this record; never edit only the pass.
							if (ImGui::SliderFloat("Volumetric density", &se.volumetricDensity,
								0.0f, 0.1f, "%.4f")) {
								authored();
								m_volumetricDensity = se.volumetricDensity;
							}
							if (ImGui::SliderFloat("Scattering intensity", &se.volumetrics,
								0.0f, 4.0f)) {
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
							ImGui::SliderFloat("Dither (banding guard)", &m_volumetricJitter, 0.0f, 1.0f);
							ImGui::SliderFloat("Height falloff (1/m)", &m_volumetricHeightFalloff, 0.0f, 1.0f, "%.3f");
							if (m_volumetricHeightFalloff > 0.0f)
								ImGui::SliderFloat("Height base (m)", &m_volumetricHeightBase, -10.0f, 30.0f);
							ImGui::SliderFloat("GI refresh (m)", &m_volumetricGiRefresh, 0.1f, 4.0f, "%.2f");
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("The baked-GI base refresh interval along the march (was hardcoded 0.75). Ambient radiance is low-frequency - coarser is safe; the GI-volume blend weights stay exact per segment.");
							ImGui::SliderFloat("Field density cadence (m)", &m_volumetricFieldCadence, 0.25f, 4.0f, "%.2f");
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("The world-field fog-density sampling interval: the field is fetched at this spacing and lerped between segments (1 m default - extinction-grade accuracy).");
							ImGui::Separator();
							// Sun occlusion for the fog - the THREE-WAY
							// mode (never combined: multiplying the two
							// visibility forms would darken the fog
							// twice; the screen-space leg composites
							// additively either way).
							{
								const char* fogShadowModes[3] = {
									"Off", "CSM cascades", "Coarse tile (marked casters)"
								};
								int fsm = std::clamp(m_fogSunShadowMode, 0, 2);
								if (ImGui::Combo("Fog sun shadows", &fsm,
										fogShadowModes, 3))
									m_fogSunShadowMode = fsm;
								if (ImGui::IsItemHovered())
									ImGui::SetTooltip(
										"CSM: the march's per-segment cascade tap (PCF mode only - under EVSM the raw atlas is stale).\n"
										"Coarse: the dedicated sun-oriented tile with ONLY Casts-Volumetric-Shadow-marked large occluders - works under EVSM.\n"
										"Mark casters in the editor's node inspector.");
							}
							ImGui::Checkbox("Screen-space god rays (Dishonored)", &m_godraysEnabled);
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("Sun occlusion mask + radial scattering over the scene depth - additive over the CSM leg, and the fog sun-occlusion fallback under EVSM. Debug views: TOURMALINE_GODRAYS_DEBUG=1..4.");
							if (m_godraysEnabled) {
								ImGui::SliderFloat("God rays intensity", &m_godraysIntensity, 0.0f, 2.0f);
								ImGui::SliderInt("God rays samples", &m_godraysSamples, 4, 64);
								ImGui::SliderFloat("God rays decay", &m_godraysDecay, 0.85f, 1.0f, "%.3f");
								ImGui::SliderFloat("God rays glow radius", &m_godraysGlowOuter, 0.02f, 0.4f);
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
							m_particleDemo.selected = std::min(
								std::max(m_particleDemo.selected, 0), emitterCount - 1);
							if (ImGui::BeginCombo("Edit emitter", std::to_string(
								m_particleDemo.selected).c_str())) {
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
							ImGui::SliderFloat("Camera-box wrap (m)", &e.fade.w, 0.0f, 20.0f,
								"%.1f  (0 = world emitter)");
							const char* shapes[] = { "Cone", "Sphere", "Point", "Ground XZ" };
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
							m_particleDemo.emitters.erase(
								m_particleDemo.emitters.begin() + m_particleDemo.selected);
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
							ImGui::SetTooltip("The farthest LOD shades vertex-lit (sun + ambient,\n"
								"no shadows/point lights/specular/textures). F5 shows it.");
						ImGui::Checkbox("Texture streaming (applies to next scene load)", &m_textureStreamingEnabled);
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
							"F1 Lit", "F2 Unlit", "F3 Meshlets",
							"F4 Cascades", "F5 LODs", "F6 Vertex Colors",
							"F9 UV0 (base)", "UV1 (lightmap atlas)",
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
							ViewMode::Lit, ViewMode::Unlit,
							ViewMode::Meshlets, ViewMode::Cascades,
							ViewMode::Lods, ViewMode::VertexColors,
							ViewMode::Uv0, ViewMode::Uv1,
							ViewMode::Cubemap,
							ViewMode::Pom, ViewMode::SsrMask,
							ViewMode::Normals,
							ViewMode::SsgiConfidence, ViewMode::SsgiRaw,
							ViewMode::SsgiLayer, ViewMode::SsgiBaseline,
							ViewMode::SsgiContribution,
							ViewMode::NormalsMapped, ViewMode::MotionVectors,
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
						constexpr int viewCount =
							static_cast<int>(std::size(kComboViews));
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
								&m_shadowClipThreshold, 0.0f, 0.1f, "%.3f");
							ImGui::SliderFloat("Highlight clipping threshold",
								&m_highlightClipThreshold, 0.9f, 1.0f, "%.3f");
						}
						// The HDR SSGI views (raw/layer/baseline/
						// contribution): one SHARED diagnostic exposure so
						// switching views preserves relative brightness
						// (confidence is excluded — its 0..1 read
						// self-normalizes via 1/exposure).
						if (IsSsgiHdrDebugView(m_debugView)) {
							ImGui::SliderFloat("SSGI debug exposure (+EV)",
								&m_ssgiDebugEv, 0.0f, 12.0f, "%.1f");
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
								ImGui::SliderFloat("Motion debug scale", &m_motionDebugScale, 1.0f, 128.0f);
						}
						const char* meshletModes[] = { "Random per meshlet", "Cone-cutoff heat" };
						ImGui::Combo("Meshlet color mode (F3)", &m_debugMeshletMode, meshletModes, 2);
						if (ImGui::Checkbox("Cone axes (line overlay)", &m_debugCones)) {
							// Arm/disarm the GPU leg (SetConeDebug lazily creates
							// the module + the shared line buffer on first arm).
							smvk::gfx::SetConeDebug(m_gfxRenderer.State(),
								*m_context, *m_rendergraph, m_debugCones);
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
							ImGui::SliderFloat("Overlay point size (px)", &m_lightPointSize, 2.0f, 16.0f);
							ImGui::SliderInt("Overlay max labels", &m_maxLightLabels, 0, 1024);
						}
						ImGui::Checkbox("Probe points", &m_giShowProbes);
						if (m_giShowProbes) {
							ImGui::SliderFloat("Probe overlay distance (m)",
								&m_giProbePointMaxDist, 5.0f, 300.0f, "%.0f m");
							const char* probeColorModes[] = {
								"Baked GI (irradiance)",
								"Visibility (blocked/near/open)",
								"Lattice (pre-bake)",
							};
							int pcm = m_giProbeColorMode;
							if (ImGui::Combo("Probe color mode", &pcm,
									probeColorModes, 3))
								m_giProbeColorMode = pcm;
						}
						ImGui::Checkbox("GI volume bounds (wireframe)", &m_giShowVolumeBounds);
						if (m_regLoaded || m_giVolumesLoaded) {
							// All five R / F8 stops (including off).
							// Select Lit so the debug shader is visible.
							const char* dbgNames[5] = { "Off", "Baked irradiance",
								"GI source", "Probe distance",
								"Visibility weight sum (wSum)" };
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
							ImGui::SliderFloat("Overlay max dist (m)", &m_cubemapOverlayMaxDist, 5.0f, 300.0f, "%.0f");
						ImGui::Checkbox("Probe preview (mirror spheres)", &m_cubemapShowBalls);
						{
							static const char* const kCubeDbgNames[3] = {
								"Off", "Cubemap only (no Fresnel)", "Probe weight heat",
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
						bool b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Degenerate) != 0;
						if (ImGui::Checkbox("Cull degenerate", &b))
							m_gfxRenderer.config.cullFlags = b
							? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Degenerate
							: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Degenerate;
						b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Backface) != 0;
						if (ImGui::Checkbox("Cull backface", &b))
							m_gfxRenderer.config.cullFlags = b
							? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Backface
							: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Backface;
						b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Frustum) != 0;
						if (ImGui::Checkbox("Cull frustum", &b))
							m_gfxRenderer.config.cullFlags = b
							? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Frustum
							: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Frustum;
						b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_SmallPrim) != 0;
						if (ImGui::Checkbox("Cull small prims", &b))
							m_gfxRenderer.config.cullFlags = b
							? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_SmallPrim
							: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_SmallPrim;
						b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_FlipWinding) != 0;
						if (ImGui::Checkbox("Flip winding (debug)", &b))
							m_gfxRenderer.config.cullFlags = b
							? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_FlipWinding
							: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_FlipWinding;
						b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_HzbSkipMasked) != 0;
						if (ImGui::Checkbox("Exclude MASK from HZB test", &b))
							m_gfxRenderer.config.cullFlags = b
							? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_HzbSkipMasked
							: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_HzbSkipMasked;
						// HZB occlusion CULL toggle: the occluder-depth fill +
						// the pyramid are a FIXED COST; the flag gates only the
						// filter's occlusion TEST.
						{
							bool occ = (m_gfxRenderer.config.cullFlags
								& smvk::gfx::CullFlagBit_HzbOcclusion) != 0;
							if (ImGui::Checkbox("HZB occlusion cull", &occ))
								m_gfxRenderer.config.cullFlags = occ
									? (m_gfxRenderer.config.cullFlags
										| smvk::gfx::CullFlagBit_HzbOcclusion)
									: (m_gfxRenderer.config.cullFlags
										& ~smvk::gfx::CullFlagBit_HzbOcclusion);
						}
						static bool s_aliasingOn = smvk::TargetHeapAliasingEnabled();
						if (ImGui::Checkbox("Target-heap memory aliasing (next rebuild)", &s_aliasingOn))
							smvk::SetTargetHeapAliasing(s_aliasingOn);
						ImGui::SliderFloat("Occluder min px", &m_hizOccluderMinPixels, 0.0f, 128.0f);
						ImGui::SliderFloat("Occluder min radius m", &m_hizOccluderMinRadius, 0.0f, 5.0f);
						b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Contribution) != 0;
						if (ImGui::Checkbox("Contribution cull (T4)", &b))
							m_gfxRenderer.config.cullFlags = b
							? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Contribution
							: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Contribution;
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
						if (m_dsDebugMode >= DsDebugViz::CountApprox
							&& m_dsDebugMode <= DsDebugViz::CountDiff)
							ImGui::SliderInt("Heat ceiling (lights)",
								&m_dsDebugCountCeil, 1, (int)smvk::kDsMaxLightsPerTile);
						if (lightScoped) {
							const int maxLight = std::max(0, (int)m_dsLightCount - 1);
							ImGui::SliderInt("Debug light (sorted idx)", &m_dsDebugLight,
								0, maxLight);
							if (m_dsDebugLightValid) {
								ImGui::Text("Light %d: %s  range %.1f m", m_dsDebugLight,
									m_dsDebugLightType == (int)smvk::kForwardPlusSpot ? "spot" : "point",
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
							const auto currentPasses = m_swapchain
								? m_swapchain->GetGpuPassTimings() : std::vector<smvk::GpuPassTiming>{};
							static std::vector<std::string> knownPassNames;
							for (const auto& pass : currentPasses) {
								if (std::find(knownPassNames.begin(), knownPassNames.end(), pass.name)
									== knownPassNames.end())
									knownPassNames.push_back(pass.name);
							}
							ImGui::TextDisabled("Rendergraph GPU passes (Optick labels, retired frame)");
							if (knownPassNames.empty()) {
                                ImGui::TextDisabled(m_stageGpuTimingsSupported
                                    ? "Waiting for GPU pass timestamps..."
                                    : "GPU pass timestamps unavailable (cross-queue support required).");
							} else {
								struct PassRow { double ms = 0.0; bool valid = false; };
								std::vector<PassRow> rows(knownPassNames.size());
								for (const auto& pass : currentPasses) {
									if (!pass.valid) continue;
									auto it = std::find(knownPassNames.begin(), knownPassNames.end(), pass.name);
									if (it == knownPassNames.end()) continue;
									auto& row = rows[static_cast<size_t>(it - knownPassNames.begin())];
									row.ms += pass.milliseconds; // repeated Optick names are grouped
									row.valid = true;
								}
								double totalMs = 0.0;
								for (const auto& row : rows)
									if (row.valid) totalMs += std::max(0.0, row.ms);
								// Fixed-height rows; the stacked column uses duration height,
								// independent of label positions.
								constexpr float rowHeight = 24.0f;
								const float plotHeight = rowHeight * static_cast<float>(rows.size());
								const float labelWidth = 230.0f;
								const float plotWidth = 75.0f;
								static constexpr ImU32 colors[] = {
									IM_COL32(105, 140, 190, 230), IM_COL32(74, 157, 148, 230),
									IM_COL32(146, 124, 190, 230), IM_COL32(193, 146, 95, 230),
									IM_COL32(168, 124, 150, 230), IM_COL32(118, 156, 108, 230),
									IM_COL32(100, 152, 170, 230), IM_COL32(177, 150, 105, 230)
								};
								ImGui::BeginChild("##gpu_pass_timeline_scroll",
									ImVec2(0.0f, std::min(420.0f, plotHeight + 8.0f)), false);
								const ImVec2 origin = ImGui::GetCursorScreenPos();
								ImDrawList* draw = ImGui::GetWindowDrawList();
								const ImVec2 plotTop(origin.x + labelWidth + 12.0f, origin.y);
								draw->AddRectFilled(plotTop,
									ImVec2(plotTop.x + plotWidth, plotTop.y + plotHeight),
									IM_COL32(65, 67, 73, 65), 3.0f);
								float cursorY = plotTop.y;
								for (size_t i = 0; i < rows.size(); ++i) {
									// Stable index, deterministic color. Neither changes on frame updates.
									uint32_t hash = 2166136261u;
									for (unsigned char c : knownPassNames[i])
										hash = (hash ^ c) * 16777619u;
									const ImU32 color = colors[hash % (sizeof(colors) / sizeof(colors[0]))];
									const ImVec2 labelTop(origin.x, origin.y + rowHeight * static_cast<float>(i));
									draw->AddCircleFilled(ImVec2(labelTop.x + 4.0f,
										labelTop.y + 8.0f), 3.0f, color);
                                    // Clip long pass names so they cannot paint
                                    // over the independent timeline column.
                                    draw->PushClipRect(labelTop,
                                        ImVec2(labelTop.x + labelWidth - 4.0f,
                                            labelTop.y + rowHeight), true);
                                    draw->AddText(ImVec2(labelTop.x + 12.0f, labelTop.y),
                                        ImGui::GetColorU32(ImGuiCol_Text), knownPassNames[i].c_str());
                                    draw->PopClipRect();
									char value[40];
									if (rows[i].valid)
										snprintf(value, sizeof(value), "%.2f ms", rows[i].ms);
									else
										snprintf(value, sizeof(value), "off");
									draw->AddText(ImVec2(labelTop.x + 12.0f,
										labelTop.y + ImGui::GetTextLineHeight()),
										ImGui::GetColorU32(ImGuiCol_TextDisabled), value);
									if (rows[i].valid && totalMs > 0.0) {
										const float height = static_cast<float>(
											std::max(0.0, rows[i].ms) / totalMs * plotHeight);
										if (height > 0.0f) {
											draw->AddRectFilled(ImVec2(plotTop.x, cursorY),
												ImVec2(plotTop.x + plotWidth, cursorY + height), color);
											cursorY += height;
										}
									}
								}
								ImGui::Dummy(ImVec2(labelWidth + plotWidth + 12.0f, plotHeight));
								ImGui::EndChild();
								ImGui::TextDisabled("Vertical stack = pass durations, not queue chronology.");
								ImGui::TextDisabled("Async passes may overlap; repeated names are grouped.");
							}
						}
						ImGui::Separator();
						{
							const glm::vec3 euler = m_cameraPositioner.getEulerDegrees();
							const glm::quat q = m_cameraPositioner.getOrientation();
							ImGui::Text("Camera yaw %.1f pitch %.1f roll %.1f (deg)", euler.y, euler.x, euler.z);
							ImGui::TextDisabled("quat: %.3f %.3f %.3f %.3f", q.x, q.y, q.z, q.w);
						}
						ImGui::Text("Swapchain %ux%u  framesInFlight=%u",
							fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height, fb.swapchain->framesInFlight());
						ImGui::Text("Scene: %ux%u (%.0f%%) -> swap %ux%u",
							renderExtent.width, renderExtent.height, m_renderScale * 100.0f,
							swapExtent.width, swapExtent.height);
						ImGui::Text("UI pass: %ux%u (%.0f%%)",
							uiExtent.width, uiExtent.height, m_uiScale * 100.0f);
						const smvk::PresentMode active = fb.swapchain->GetActivePresentMode();
						ImGui::TextDisabled("Present: %s%s", smvk::ToString(active),
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
						}
						else {
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
							if (ImGui::SmallButton(m_gpuMemShowAllHeaps ? "heaps: all [-]" : "heaps: details [+]"))
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
										if (m_gpuMemHeaps[k].deviceLocal) { idx = k; break; }
									}
								}
								const auto& h = m_gpuMemHeaps[idx];
								const float hf = h.budgetBytes > 0
									? (float)((double)h.usageBytes / (double)h.budgetBytes) : 0.0f;
								char hu[32], hb[32], hs[32], hov[80];
								fmtMiB(h.usageBytes, hu, sizeof(hu));
								fmtMiB(h.budgetBytes, hb, sizeof(hb));
								fmtMiB(h.heapSizeBytes, hs, sizeof(hs));
								snprintf(hov, sizeof(hov), "heap%zu %s %s/%s %.1f%% (heap %s)",
									idx, h.deviceLocal ? "VRAM" : "host",
									hu, hb, hf * 100.0f, hs);
								ImGui::ProgressBar(hf, ImVec2(-1.0f, 0.0f), hov);
								if (!m_gpuMemShowAllHeaps)
									break;
							}
						}
						else {
							ImGui::TextDisabled("GPU memory: no heap data");
						}
						if (!m_textureStreamer)
							ImGui::TextDisabled("Streaming: off (full-resident)");
					}

					ImGui::SeparatorText("Shadows");
					{
						const auto& atlas = m_viewportResources.directionalShadows;
						ImGui::Text("CSM: %u cascades  tile %ux%u  atlas %ux%u  lambda %.2f  far %.1fm",
							NUM_CASCADES, CascadeTileExtent().width, CascadeTileExtent().height,
							atlas.gridExtent.width, atlas.gridExtent.height,
							(double)m_cascadeLambda, (double)CascadeFar());
						for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
							ImGui::Text(" cascade %u: [%.2f .. %.2f] depthBias %.5f normalBias %.4f",
								i, (double)m_cascadeSplitsDbg[i], (double)m_cascadeSplitsDbg[i + 1],
								(double)m_cascadeBiasDbg[i], (double)m_cascadeNormalBiasDbg[i]);
						}
						if (m_sdsmEnabled && m_sdsmStatsValid) {
							ImGui::TextDisabled("SDSM measured %.2f..%.2f m (pct %.2f..%.2f) -> applied %.2f..%.2f m%s",
								m_sdsmMin, m_sdsmMax, m_sdsmPctMin, m_sdsmPctMax,
								m_sdsmAppliedValid ? m_sdsmAppliedMin : 0.0f,
								m_sdsmAppliedValid ? m_sdsmAppliedMax : 0.0f,
								m_sdsmCutCooldown > 0 ? " (cut cooldown: heuristic)" : "");
						}
						const glm::vec3 toSunDbg = frameEnv.SunDirectionToSun();
						ImGui::Text("sun toSun: %.2f %.2f %.2f  int %.0f lux",
							toSunDbg.x, toSunDbg.y, toSunDbg.z, frameEnv.sunIntensity);
						ImGui::Text("RenderViews: %u allocated (main=%u, CSM @%u, spots @%u, point faces @%u)",
							m_renderViews.Count(), m_renderViews.MainViewIndex(),
							cascadeViewBase, spotViewBase, pointViewBase);
						ImGui::Text("Spot shadows: %u / %u (budget %d)  atlas %ux%u",
							m_spotShadowCount, smvk::kMaxSpotShadows, m_spotShadowMax,
							m_viewportResources.spotShadows.atlasExtent.width,
							m_viewportResources.spotShadows.atlasExtent.height);
						ImGui::Text("Point shadows: %u lights / %u (%u face tiles, 6 each)",
							m_pointShadowLightCount, smvk::kMaxPointShadowLights,
							m_pointShadowTileCount);
						if (!m_viewportResources.spotShadows.Valid())
							ImGui::TextDisabled("Spot atlas: MISSING (spots run unshadowed)");
						ImGui::Separator();
						// Deferred shadows (tiled/zBin): counters are
						// retired-slice peaks (~2 frames old).
						const smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
						if (ds.Valid()) {
							ImGui::Text("Shadow tiles: %ux%u (8px)  TileGroups: %ux%u (64px)",
								ds.tileCountX, ds.tileCountY, ds.groupCountX, ds.groupCountY);
							ImGui::Text("Light list: %u / %u uints (%.1f MB used)",
								m_dsStats.listSlots, smvk::kDsMaxListUInts,
								float(m_dsStats.listSlots) * 4.0f / (1024.0 * 1024.0));
							ImGui::Text("Visibility: %u / %u slots x 64B (%.1f MB used)",
								m_dsStats.visSlots, smvk::kDsMaxVisSlots,
								float(m_dsStats.visSlots) * 64.0f / (1024.0 * 1024.0));
							if (m_dsStats.listSlots >= smvk::kDsMaxListUInts)
								ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
									"Light list SATURATED: tiles clamp (lights drop to fully lit)");
							if (m_dsStats.visSlots >= smvk::kDsMaxVisSlots)
								ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
									"Visibility SATURATED: partial shadows degrade to fully lit");
						}
						else {
							ImGui::TextDisabled("Deferred buffers/shaders missing (extent cap?); pipeline off");
						}
					}

					ImGui::SeparatorText("Post + AA state");
					{
						const auto& bloom = m_viewportResources.bloom;
						if (bloom.Valid() && m_bloomEnabled) {
							ImGui::Text("Bloom: %u mips  base %ux%u  thr %.2f knee %.2f int %.2f",
								bloom.MipCount(),
								bloom.MipExtent(0).width, bloom.MipExtent(0).height,
								bloom.desc.threshold, bloom.desc.knee, bloom.desc.intensity);
						}
						else {
							ImGui::TextDisabled("Bloom: OFF");
						}
						float histPeak = 1.0f;
						for (float v : m_exposureBinsUi)
							histPeak = std::max(histPeak, v);
						ImGui::PlotHistogram("##expHist", m_exposureBinsUi,
							(int)smvk::kExposureBinCount, 0, nullptr, 0.0f, histPeak, ImVec2(0, 48));
						if (m_aaMode == AaMode::Msaa2x || m_aaMode == AaMode::Msaa2xCustom) {
							if (!m_context->SupportsMSAA2x())
								ImGui::TextDisabled("MSAA 2x unsupported - running 1x");
							else
								ImGui::TextDisabled(m_viewportResources.msaa.Valid()
									? (m_aaMode == AaMode::Msaa2xCustom ? "MSAA custom requested (hardware fallback if unavailable)" : "MSAA 2x -> HDR resolve")
									: "MSAA target missing - running 1x");
						}
						else if (m_aaMode == AaMode::Smaa1x || m_aaMode == AaMode::SmaaT2x) {
							if (!m_viewportResources.smaa.Valid())
								ImGui::TextDisabled("SMAA unavailable (shaders/LUTs missing - rebuild?)");
							else if (!(m_passes.motionVectors && m_viewportResources.motionVectors.Valid()))
								ImGui::TextDisabled("SMAA needs the motion-vector pass - disabled");
							else if (m_aaMode == AaMode::SmaaT2x)
								ImGui::TextDisabled("SMAA T2x: jitter + temporal resolve%s [img=%u par=%u]",
									m_smaaFrameParams.historyValid ? "" : " (history warming up)",
									fb.imageIndex, m_smaaFrameParams.parity);
							else
								ImGui::TextDisabled("SMAA 1x: edges -> weights -> neighborhood blend");
						}
						else {
							ImGui::TextDisabled("AA: off");
						}
						{
							const char* dlssModeName[] = { "off", "performance", "balanced",
								"quality", "ultra performance", "ultra quality", "DLAA" };
							const int modeIdx = static_cast<int>(m_dlssMode);
							if (m_dlssFrameActive)
								ImGui::TextDisabled("DLSS %s: HDR %ux%u -> %ux%u%s",
									dlssModeName[std::clamp(modeIdx, 0, 6)],
									renderExtent.width, renderExtent.height,
									swapExtent.width, swapExtent.height,
									m_dlssReset ? " (history reset)" : "");
							else if (DlssRequested())
								ImGui::TextDisabled("DLSS %s: INACTIVE (%s%s)",
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
								m_dofFocusDistance, m_dofFocusRange,
								m_dofHqBokeh ? "HQ bokeh" : "low");
						if (!m_viewportResources.ssr.Valid())
							ImGui::TextDisabled("Reflection target/shaders missing - no reflections");
						else if (!m_viewportResources.ssgi.hzb.Valid())
							ImGui::TextDisabled("SSR: %s %u steps intensity %.2f hzb:MISSING (chain skips)",
								m_ssrEnabled ? "ON" : "off",
								(uint32_t)std::clamp(m_ssrSteps, 1, 128), m_ssrIntensity);
						else
							ImGui::TextDisabled("SSR: %s %u steps intensity %.2f hzb:shared %ux%u %u mips blur:%s",
								m_ssrEnabled ? "ON" : "off",
								(uint32_t)std::clamp(m_ssrSteps, 1, 128), m_ssrIntensity,
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
								ImGui::TextDisabled("SSGI AO: targets up but the shared HZB is missing - chain skips");
							else {
								ImGui::Text("SSGI AO: half %ux%u (full %ux%u) hzb %ux%u (%u mips)",
									ssgi.halfExtent.width, ssgi.halfExtent.height,
									ssgi.fullExtent.width, ssgi.fullExtent.height,
									ssgi.hzb.width, ssgi.hzb.height, ssgi.hzb.mipCount);
							}
						}
						// World env tiles (zones): always on; the readout.
						ImGui::Text("Zones: baked %u x %u tiles @64 m, %u record(s), grid @ (%.0f, %.0f)%s",
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
						ImGui::Text("Cubemaps: %u resident / %u rows (baked %u, procedural %u)  Probes: %u (light tail)",
							(unsigned)m_cubemaps.entries.size(),
							(unsigned)m_cubemapRows.size(),
							(unsigned)bakedCount, (unsigned)procCount,
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
							constexpr uint32_t kTailCap =
								smvk::kForwardPlusMaxProbes;
							const uint32_t n =
								std::min(ProbeEntryCount(), kTailCap);
							smvk::probepremix::NdfCandidate cands[kTailCap];
							const glm::vec3 poi =
								m_cameraPositioner.cameraPosition_;
							uint32_t m = 0;
							for (uint32_t i = 0; i < n; ++i) {
								const CubemapProbeGpu& pr = m_cubemapProbes[i];
								const float influence =
									std::max(pr.posInfluence.w, 0.0f);
								const float fade =
									std::max(pr.fadeCubemap.x, 0.0f);
								const float dist = glm::distance(poi,
									glm::vec3(pr.posInfluence));
								float ndf = 1.0f;
								if (dist <= influence)
									ndf = 0.0f;
								else if (fade > 0.0f
									&& dist <= influence + fade)
									ndf = (dist - influence) / fade;
								if (ndf >= 1.0f)
									continue;
								cands[m].cubemap =
									static_cast<uint32_t>(pr.fadeCubemap.y);
								cands[m].ndf = ndf;
								cands[m].influence = influence;
								++m;
							}
							float w[kTailCap];
							smvk::probepremix::ComputeNdfWeights(cands, m, w,
								kTailCap);
							struct SlotReadout {
								float w; uint32_t cube, i;
							} slots[kTailCap];
							for (uint32_t i = 0; i < m; ++i)
								slots[i] = { w[i], cands[i].cubemap, i };
							std::sort(slots, slots + m,
								[&](const SlotReadout& a, const SlotReadout& b) {
									if (a.w != b.w)
										return a.w > b.w;
									if (cands[a.i].influence
										!= cands[b.i].influence)
										return cands[a.i].influence
											> cands[b.i].influence;
									return a.cube < b.cube;
								});
							const uint32_t k = std::min(m, 4u);
							float sumK = 0.0f;
							for (uint32_t s = 0; s < k; ++s)
								sumK += slots[s].w;
							const float invK = sumK > 1e-6f
								? 1.0f / sumK : 0.0f;
							for (uint32_t s = 0; s < k && invK > 0.0f; ++s) {
								const bool inner =
									cands[slots[s].i].ndf <= 0.0f;
								ImGui::BulletText(
									"slot %u: cubemap %u  w=%.3f  d=%.2f%s",
									(unsigned)s, (unsigned)slots[s].cube,
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
							fwd.tileCountX, fwd.tileCountY, fwd.tileCount,
							FP_TILE_SIZE, fwd.desc.maxLightsPerTile);
						ImGui::Text("Lights: %u / %u%s", m_fpLightCount, FP_MAX_LIGHTS,
							m_fpLightCount > FP_MAX_LIGHTS ? "  (CLAMPED!)" : "");
						ImGui::Text("Decals: %u / %u (cap %u/tile)%s", m_decalFrameCount,
							FP_MAX_DECALS, FP_MAX_DECALS_PER_TILE,
							DecalsActive() ? "" : "  (off)");
						const smvk::gfx::Stats gs = m_gfxRenderer.LastStats();
						// The cull runs in compute; the kept count lands via the
						// async counter-word readback (see UpdateReadbacks).
						const bool gpuKept = m_gfxKeptValid;
						uint32_t kept = gs.clustersProcessed - gs.clustersCulled;
						if (gpuKept)
							kept = std::min(m_gfxKeptBatches, gs.clustersProcessed);
						ImGui::Text("view: %u draws %u cmds %u tris | clusters %u kept %u culled %u%s",
							gs.draws, gs.commands, gs.trianglesProcessed,
							gs.clustersProcessed, kept, gs.clustersProcessed - kept,
							gpuKept ? " (GPU cull)" : "");
						ImGui::Text("materials: %u ranges  batches: %u",
							(unsigned)m_gfxRenderer.LastMaterialRangeCount(),
							(unsigned)m_gfxRenderer.LastBatchCount());
						{
							const bool occOn = (m_gfxRenderer.config.cullFlags
								& smvk::gfx::CullFlagBit_HzbOcclusion) != 0;
							uint32_t hw = 0, hh = 0, hm = 0;
							if (gfxSlot != smvk::RenderViewSet::kInvalidIndex
								&& gfxSlot < m_gfxRenderer.State().temporals.size()) {
								const smvk::gfx::HizResources& hz =
									smvk::gfx::Hiz(m_gfxRenderer.State(), gfxSlot);
								hw = hz.width; hh = hz.height; hm = hz.mipCount;
							}
							const bool depthOk =
								m_viewportResources.hizDepth.bindlessIndex != UINT32_MAX;
							ImGui::Text("hzb: %ux%u/%u mips depthIdx=%s occ=%s cull=%s",
								hw, hh, hm, depthOk ? "ok" : "INVALID", occOn ? "on" : "OFF",
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
							m_lodChainMeshes, m_lodChainLods,
							m_lodEnabled ? "" : "  (OFF - always LOD0)");
						if (m_lodEnabled && m_lodChainMeshes == 0)
							ImGui::TextDisabled("No cooked LOD chain in this scene - cook with --lods on.");
						if (!ShaderLodActive())
							ImGui::TextDisabled("Shader LODs OFF - every level full-lit.");
					}

					ImGui::SeparatorText("Keys");
					ImGui::TextDisabled("[I] %s  [ESC] quit", m_uiModeHint ? "UI (NORMAL) -> press I for FPS" : "FPS (DISABLED) -> press I for UI");
					ImGui::TextDisabled("Keys (toggles): TAB debug panel  M blur  O AO(No/SSAO/SAO)  B bloom  X exposure  V vectors  C map");
					ImGui::TextDisabled("      L LODs  F particles  Ctrl+F freeze cull  G filter  K sky  H shadows  T terrain");
					ImGui::TextDisabled("      J blood splat at crosshair  TOURMALINE_DECALS=0/1");
					ImGui::TextDisabled("Keys (views): N ambient  R/F8 GI view  F1 lit  F2 unlit  F3 meshlets");
					ImGui::TextDisabled("      F4 cascades  F5 LOD colors  F6 vertex colors  F9 UV0  F10 cubemap  F11 POM");
#if defined(DS_DEBUG_VIZ)
					ImGui::TextDisabled("      Y DS visualizer (%s)",
						m_dsDebugMode == DsDebugViz::Off ? "off" : "on");
#endif
					ImGui::TextDisabled("Keys (actions): Z/1-4 AA  P wireframe  F7 capture  ` texture viewer  Ctrl+T stats HUD");
					ImGui::EndTabItem();
				}
                // Export the active cooked-scene camera and replayable settings
                // as a git-tracked stress scenario. This is not displayed by the
                // editor, whose borrowed world has no stable pak entry point.
                if (!m_benchmarkSourceArgs.empty() && ImGui::BeginTabItem("Benchmarks")) {
                    ImGui::SeparatorText("Benchmark scenarios");
                    ImGui::InputText("Label##bench", m_benchmarkLabel,
                        sizeof(m_benchmarkLabel));
                    ImGui::InputText("Subsystem##bench", m_benchmarkSubsystem,
                        sizeof(m_benchmarkSubsystem));
                    ImGui::InputTextMultiline("Notes##bench", m_benchmarkNotes,
                        sizeof(m_benchmarkNotes), ImVec2(-1.0f, 48.0f));
                    if (ImGui::Button("Export for benchmark"))
                        ExportBenchmarkScenario();
                    ImGui::TextDisabled("Saves camera and supported run pins to benchmarks/scenarios.json");
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
