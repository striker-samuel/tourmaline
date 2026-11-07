// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "editor_asset_browser_nav.h" // collection folders belong at root
#include "editor_asset_creation.h"
#include "editor_asset_dcc.h"     // DCC source link + export build stage
#include "editor_asset_inspect.h" // generic file/directory inspector
#include "editor_attr_ui.h"       // the attribute-widget stubs (attr::Bool/...)
#include "editor_brush_presets.h"
#include "modal_bindings.h"
#include "ui/dialogs/system_modal.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/shape_asset.h>
#include <Engine/Profile/profile.h>

#include <Engine/Asset/spriteset.h>
#include <SDL3/SDL_filesystem.h> // SDL_GetBasePath (staged export scripts)
#include <SDL3/SDL_misc.h>       // SDL_OpenURL: launch the local asset history UI
#include <imgui.h>
#include <imgui_impl_vulkan.h> // ImGui_ImplVulkan_AddTexture (TextureThumb)
#include <imgui_internal.h>    // DockBuilder: initial workspace graph + user reset
#include <stb_image.h>         // stbi_load (AssetSidecarThumb; impl in editor_branding)

#include <ktx.h>

#include <cmath>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shellapi.h> // ShellExecuteA (reveal-in-explorer)
#include <windows.h>
#endif

namespace editor {
void EditorApp::DrawLightmapPanel() {
    ImGui::Text("Lightmap bake (.sglm)");
    // Registry slots from the loaded world (first per kind wins, like App).
    if (m_world && !m_world->clusters.empty()) {
        for (const auto& clusterPtr : m_world->clusters) {
            if (!clusterPtr || clusterPtr->giBindings.empty())
                continue;
            for (const smvk::GiBinding& b : clusterPtr->giBindings) {
                const char* kindName = "?";
                switch (b.kind) {
                case smvk::GiBinding::Kind::Probe:
                    kindName = "probe";
                    break;
                case smvk::GiBinding::Kind::Lightmap:
                    kindName = "lightmap";
                    break;
                case smvk::GiBinding::Kind::RegManifest:
                    kindName = "regmanifest";
                    break;
                case smvk::GiBinding::Kind::GiVolumeManifest:
                    kindName = "givolumemanifest";
                    break;
                case smvk::GiBinding::Kind::Cubemap:
                    kindName = "cubemap";
                    break;
                }
                ImGui::TextDisabled("registry: %s '%s'", kindName, b.name.c_str());
            }
            break; // first cluster carrying bindings (matches runtime)
        }
    }
    ImGui::TextWrapped("atlas: %s", m_sglmStatus.c_str());
    if (m_hasSglm) {
        uint32_t l5 = 0, l6 = 0, l9 = 0, l12 = 0;
        uint64_t texels = 0;
        for (size_t i = 0; i < m_sglm.chunks.size() && i < m_sglm.payloads.size(); ++i) {
            const uint32_t l = m_sglm.chunks[i].lobeCount;
            if (l == 5)
                ++l5;
            else if (l == 6)
                ++l6;
            else if (l == 9)
                ++l9;
            else
                ++l12;
            texels += uint64_t(m_sglm.payloads[i].width) * m_sglm.payloads[i].height;
        }
        ImGui::Text("chunks=%zu texels=%llu gpu=%.1f MiB",
                    m_sglm.chunks.size(),
                    (unsigned long long)texels,
                    double(m_sglm.GpuBytes()) / (1024.0 * 1024.0));
        ImGui::Text("lobes: 5:%u 6:%u 9:%u 12:%u", l5, l6, l9, l12);
        // Per-selected-mesh atlas rects (meshKey == cook node id).
        if (m_selected != UINT32_MAX && m_world) {
            uint32_t rects = 0;
            for (const auto& c : m_sglm.chunks)
                if (c.meshKey == m_selected)
                    ++rects;
            ImGui::TextDisabled("selected node %u: %u atlas rects", m_selected, rects);
        }
        ImGui::TextDisabled("F10 = UV1 view (baked atlas uv: chart uv + id, unpacked in VS)");
        if (m_world && ImGui::Button("Baked irradiance view (F8)"))
            ModalViewBaked();
        // Flat lightmap PNG export (whole scene, atlas resolution).
        ImGui::Separator();
        ImGui::Checkbox("Export with UV wireframe", &m_lmExportWires);
        ImGui::SetItemTooltip("Chart shell edges drawn over the export "
                              "(whole scene, atlas uv2 layout).");
        ImGui::Checkbox("Export UV coordinates", &m_lmExportUv);
        ImGui::SetItemTooltip("Render the atlas uv itself (checker + "
                              "gradient + stretch glow) instead of the baked content — "
                              "the QA view of the parameterization.");
        if (!m_lmGpu)
            ImGui::BeginDisabled();
        if (ImGui::Button(m_lmExportUv ? "Export UV coords PNG" : "Export lightmap PNG")) {
            m_lmExportQueued = true;
            Log("Lightmap export queued: whole scene unwrapped by atlas "
                "uv2 (" +
                std::string(m_lmExportUv ? "uv coords" : "baked content") +
                std::string(m_lmExportWires ? " + wireframe" : "") + ") -> exports/<stem>.png");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetItemTooltip(m_lmExportUv
                                      ? "Whole scene unwrapped by its atlas uv2, uv rendered "
                                        "as color, saved at atlas resolution."
                                      : "Whole scene unwrapped by its atlas uv2 "
                                        "over the baked content, saved at atlas resolution.");
        if (!m_lmGpu) {
            ImGui::EndDisabled();
            ImGui::TextDisabled("export needs the atlas GPU layers "
                                "(open the baked pak)");
        }
    }
    if (ImGui::InputText(
            "sglm path", m_sglmBuf, sizeof(m_sglmBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (m_sglmBuf[0] != '\0')
            LoadSglm(m_sglmBuf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load sglm") && m_sglmBuf[0] != '\0')
        LoadSglm(m_sglmBuf);
}

void EditorApp::DrawTexturePanel() {
    ImGui::Text("Textures");
    if (!m_world || m_world->scene.hierarchy.empty()) {
        ImGui::TextDisabled("No scene loaded.");
        return;
    }
    int mode = int(m_texMode);
    if (ImGui::Combo(
            "Mode", &mode, "Streaming (distance demand)\0Coarsest mip only\0Full (reimport)\0")) {
        m_texMode = TexMode(mode);
        ApplyTexMode(); // reimports the glTF scene in the new mode
    }
    if (!m_textureStreamer) {
        ImGui::TextDisabled("full-resident (no streaming).");
        return;
    }
    if (m_texMode == TexMode::Coarse)
        ImGui::TextDisabled("floor: single coarsest mip (flat colors).");
    const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
    ImGui::Text("%u tex  %.1f/%.1f MiB  full-res %u",
                stats.textureCount,
                double(stats.residentBytes) / (1024.0 * 1024.0),
                double(stats.fullBytes) / (1024.0 * 1024.0),
                stats.fullResidentCount);
    if (ImGui::Button("Dump residency"))
        DumpStreamerStats();
}

void EditorApp::DrawConsolePanel() {
    // Selectable log: readonly multiline text (mouse-drag selection +
    // Ctrl+C) with Clear/Copy buttons and stick-to-bottom autoscroll.
    if (ImGui::SmallButton("Clear"))
        m_consoleLines.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy all")) {
        std::string all;
        size_t total = 0;
        for (auto& l : m_consoleLines)
            total += l.size() + 1;
        all.reserve(total);
        for (auto& l : m_consoleLines) {
            all += l;
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines (drag to select, Ctrl+C to copy)", m_consoleLines.size());
    std::string all;
    {
        size_t total = 0;
        for (auto& l : m_consoleLines)
            total += l.size() + 1;
        all.reserve(total + 1);
        for (auto& l : m_consoleLines) {
            all += l;
            all += '\n';
        }
    }
    // ImGui needs a mutable char buffer for InputTextMultiline; the
    // console caps at 400 lines so rebuilding each frame is cheap.
    static std::vector<char> buf;
    buf.assign(all.begin(), all.end());
    buf.push_back('\0');
    ImGui::BeginChild("##logscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::InputTextMultiline("##consoletext",
                              buf.data(),
                              buf.size(),
                              ImVec2(-1, -1),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo);
    // Stick to bottom while new lines arrive (unless the user scrolled up).
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ---- Images tab (scene image definitions, Maya file nodes) -------------
// Image declarations OUTSIDE the DAG (like materials): a path +
// sampling settings each. A material that uses one mints the cluster
// texture resource at use time; until then these are authoring state.
// Persisted as img.* editorSettings (legacy) AND cooked fileNode records
// (path + colorspace + sampler intent — the cooker reads them for KTX
// conversion); fileNodes win on load.
void EditorApp::DrawImagesTab() {
    if (ImGui::Button("Add image...")) {
        std::string picked;
        if (NativeOpenFile(picked, true) && !picked.empty())
            AddImageDef(picked);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu image(s) — outside the DAG, editor-only", m_images.size());
    if (m_images.empty()) {
        ImGui::TextDisabled("Create > Image... adds one; a material that");
        ImGui::TextDisabled("uses an image mints its cluster texture.");
        return;
    }
    int pendingRemove = -1;
    ImGui::BeginChild("##imglist", ImVec2(0, 0), false);
    for (size_t i = 0; i < m_images.size(); ++i) {
        ImageDef& img = m_images[i];
        ImGui::PushID(int(i));
        ImGui::Separator();
        if (m_imageFocus == i) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.25f, 1.0f));
            ImGui::TextUnformatted(">> outliner selection");
            ImGui::PopStyleColor();
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::SetNextItemWidth(-120.0f);
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", img.name.c_str());
        if (ImGui::InputText("##imgname", nameBuf, sizeof(nameBuf))) {
            img.name = nameBuf;
            AutosaveTour();
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove"))
            pendingRemove = int(i);
        {
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", img.path.c_str());
            ImGui::SetNextItemWidth(-120.0f);
            ImGui::InputText("##imgpath", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                std::string picked;
                if (NativeOpenFile(picked, true) && !picked.empty() && picked != img.path) {
                    img.path = picked;
                    AutosaveTour();
                }
            }
        }
        static const char* kWrapNames[] = {"repeat", "clamp edge", "mirrored repeat"};
        static const char* kCsNames[] = {"sRGB", "linear"};
        static const char* kFilterNames[] = {"linear", "nearest"};
        ImGui::SetNextItemWidth(-4.0f);
        int w = img.wrap;
        if (ImGui::Combo("Tiling (wrap)##imgwrap", &w, kWrapNames, 3) && w != img.wrap) {
            img.wrap = w;
            AutosaveTour();
        }
        ImGui::SetNextItemWidth(-4.0f);
        int cs = img.colorspace;
        if (ImGui::Combo("Colorspace##imgcs", &cs, kCsNames, 2) && cs != img.colorspace) {
            img.colorspace = cs;
            AutosaveTour();
        }
        ImGui::SetNextItemWidth(-4.0f);
        int f = img.filter;
        if (ImGui::Combo("Filter##imgfilter", &f, kFilterNames, 2) && f != img.filter) {
            img.filter = f;
            AutosaveTour();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (pendingRemove >= 0)
        RemoveImageDefAt(size_t(pendingRemove));
}

void EditorApp::DrawInsetTag() {
    // (Viewport legend removed — overlay window, menu item and chip
    // button deleted. The per-view refresh combos moved onto the
    // viewport chips, next to the per-panel view-mode combo.)

    // UV inset tag (top-left of the side strip, inside the viewport).
    if (SecondViewActive() && m_vpLayout == 0) {
        uint32_t stripX = 0;
        InsetStrip(m_vp, stripX);
        ImGui::SetNextWindowPos(ImVec2(float(stripX) + 8, m_menuH + 8), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(1.0f); // fully opaque UI
        if (ImGui::Begin("##insetlabel",
                         nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs)) {
            ImGui::Text("%s (F5)", SecondViewName(m_secondView));
        }
        ImGui::End();
    }
}

void EditorApp::DrawRecoverDialog() {
    // Autosave recovery is NEVER silent: a newer (or orphan) autosave
    // arms this modal at load; the user picks the document explicitly.
    if (!m_recoverArmed)
        return;
    ui::PrepareSystemModal();
    if (!ImGui::BeginPopupModal(
            "Recover autosave", nullptr, ui::kSystemModalFlags | ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (ui::SystemModalCloseRequested()) {
        // Preserve the autosave for a later recovery prompt.
        m_recoverArmed = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("An autosave is newer than the tour file.");
    ImGui::TextDisabled("tour: %s", m_recoverTour.c_str());
    ImGui::TextDisabled("autosave (%s): %s",
                        m_recoverStamp.empty() ? "unknown time" : m_recoverStamp.c_str(),
                        m_recoverAutosave.c_str());
    if (ImGui::Button("Recover autosave")) {
        smvk::tour::Doc doc;
        std::string err;
        std::vector<std::string> warns;
        if (m_world && smvk::tour::LoadTour(m_recoverAutosave, doc, err, &warns)) {
            for (auto& w : warns)
                Log(w);
            if (ReplayTourDoc(doc, m_gltfPath)) {
                Log("Recovered from autosave: " + m_recoverAutosave);
                // The content is live now (and the next edit
                // re-autosaves): the stale file goes away.
                std::error_code ec;
                std::filesystem::remove(std::filesystem::path(m_recoverAutosave), ec);
            } else {
                Log("Autosave recovery failed (kept the tour file).");
            }
        } else {
            Log("Autosave recovery failed: " + err);
        }
        m_recoverArmed = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep tour file")) {
        // The stale autosave stays on disk: it will offer again on the
        // next open (until an explicit save clears it).
        Log("Kept the tour file; the autosave stays pending.");
        m_recoverArmed = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::DrawCreateTerrainDialog() {
    ui::PrepareSystemModal(ImVec2(520, 420));
    if (!ImGui::BeginPopupModal("Create Terrain", nullptr, ui::kSystemModalFlags))
        return;
    if (ui::SystemModalCloseRequested()) {
        m_showTerrainDialog = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("Create a procedural terrain mesh (heightfield grid).");
    ImGui::Separator();
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::InputText("Name",
                         m_terrainNameBuf,
                         sizeof(m_terrainNameBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        ;
    ImGui::DragFloat3("Origin (x, 0, z)", m_terrainOrigin, 0.5f);
    ImGui::DragFloat("Size (m)", &m_terrainSize, 0.5f, 1.0f, 1024.0f);
    int res = m_terrainRes;
    if (ImGui::DragInt("Resolution (samples/side)", &res, 1.0f, 8, 512))
        m_terrainRes = res;
    ImGui::DragFloat("Hill height (m)", &m_terrainHeight, 0.1f, 0.0f, 128.0f);
    ImGui::DragFloat("Hill frequency", &m_terrainFreq, 0.002f, 0.0005f, 1.0f, "%.4f");
    int seed = m_terrainSeed;
    if (ImGui::DragInt("Noise seed", &seed, 1, 1, 1000000))
        m_terrainSeed = seed;
    int tX = m_terrainTilesX, tZ = m_terrainTilesZ;
    if (ImGui::DragInt("Tiles X", &tX, 1.0f, 1, 16, "%.0f"))
        m_terrainTilesX = tX;
    if (ImGui::DragInt("Tiles Z", &tZ, 1.0f, 1, 16, "%.0f"))
        m_terrainTilesZ = tZ;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tile split: each tile is its own mesh node "
                          "(per-tile materials/texture layers); edges stay continuous.");
    // Material combo over the live world materials; the editor default
    // (editor://default) is the fallback and the dialog's default pick.
    if (m_world && !m_world->materialLocations.empty()) {
        const uint32_t def = EnsureDefaultMaterial();
        if (m_terrainMat == UINT32_MAX || m_terrainMat >= m_world->materialLocations.size())
            m_terrainMat = def != UINT32_MAX ? def : 0;
        std::string preview = "mat." + std::to_string(m_terrainMat);
        if (auto* M = m_world->GetMaterial(m_terrainMat)) {
            const auto& loc = m_world->materialLocations[m_terrainMat];
            const std::string uri =
                loc.cluster ? std::string(loc.cluster->GetMaterialUri(loc.handle).Str()) : "";
            preview += " " + uri;
            (void)M;
        }
        if (ImGui::BeginCombo("Material", preview.c_str())) {
            for (uint32_t i = 0; i < m_world->materialLocations.size(); ++i) {
                const auto& loc = m_world->materialLocations[i];
                std::string uri =
                    loc.cluster ? std::string(loc.cluster->GetMaterialUri(loc.handle).Str()) : "";
                char item[320];
                std::snprintf(
                    item, sizeof(item), "%u: %s", i, uri.empty() ? "(unnamed)" : uri.c_str());
                if (ImGui::Selectable(item, m_terrainMat == i))
                    m_terrainMat = i;
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("Material: (scene has no materials)");
        m_terrainMat = UINT32_MAX;
    }
    ImGui::Separator();
    if (ImGui::Button("Create")) {
        TerrainDraft d{};
        d.name = m_terrainNameBuf;
        d.origin = glm::vec3(m_terrainOrigin[0], m_terrainOrigin[1], m_terrainOrigin[2]);
        d.size = m_terrainSize;
        d.res = uint32_t(m_terrainRes);
        d.height = m_terrainHeight;
        d.freq = m_terrainFreq;
        d.seed = m_terrainSeed;
        d.tilesX = uint32_t(m_terrainTilesX);
        d.tilesZ = uint32_t(m_terrainTilesZ);
        d.material = m_terrainMat;
        DraftCreateTerrain(d);
        m_showTerrainDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_showTerrainDialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::DrawCookWindow() {
    if (!m_showCookWindow)
        return;
    ImGui::SetNextWindowSize(ImVec2(470, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Cook & bake", &m_showCookWindow))
        DrawCookPanel();
    ImGui::End();
}

// ---- WYSIWYG live view (Lit or F8 baked irradiance) --------------
// The engine renders Lit in viewport 0 by default and F8 baked GI
// in the chosen viewport. Other views keep the editor-only passes.
// The GPU composite pass
// (RecordLiveViewComposePass, editor_render.cpp) is recorded AFTER
// the imgui pass and over-draws the captured rect — so nothing
// ImGui-side can live inside it while the composite runs. This draws
// only the rect capture (the ACTIVE viewport's rect, inset 3px so
// the chrome border survives the composite's edge) + the
// unavailable hint INSIDE the rect (visible only while there is no
// composite to cover it — no LIVE badge, no toggle: a panel like the
// others).
void EditorApp::DrawLiveViewStage() {
    // A docked Viewport is not necessarily the selected dock tab.
    // Never composite stale content over whichever tool tab replaced it.
    if (m_uiMode && !m_dockViewportVisible)
        return;
    const int liveVp = LiveViewViewport();
    if (liveVp < 0)
        return;
    // DETACHED STAGE: the Viewport window lives in a secondary OS
    // window, so ComputeViewportRects collapsed the main-window
    // rects to 1x1 (the editor passes cannot record there). Derive
    // the capture from the RAW dock stage through the shared layout
    // authority (StageViewportRect) so the drive keeps the true
    // extent and aspect. Only the SIZE is consumed (the extent
    // settle + the deadband); the desktop position is irrelevant
    // because the main-swapchain compose is skipped (its detached
    // guard in RecordLiveViewComposePass) - the detached window
    // presents the scene through the ImGui image lane instead.
    if (m_dockStageDetached) {
        const ViewportRect stage = StageViewportRect(m_dockViewportRect, liveVp, m_vpLayout);
        if (stage.w < 8.0f || stage.h < 8.0f)
            return;
        m_wysiwygLiveRectMin = ImVec2(stage.x, stage.y);
        m_wysiwygLiveRectMax = ImVec2(stage.x + stage.w, stage.y + stage.h);
        m_wysiwygLiveRectFresh = true;
        return; // the detached lane draws the image in the panel
    }
    const ViewportRect& stage = m_viewports[liveVp].rect;
    if (stage.w < 8.0f || stage.h < 8.0f)
        return;
    // The capture: THE FULL stage rect — NO INSET. The first cut
    // inset 3px to preserve the chrome border stroke, but the inline
    // target then rendered at (w-6)/(h-6) while the editor preview
    // renders w/h — a wider aspect at the same fovY, slightly more
    // horizontal world: "lit almost matches but is not identical to
    // unlit". The live view's extent and the preview's rect are now
    // THE SAME pixels → the same aspect → the identical camera. (The
    // chrome border's stroke straddles the rect edge; the composite
    // covers its inner half — the outer half still reads.)
    // ViewportRect is SDL-window-local, while the ImGui compositor
    // uses desktop/global screen coordinates. Convert exactly once
    // before recording the live composite destination.
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    m_wysiwygLiveRectMin = ImVec2(origin.x + stage.x, origin.y + stage.y);
    m_wysiwygLiveRectMax = ImVec2(origin.x + stage.x + stage.w, origin.y + stage.y + stage.h);
    m_wysiwygLiveRectFresh = true;
    // The placeholder hint only reads while the renderer is
    // unavailable (attach failed / no scene): the composite paints
    // over the whole rect once it runs.
    if (!(m_wysiwyg && m_wysiwygValid)) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddText(ImVec2(origin.x + stage.x + 14.0f, origin.y + stage.y + 14.0f),
                    IM_COL32(160, 160, 170, 255),
                    "Lit: %s",
                    m_world ? "renderer unavailable (attach failed - see the console)"
                            : "no scene loaded");
    }
}
// ---- GI probe viz window (View menu; editor_app.h decl) -----------
// The GI-authoring legend + control center: every probe-overlay
// switch in one place, the size/distance knobs, and the counts the
// user reasons with while placing probes — "how many am I
// authoring, here or anywhere in the level":
//   - Cubemap probes: manifest rows in the LEVEL, authored pins,
//     how many are in view, plus the camera-distance color legend.
//   - .sggi lattice: total probes, then the per-OCTREE-LEVEL
//     breakdown (probe count, effective cell size, lookup cells and
//     max probes per cell — the chunk/cluster density answer), each
//     row tagged with the level's ramp swatch.
// Read-live like the overlays (the bake manifest + the .sggi
// cache are queried per frame — the window can never disagree with
// what the viewports draw). LIGHTING mode only: the overlays it
// documents are LIGHTING-gated too.
void EditorApp::DrawGiVizWindow() {
    if (!m_showGiVizWindow || m_mode != EditorMode::Lighting)
        return;
    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GI probe viz", &m_showGiVizWindow))
        return;
    // A 12px color swatch before a text line (the legend rows):
    // drawlist rect + a dummy advance so the swatch rides IN the
    // layout flow instead of fighting SameLine offsets.
    const auto legendSwatch = [](ImU32 col) {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        wdl->AddRectFilled(p, ImVec2(p.x + 12.0f, p.y + 12.0f), col);
        ImGui::Dummy(ImVec2(16.0f, 14.0f));
        ImGui::SameLine();
    };

    // ---- marker switches ----
    // (The gizmo-family toggles — capture markers, light gizmos,
    // GI volume boxes — stay View-menu-only; this window owns the
    // probe/density cluster, no overlap.)
    ImGui::Checkbox("Cubemap probe overlay", &m_showProbeOverlay);
    ImGui::Checkbox("Octahedra markers (all probe sets)", &m_showProbeOctahedra);
    ImGui::TextDisabled("Selected capture: 3D influence guide (viewport)");
    ImGui::Checkbox("C<idx>/P<idx> labels", &m_probeLabels);
    ImGui::SliderFloat("Marker size", &m_probeMarkerSize, 1.0f, 12.0f, "%.1f px");
    ImGui::SliderFloat("Probe overlay distance", &m_probeOverlayMaxDist, 5.0f, 300.0f, "%.0f m");

    // ---- density preview toggles (same flags as the GI panel) ----
    ImGui::Separator();
    ImGui::TextUnformatted("Bake density previews (no baker run)");
    if (ImGui::Checkbox("Cubemap density preview", &m_cubemapDensityOn) && m_cubemapDensityOn &&
        m_cubemapDensity.empty())
        RefreshCubemapDensity();

    // ---- legend: cubemap probes ----
    ImGui::Separator();
    ImGui::TextUnformatted("Cubemap probes (bake manifest)");
    std::vector<CubemapProbeRow> rows;
    CubemapProbeRowsFromFile(rows);
    ImGui::Text("In level: %zu    in view: %u (cap 64)", rows.size(), m_probeDrawnLast);
    if (m_world) {
        ImGui::Text("Authored pins: %zu node(s)", m_world->scene.tables.cubemaps.size());
    }
    legendSwatch(IM_COL32(80, 255, 120, 230));
    ImGui::TextUnformatted("camera inside influence (full weight)");
    legendSwatch(IM_COL32(255, 220, 80, 230));
    ImGui::TextUnformatted("camera inside fade ring (blending)");
    legendSwatch(IM_COL32(120, 140, 160, 160));
    ImGui::TextUnformatted("beyond fade (probe idle for this view)");
    legendSwatch(IM_COL32(0, 0, 0, 255));
    ImGui::TextUnformatted("authored pin, no baked probe behind it (black ball)");

    ImGui::End();
}

void EditorApp::DrawCookPanel() {
    ImGui::Text("Export the current scene + tour edits to a cooked .pak.");
    ImGui::Separator();

    // Labels ride their OWN lines and fields take the full width —
    // shared label/widget lines collide in narrow windows.
    ImGui::Text("Output .pak path (relative to content/ or absolute):");
    {
        const float browseW = 90.0f;
        ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - browseW));
        ImGui::InputText("##outPak", m_cookOutPakBuf, sizeof(m_cookOutPakBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            std::string picked;
            if (NativeSaveFile(picked) && !picked.empty()) {
                std::snprintf(m_cookOutPakBuf, sizeof(m_cookOutPakBuf), "%s", picked.c_str());
            }
        }
    }

    // Bake/export knobs live on the scene's exportSettings NODE now
    // (authored in the Inspector — select it below; persisted as
    // createNode -at exportSettings + setExportAttr tour lines and
    // read by Cook.exe/Pak.exe off the replayed scene).
    ImGui::Separator();
    {
        const uint32_t esNode = FindExportSettingsNode();
        if (esNode != UINT32_MAX) {
            ImGui::Text("Export settings: node '%s' (#%u)",
                        getNodeName(m_world->scene, int(esNode)).c_str(),
                        esNode);
            if (ImGui::Button("Select ExportSettings node")) {
                SetSelection(esNode);
                ImGui::SetWindowFocus("Inspector");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Quality/GI knobs (meshopt, LODs,\n"
                                  "lightmaps, cubemaps, meshlets, ignoreVfs...) are\n"
                                  "edited on that node's Inspector page.");
        } else {
            ImGui::TextDisabled("No exportSettings node in this scene.");
            if (ImGui::Button("Create ExportSettings node"))
                EnsureExportSettingsNode();
        }
    }

    ImGui::Separator();
    ImGui::Text("GI (probes) - baked ON DEMAND, never with the cook");
    ImGui::TextDisabled("%u GI volume node(s) authored in this scene",
                        m_world ? uint32_t(m_world->scene.tables.giVolumes.size()) : 0u);
    ImGui::TextDisabled("Bake GI uses the authored scene volumes; "
                        "no ExportSettings node or prior cook is required.");
    if (!m_giBakedHash.empty()) {
        const bool fresh = !m_cookDefHash.empty() && m_giBakedHash == m_cookDefHash;
        ImGui::TextDisabled("GI baked (%s)",
                            fresh ? "bakes fresh" : "may predate this cook — rebake to refresh");
    }
    if (m_bakeRunning) {
        ImGui::Text("Baking GI... (detail in the task window)");
        ImGui::ProgressBar(m_taskWindow.progress, ImVec2(-1, 0), ""); // current bake phase
    } else if (ImGui::Button("Bake GI now")) {
        BakeGI(); // only a loaded project scene with GI volumes is needed
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run GiBaker over the current definition tour.\n"
                          "Bake files land next to the cook's out pak; the live view\n"
                          "rebinds itself on completion. Cooks stay GI-free.");

    ImGui::Separator();
    ImGui::Text("Octree precook (in-process overlay, no baker run)");
    size_t dirtyTiles = 0;
    for (const OctreeTile& t : m_octreeTiles)
        if (t.dirty)
            ++dirtyTiles;
    ImGui::TextDisabled("%zu tile(s), %zu dirty", m_octreeTiles.size(), dirtyTiles);
    ImGui::Checkbox("Show precook overlay", &m_octreePreviewOn);
    ImGui::Checkbox("Auto-update on edit/select", &m_octreeAutoUpdate);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Recompute the tile grid when the scene changes\n"
                          "and refresh when a capture below is clicked.");
    float tileSize = m_octreeTileSize;
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat("Tile size (m)", &tileSize, 0.5f, 1.0f, 32.0f)) {
        m_octreeTileSize = std::clamp(tileSize, 1.0f, 32.0f);
        if (m_octreeAutoUpdate)
            UpdateOctreePreview();
        else
            MarkOctreeDirty();
    }
    if (ImGui::Button("Update precook")) {
        UpdateOctreePreview();
    }
    ImGui::SameLine();
    if (ImGui::Button("New .cubemap")) {
        CreateCubemapAsset(m_assetsDir);
    }
    ImGui::SameLine();
    if (ImGui::Button("New .givolume")) {
        CreateGivolumeAsset(m_assetsDir, m_selected != UINT32_MAX ? m_selected : UINT32_MAX);
    }
    // Clickable captures: selecting one also refreshes the overlay
    // when auto-update is on (the "automatic update" half). The census
    // shares the overlay's match, so the readout and the balls agree —
    // black balls with "0/N baked" means no baked cubemap is loaded for
    // these pins (bake + rebind first), not a display bug.
    if (m_world && !m_world->scene.tables.cubemaps.empty()) {
        size_t bakedCount = 0, totalCount = 0;
        CaptureBakeState(bakedCount, totalCount);
        ImGui::TextDisabled("captures (click to select): %zu/%zu baked", bakedCount, totalCount);
        int shown = 0;
        for (const CubemapRow& _row : m_world->scene.tables.cubemaps) {
            const NodeId node = _row.node;
            const auto& def = _row.cubemap;
            if (shown++ >= 12)
                break;
            (void)def;
            char btn[128];
            std::snprintf(btn,
                          sizeof(btn),
                          "%s (#%u)##cube%u",
                          getNodeName(m_world->scene, int(node)).c_str(),
                          node,
                          node);
            if (ImGui::SmallButton(btn)) {
                SetSelection(node);
                if (m_octreeAutoUpdate)
                    UpdateOctreePreview();
            }
            if (shown % 3 != 0)
                ImGui::SameLine();
        }
    }
    // On-demand cubemap refresh (capture-only quick bake, GiBaker
    // skipped) + the live face strip below: finished captures
    // appear as KTX-decoded face strips while the cook runs.
    ImGui::Separator();
    ImGui::Text("Bake density (predictive overlays, no baker run)");
    if (ImGui::Checkbox("Cubemap density preview", &m_cubemapDensityOn) && m_cubemapDensityOn &&
        m_cubemapDensity.empty())
        RefreshCubemapDensity();
    ImGui::SameLine();
    if (ImGui::Button("Refresh prediction")) {
        RefreshCubemapDensity();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rebuild the BVH + rerun placement from the\n"
                          "cook dialog's cubemap octree knobs (8 m grid mirror\n"
                          "when the octree is off). Manual like 'Update precook'.");
    ImGui::TextDisabled("octree max %.1f / min %.1f / depth %u%s",
                        ExportCfg().cubemapOctreeMax,
                        ExportCfg().cubemapOctreeMin,
                        ExportCfg().cubemapOctreeDepth,
                        ExportCfg().cubemapOctree ? "" : " (octree OFF: 8 m grid mirror)");
    ImGui::TextDisabled(
        "%s", m_cubemapDensityInfo.empty() ? "no prediction yet" : m_cubemapDensityInfo.c_str());
    if (m_cookRunning && m_cubeRunActive && m_cubeTotal > 0) {
        ImGui::Text("Cubemaps %d/%d captures...", m_cubeDone, m_cubeTotal);
        ImGui::ProgressBar(float(m_cubeDone) / float(m_cubeTotal), ImVec2(-1, 0), "");
    } else if (!m_cookRunning && !m_bakeRunning) {
        if (ImGui::Button("Update cubemaps now")) {
            CookCubemapsNow();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cubemap-only quick bake (GiBaker skipped): re-traces\n"
                              "every capture in the set; faces land below as each\n"
                              "capture finishes. Loose bakes override the pak\n"
                              "through the VFS overlay.");
    }
    DrawCubeFaceStrip();

    ImGui::Separator();
    ImGui::TextDisabled("Cook = PRODUCT step: a .pak for the game/App "
                        "(Run in App loads it). The editor is unaffected by cooks.");
    if (m_cookRunning) {
        ImGui::Text("Cooking... (detail in the task window)");
        ImGui::ProgressBar(-1.0f, ImVec2(-1, 0), "Cooking..."); // indeterminate
    } else {
        if (m_cookJustFinished) {
            ImGui::TextColored(
                m_cookLastOk ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                "%s",
                m_cookLastOk ? "Last cook finished OK." : "Last cook FAILED (see Console).");
        }
        if (ImGui::Button("Cook")) {
            m_cook.outPak = m_cookOutPakBuf;
            CookToPak();
        }
        ImGui::SameLine();
        if (ImGui::Button("Update bakes (no pak)")) {
            m_cook.outPak = m_cookOutPakBuf;
            CookToPak(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Quick refresh, no pak write: GiBaker\n"
                              "re-runs the GI bakes in place, then Cook\n"
                              "--quick-bakes re-captures the cubemap set as\n"
                              "loose bake files. The last pak stays untouched.");
    }
}

// ---- Material editor (right-dock tab) ---------------------------------
// Texture-centric material authoring: pick a material, assign its PBR
// texture slots from the cluster texture registry, manage layers, and add
// textures as FILE PATH refs (KTX2 loads into the pool with the path as
// the URI — cook re-embeds the stashed bytes). Deeper per-material pages
// (emissive/alpha/flags/travel) live in the material focus page.
// Texture thumbnail bindings (the material page's previews; see the
// header block). The sampler is shared with the cubemap face strip
// (created there once — the first user creates it).

} // namespace editor
