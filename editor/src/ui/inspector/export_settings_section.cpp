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
void EditorApp::DrawExportSettingsSection(uint32_t node) {
    const attr::ScopedID componentId("export_settings");
    // The scene's exportSettings node: the WorldSettings-style EXPORT
    // CONFIG page (cook knobs). Every edit is an undoable SetExportAttr
    // command persisted as a setExportAttr tour line; Cook.exe/Pak.exe
    // read the replayed scene (CLI flags still win). outPak/contentDir
    // stay session-only in the Cook panel (never scene state).
    if (!m_world || node >= m_world->scene.hierarchy.size())
        return;
    Scene& scene = m_world->scene;
    auto it = FindExport(scene, node);
    if (!it)
        return;
    ImGui::Separator();
    ImGui::Text("Export settings: '%s'", getNodeName(scene, int(node)).c_str());
    ImGui::TextDisabled("cook/export knobs (createNode -at "
                        "exportSettings + setExportAttr; CLI flags still win)");

    auto beginDrag = [&](const char* attr) {
        if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetExportAttr ||
                                 m_pending.cmd.node != node || m_pending.cmd.exportAttr != attr))
            FinalizePending();
        if (!m_pending.active) {
            m_pending.active = true;
            m_pending.cmd = EditCmd{};
            m_pending.cmd.verb = EditVerb::SetExportAttr;
            m_pending.cmd.node = node;
            m_pending.cmd.exportAttr = attr;
            ReadNodeExportAttr(node,
                               attr,
                               m_pending.cmd.beforeNum,
                               m_pending.cmd.beforeFlag,
                               m_pending.cmd.beforeText);
        }
    };
    auto endDrag = [&](const char* attr) {
        if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
            m_pending.cmd.verb == EditVerb::SetExportAttr && m_pending.cmd.node == node &&
            m_pending.cmd.exportAttr == attr)
            FinishDrag();
    };
    // The row layer: every export row rides the attr stubs (name
    // left, value right - the panel-wide convention). endDrag's
    // IsItemDeactivatedAfterEdit still sees the drag as the last
    // item (the stubs add nothing after the widget).
    auto setBool = [&](const char* label, const char* attr, bool v, const char* tip) {
        bool val = v;
        if (attr::Check(label, &val, tip))
            PushExportAttr(node, attr, 0.0, val, "");
    };
    auto dragInt = [&](const char* label, const char* attr, int v, float speed, int lo, int hi) {
        (void)speed; // the stub's drag step is 1 (every caller's)
        int val = v;
        if (attr::Int(label, &val, lo, hi, nullptr)) {
            val = std::clamp(val, lo, hi);
            beginDrag(attr);
            WriteNodeExportAttr(node, attr, double(val), false, {});
            m_pending.cmd.number = double(val);
        }
        endDrag(attr);
    };
    auto dragFloat = [&](const char* label,
                         const char* attr,
                         float v,
                         float speed,
                         float lo,
                         float hi,
                         const char* fmt) {
        float val = v;
        if (attr::Float(label, &val, speed, lo, hi, fmt, nullptr)) {
            val = std::clamp(val, lo, hi);
            beginDrag(attr);
            WriteNodeExportAttr(node, attr, double(val), false, {});
            m_pending.cmd.number = double(val);
        }
        endDrag(attr);
    };

    const SceneExportSettings& es = it->settings;
    setBool("MeshOpt (meshoptimizer)",
            "meshopt",
            es.meshopt,
            "Reorder vertices/indices for GPU cache efficiency.");
    setBool("Generate LODs", "lods", es.lods, "Build mesh LOD chains via meshoptimizer.");
    if (es.lods) {
        ImGui::Indent();
        char ratios[64];
        std::snprintf(ratios, sizeof(ratios), "%s", es.lodRatios.c_str());
        if (attr::Text("LOD ratios",
                       ratios,
                       sizeof(ratios),
                       "The LOD chain's simplify ratios (csv, e.g. "
                       "\"0.5,0.25\")"))
            PushExportAttr(node, "lodRatios", 0.0, false, ratios);
        dragFloat("LOD error", "lodError", es.lodError, 0.001f, 0.0f, 1.0f, "%.3f");
        ImGui::Unindent();
    }
    setBool("Meshlets", "meshlets", es.meshlets, "Per-meshlet build over the cooked topology.");
    if (es.meshlets) {
        ImGui::Indent();
        dragInt("Max verts", "meshletMaxVerts", int(es.meshletMaxVerts), 1.0f, 3, 255);
        dragInt("Max tris", "meshletMaxTris", int(es.meshletMaxTris), 1.0f, 1, 512);
        dragFloat(
            "Cone weight", "meshletConeWeight", es.meshletConeWeight, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::Unindent();
    }
    setBool("Auto-convert textures (KTX2)",
            "autoKtx",
            es.autoKtx,
            "Non-KTX sources transcode in-process at cook.");
    setBool("Skip loose VFS bake files (ignoreVfs)",
            "ignoreVfs",
            es.ignoreVfs,
            "Embed ONLY what the scene itself carries: no loose\n"
            "gi/<stem>.sggi, no givolumemanifest/.givolume rows, no cubemap\n"
            "manifest/KTX rows at pak assembly (minimal variant pak).");

    setBool("Lightmaps (bake)", "lightmap", es.lightmap, "On-demand lightmap bake (Bake GI).");
    if (es.lightmap) {
        ImGui::Indent();
        dragInt("Voxel proxy resolution", "lmProxyRes", es.lmProxyRes, 1.0f, 0, 256);
        dragFloat("Meters per texel",
                  "lmMetersPerTexel",
                  es.lmMetersPerTexel,
                  0.01f,
                  0.01f,
                  1.0f,
                  "%.3f");
        dragInt("Max SG lobes", "lmLobes", int(es.lmLobes), 1.0f, 5, 12);
        dragInt("Atlas width (px)", "lmAtlasWidth", int(es.lmAtlasWidth), 64.0f, 256, 8192);
        dragInt("Rays per texel", "lmRays", int(es.lmRays), 8.0f, 64, 8192);
        ImGui::Unindent();
    }

    setBool("Cubemap captures",
            "cubemap",
            es.cubemap,
            "On-demand preconvolved specular capture set (Update cubemaps).");
    if (es.cubemap) {
        ImGui::Indent();
        dragInt("Size", "cubemapSize", int(es.cubemapSize), 32.0f, 64, 2048);
        dragInt("Rays per texel", "cubemapRays", int(es.cubemapRays), 8.0f, 1, 4096);
        setBool("Octree placement (adaptive)",
                "cubemapOctree",
                es.cubemapOctree,
                "Sparse adaptive placement; growth stops at the\n"
                "32-probe runtime budget.");
        if (es.cubemapOctree) {
            ImGui::Indent();
            dragFloat("Max cell (m)",
                      "cubemapOctreeMax",
                      es.cubemapOctreeMax,
                      0.25f,
                      0.5f,
                      64.0f,
                      "%.1f");
            dragFloat("Min cell (m)",
                      "cubemapOctreeMin",
                      es.cubemapOctreeMin,
                      0.1f,
                      0.125f,
                      std::max(es.cubemapOctreeMax, 0.25f),
                      "%.2f");
            dragInt("Depth cap", "cubemapOctreeDepth", int(es.cubemapOctreeDepth), 1.0f, 0, 8);
            dragFloat("Pinned influence (m)",
                      "cubemapExtraRadius",
                      es.cubemapExtraRadius,
                      0.25f,
                      0.5f,
                      50.0f,
                      "%.1f");
            ImGui::Unindent();
        }
        ImGui::Unindent();
    }

    ImGui::Separator();
    ImGui::Text("GI bakes (on demand — Bake GI in the Cook panel)");
    setBool("GI volumes (bake authored)",
            "giVolumes",
            es.giVolumes,
            "Bake the authored GI volume nodes (gi/<stem>_*.givolume).");
    ImGui::TextDisabled("the world-field octree bake is retired — convex\n"
                        "volumes + the sky IBL carry the ambient");
}

} // namespace editor
