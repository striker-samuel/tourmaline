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
void EditorApp::DrawAssetsPanel() {
    // Asset browser: a GRID directory explorer of the project folder
    // (Unity Project window style). Breadcrumb + Up; folders first;
    // SVG icon tiles (m_icons via ImDrawList::AddImage, drawn shapes
    // as the fallback); a green badge marks assets with a registered
    // GUID (assets.db); click selects, double-click enters folders /
    // opens scenes; Import + Rescan ride the header.
    if (m_projectRoot.empty()) {
        ImGui::TextDisabled("Assets: open a project first (File > New / "
                            "Open Project...).");
        return;
    }
    namespace fs = std::filesystem;
    const auto clearAssetSelection = [&]() {
        ClearAssetBrowserSelection(m_assetsSel, m_assetsSelSet, m_assetsAnchor);
    };
    // Creating/reimporting an internal artifact can select it from code.
    // Drop that stale browser selection: the corresponding tile is hidden.
    if (!m_assetsSel.empty()) {
        std::error_code selEc;
        if (std::filesystem::is_regular_file(m_assetsSel, selEc) &&
            HideAssetBrowserFile(m_assetsSel))
            clearAssetSelection();
    }
    const auto navigateAssetDirectory = [&](const std::string& path) {
        m_assetsDir = path;
        m_assetsExpandGltf.clear();
        m_assetsExpandTour.clear();
        m_assetsFilter = 0;
        clearAssetSelection();
    };
    ImGui::BeginChild("##assets_root",
                      ImVec2(0, 0),
                      false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Navigation state heal: home = <root>/content, but the project
    // root itself is also a valid browsing location.
    std::error_code ec;
    if (m_assetsDir.empty() || !fs::is_directory(fs::path(m_assetsDir), ec) ||
        (!InProjectDir(m_assetsDir) && !AssetBrowserAtProjectRoot(m_projectRoot, m_assetsDir))) {
        navigateAssetDirectory((fs::path(m_projectRoot) / "content").generic_string());
    }

    // ---- OS drag-and-drop target (Windows Explorer -> browser) -------
    // The input hook (OnInputEvent) collects FileDrop batches;
    // route them HERE by the drop point against this panel's rect (the
    // only drop target in the editor): images become .texture assets in
    // the browsed dir, .gltf/.glb run the full import. A drop outside
    // the panel drops with a log (nothing silently swallowed).
    if (m_pendingDropX >= 0.0f) {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const ImVec2 vp = ImGui::GetMainViewport()->Pos;
        const float dx = vp.x + m_pendingDropX;
        const float dy = vp.y + m_pendingDropY;
        if (dx >= wp.x && dx < wp.x + ws.x && dy >= wp.y && dy < wp.y + ws.y)
            HandleDroppedFiles(m_pendingDropFiles);
        else if (!m_pendingDropFiles.empty())
            Log("Drop: landed outside the asset browser (" +
                std::to_string(m_pendingDropFiles.size()) +
                " file(s) ignored — drop onto the Assets panel).");
        m_pendingDropX = -1.0f;
        m_pendingDropY = -1.0f;
        m_pendingDropFiles.clear();
    }

    // Header controls live in this non-scrolling root.  Wrap every
    // action when the dock gets narrow instead of spilling into the grid.
    auto SameLineIfFits = [](const char* nextLabel) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width =
            ImGui::CalcTextSize(nextLabel, nullptr, true).x + style.FramePadding.x * 2.0f;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right)
            ImGui::SameLine();
    };
    // Wrap a group of neighboring widgets as one unit (breadcrumb arrow
    // and the button after it must stay together).
    auto SameLineIfWidthFits = [](float width) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right)
            ImGui::SameLine();
    };
    // Truncate status text so paths/UUIDs cannot escape the dock's width.
    auto FitLabel = [](const std::string& text, float maxWidth) {
        if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
            return text;
        std::string shown = text;
        while (!shown.empty() && ImGui::CalcTextSize(("..." + shown).c_str()).x > maxWidth)
            shown.erase(shown.begin());
        return "..." + shown;
    };

    // ---- header row 1: navigation breadcrumb (wrapping) ------------
    ImGui::BeginDisabled(ProjectRelativePath(m_assetsDir).empty());
    if (ImGui::SmallButton("^ Up")) {
        const std::string rel = ProjectRelativePath(m_assetsDir);
        const size_t slash = rel.find_last_of('/');
        navigateAssetDirectory(
            (slash == std::string::npos)
                ? m_projectRoot
                : (fs::path(m_projectRoot) / rel.substr(0, slash)).generic_string());
    }
    ImGui::EndDisabled();
    SameLineIfFits(m_projectName.c_str());
    if (ImGui::SmallButton(m_projectName.c_str())) {
        navigateAssetDirectory(m_projectRoot);
    }
    {
        fs::path acc = fs::path(m_projectRoot);
        std::string rel = ProjectRelativePath(m_assetsDir);
        int depth = 0;
        while (!rel.empty()) {
            const size_t slash = rel.find('/');
            const std::string comp = (slash == std::string::npos) ? rel : rel.substr(0, slash);
            acc /= comp;
            const std::string shown = comp.size() > 22 ? comp.substr(0, 19) + "..." : comp;
            const ImGuiStyle& style = ImGui::GetStyle();
            const float width = ImGui::CalcTextSize(">").x + style.ItemSpacing.x +
                                ImGui::CalcTextSize(shown.c_str()).x + style.FramePadding.x * 2.0f;
            SameLineIfWidthFits(width);
            ImGui::TextDisabled(">");
            ImGui::SameLine();
            ImGui::PushID(depth++);
            if (ImGui::SmallButton(shown.c_str())) {
                navigateAssetDirectory(acc.generic_string());
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", acc.generic_string().c_str());
            ImGui::PopID();
            rel = (slash == std::string::npos) ? std::string() : rel.substr(slash + 1);
        }
    }

    // ---- header row 2: collection filter and expanded views --------
    // Internal cubemap / GI-cook artifacts are not browsable collections.
    const char* filters[7] = {"All items",
                              "Meshes",
                              "Materials",
                              "Animations",
                              "GI bakes",
                              "Lightmap cooks",
                              "GI volumes"};
    ImGui::SetNextItemWidth(std::min(180.0f, std::max(1.0f, ImGui::GetContentRegionAvail().x)));
    if (ImGui::Combo("##assetsfilter", &m_assetsFilter, filters, 7))
        clearAssetSelection();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show files and folders, or browse assets by type.");
    if (!m_assetsExpandGltf.empty()) {
        const std::string stem = fs::path(m_assetsExpandGltf).stem().string();
        const std::string btn = stem + " X##expand";
        SameLineIfFits(btn.c_str());
        if (ImGui::SmallButton(btn.c_str())) {
            m_assetsExpandGltf.clear();
            clearAssetSelection();
        }
    }
    if (!m_assetsExpandTour.empty()) {
        const std::string stem = fs::path(m_assetsExpandTour).stem().string();
        const std::string btn = stem + " X##expandtour";
        SameLineIfFits(btn.c_str());
        if (ImGui::SmallButton(btn.c_str())) {
            m_assetsExpandTour.clear();
            clearAssetSelection();
        }
    }

    // ---- header row 3: actions (responsive, not scrolling) ----------
    ImGui::Separator();
    // The toolbar and grid context menu share the same creation
    // actions; new assets are placed in the browsed directory.
    const auto drawNewAssetMenu = [&]() {
        if (ImGui::MenuItem("New Scene (.tour)", nullptr, false, !m_cookRunning))
            CreateSceneAsset(m_assetsDir);
        if (ImGui::MenuItem("New Folder"))
            CreateAssetFolder(m_assetsDir);
        ImGui::Separator();
        // Authoring-only .shape documents. The importer/runtime draw
        // paths are intentionally separate from this source creation.
        if (ImGui::BeginMenu("New Procedural Shape")) {
            for (tour::asset::ShapeKind kind : {tour::asset::ShapeKind::Spline,
                                                tour::asset::ShapeKind::PointCloud,
                                                tour::asset::ShapeKind::Plane,
                                                tour::asset::ShapeKind::Box,
                                                tour::asset::ShapeKind::Sphere,
                                                tour::asset::ShapeKind::Cylinder,
                                                tour::asset::ShapeKind::Cone}) {
                const char* name = tour::asset::ShapeKindName(kind);
                if (!ImGui::MenuItem(name))
                    continue;
                const fs::path dir = AssetCreateDirectory(m_projectRoot, m_assetsDir, "shapes");
                const fs::path target = UniqueAssetCreatePath(dir, name, ".shape");
                std::error_code ec;
                fs::create_directories(dir, ec);
                if (ec || target.empty()) {
                    Log("Could not create shape asset directory");
                    continue;
                }
                std::ofstream output(target, std::ios::binary);
                output << tour::asset::SerializeShapeAsset(tour::asset::DefaultShapeAsset(kind));
                output.close();
                if (!output) {
                    Log("Could not write shape asset: " + target.generic_string());
                    continue;
                }
                Log("Created shape source: " + target.generic_string());
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("New GI Volume Asset (.givolume)"))
            CreateGivolumeAsset(m_assetsDir);
        if (ImGui::MenuItem("New Texture (from image)...")) {
            std::string picked;
            if (NativeOpenFile(picked, true) && !picked.empty())
                CreateTextureAsset(picked, m_assetsDir);
        }
    };
    SameLineIfFits("New...");
    if (ImGui::Button("New..."))
        ImGui::OpenPopup("##newcookasset");
    if (ImGui::BeginPopup("##newcookasset")) {
        drawNewAssetMenu();
        ImGui::EndPopup();
    }
    if (m_assetDb.IsOpen()) {
        SameLineIfFits("Refresh");
        const float refreshSize = ImGui::GetFrameHeight();
        const bool rescan = ImGui::Button("##rescan_assets", ImVec2(refreshSize, refreshSize));
        // Two curved arrows drawn in the current UI font-independent style.
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 center(a.x + refreshSize * 0.5f, a.y + refreshSize * 0.5f);
            const float radius = refreshSize * 0.22f;
            const ImU32 c = ImGui::GetColorU32(ImGuiCol_Text);
            constexpr float pi = 3.14159265f;
            for (int half = 0; half < 2; ++half) {
                const float start = half * pi + 0.25f;
                const float finish = start + pi - 0.50f;
                dl->PathArcTo(center, radius, start, finish, 12);
                dl->PathStroke(c, 0, 1.5f);
                const ImVec2 end(center.x + radius * std::cos(finish),
                                 center.y + radius * std::sin(finish));
                const ImVec2 tangent(-std::sin(finish), std::cos(finish));
                const ImVec2 normal(std::cos(finish), std::sin(finish));
                const float head = refreshSize * 0.13f;
                dl->AddTriangleFilled(end,
                                      ImVec2(end.x - tangent.x * head + normal.x * head * 0.65f,
                                             end.y - tangent.y * head + normal.y * head * 0.65f),
                                      ImVec2(end.x - tangent.x * head - normal.x * head * 0.65f,
                                             end.y - tangent.y * head - normal.y * head * 0.65f),
                                      c);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rescan project assets and refresh identity mappings");
        if (rescan) {
            smvk::cache::RescanStats stats;
            std::string rerr;
            if (m_assetDb.RescanProject(stats, rerr))
                Log("Asset rescan: " + std::to_string(stats.adopted) + " adopted, " +
                    std::to_string(stats.moved) + " re-pointed, " + std::to_string(stats.derived) +
                    " derived, " + std::to_string(stats.missing) + " missing.");
            else
                Log("Asset rescan failed: " + rerr);
        }
    }
    // Selection actions (Open = scene files only; paks run in the App).
    static const auto isOpenable = [](const std::string& p) {
        std::string e = fs::path(p).extension().string();
        for (char& c : e)
            c = char(std::tolower((unsigned char)c));
        return smvk::assets::IsMeshSourceExtension(e) || e == ".tour";
    };
    {
        const bool selFile = !m_assetsSel.empty() && fs::is_regular_file(fs::path(m_assetsSel), ec);
        SameLineIfFits("Open");
        ImGui::BeginDisabled(!(selFile && isOpenable(m_assetsSel)));
        if (ImGui::SmallButton("Open##sel")) {
            if (!m_cookRunning)
                OpenAnySceneFile(m_assetsSel);
            else
                Log("Assets: a cook is running - wait for it to "
                    "finish.");
        }
        ImGui::EndDisabled();
        SameLineIfFits("Reimport");
        const std::string selectedExt = fs::path(m_assetsSel).extension().string();
        const bool canReimport =
            selFile && !m_cookRunning &&
            (smvk::assets::IsMeshSourceFile(fs::path(m_assetsSel)) || selectedExt == ".texture");
        ImGui::BeginDisabled(!canReimport);
        if (ImGui::SmallButton("Reimport##sel"))
            ReimportAsset(m_assetsSel);
        ImGui::EndDisabled();
        SameLineIfFits("Reveal");
        ImGui::BeginDisabled(m_assetsSel.empty());
        if (ImGui::SmallButton("Reveal##sel"))
            RevealInExplorer(m_assetsSel);
        ImGui::EndDisabled();
        // Add-to-scene (mesh/geometry selection): the click twin of the
        // viewport drag-drop (AddSceneReference asset-bind). Imports
        // land in-project first (ImportAsset), so this only ever adds
        // in-project assets. A mesh-source container adds its first
        // derived geometry (same as expanding it and dropping the tile).
        {
            auto isMeshAsset = [](const std::string& p) {
                std::string e = fs::path(p).extension().string();
                for (char& c : e)
                    c = char(std::tolower((unsigned char)c));
                return e == ".geometry" || e == ".mesh";
            };
            auto isGltfContainer = [](const std::string& p) {
                std::string e = fs::path(p).extension().string();
                for (char& c : e)
                    c = char(std::tolower((unsigned char)c));
                return smvk::assets::IsMeshSourceExtension(e);
            };
            const bool canAdd = selFile && m_world != nullptr && !m_cookRunning &&
                                (isMeshAsset(m_assetsSel) || isGltfContainer(m_assetsSel));
            SameLineIfFits("Add to scene");
            ImGui::BeginDisabled(!canAdd);
            if (ImGui::SmallButton("Add to scene##sel")) {
                std::string target = m_assetsSel;
                if (isGltfContainer(target) && !isMeshAsset(target)) {
                    // Resolve the container's first derived geometry.
                    smvk::assets::AssetManifest man;
                    std::string merr;
                    if (smvk::assets::LoadAssetManifest(target, man, merr)) {
                        const std::string base = fs::path(target).parent_path().generic_string();
                        for (const auto& e : man.entries) {
                            if (e.kind == smvk::assets::AssetKind::Geometry) {
                                target = (fs::path(base) / e.file).generic_string();
                                break;
                            }
                        }
                    }
                }
                if (isMeshAsset(target) || target != m_assetsSel) {
                    if (AddSceneReference(target, {}, true))
                        Log("Assets: added geometry asset '" + target + "' (asset bind).");
                } else {
                    Log("Assets: '" + m_assetsSel +
                        "' has no derived mesh yet — reimport it first.");
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Add the selected mesh to the scene\n"
                                  "(same as dropping its tile on a viewport).");
        }
        // Delete (selection action): plans + confirms through the modal
        // (refusals — open scene, derived assets, cache — log + no-op).
        // MULTI: the set deletes as one request when >1.
        SameLineIfFits("Delete");
        ImGui::BeginDisabled(m_assetsSel.empty());
        const std::string deleteLabel =
            m_assetsSelSet.size() > 1
                ? "Delete (" + std::to_string(m_assetsSelSet.size()) + ")##sel"
                : "Delete##sel";
        if (ImGui::SmallButton(deleteLabel.c_str())) {
            if (m_assetsSelSet.size() > 1)
                RequestAssetDelete(m_assetsSelSet.data(), m_assetsSelSet.size());
            else
                RequestAssetDelete(&m_assetsSel, 1);
        }
        ImGui::EndDisabled();
        // Selected file is informational and has its OWN line. Never
        // put a potentially long filename/UUID beside the action buttons.
        if (!m_assetsSel.empty()) {
            std::string selGuid;
            if (m_assetDb.IsOpen())
                m_assetDb.GuidForPath(m_assetsSel, selGuid);
            const std::string selected = fs::path(m_assetsSel).filename().string();
            const std::string label =
                FitLabel("Selected: " + selected, ImGui::GetContentRegionAvail().x);
            ImGui::TextDisabled("%s", label.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", m_assetsSel.c_str());
            if (!selGuid.empty()) {
                SameLineIfFits("Copy UUID");
                if (ImGui::SmallButton("Copy UUID##sel"))
                    ImGui::SetClipboardText(selGuid.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", selGuid.c_str());
            }
        }
    }

    // ---- scrollable grid --------------------------------------------
    // Defer tile-context deletes until after the grid child has ended:
    // OpenPopup() and BeginPopupModal() must run at the same popup depth.
    std::string tileDeleteRequested;
    bool tileDeleteMulti = false;
    static std::string renameTarget;
    static char renameName[256]{};
    bool openRename = false;
    const auto requestRename = [&](const std::string& path) {
        if (path.empty() || m_cookRunning)
            return;
        // Never move an active scene: autosave still targets its path.
        if (path == m_gltfPath || path == m_gltfHostPath) {
            Log("Rename: close the open scene first.");
            return;
        }
        renameTarget = path;
        const std::string filename = fs::path(path).filename().string();
        std::snprintf(renameName, sizeof(renameName), "%s", filename.c_str());
        openRename = true;
    }; // the multi-set delete request
    ImGui::Separator();
    // The grid alone fills the remaining height, so toolbar rows never
    // compete with tiles. IMPORTANT: obtain dl after BeginChild; using
    // the parent draw list causes custom labels/icons to bleed over the
    // toolbar when scrolling.
    ImGui::BeginChild("##assetgrid", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Background context menu shares the header's New... actions.
    // .gicook/.lmcook are bake outputs, never user-created.
    if (ImGui::BeginPopupContextWindow("##assetgrid_ctx",
                                       ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
        drawNewAssetMenu();
        ImGui::EndPopup();
    }
    // Registered GUID map (assets.db) for badges + tooltips. The rows
    // also feed the bake-asset collections below (kept in scope: the
    // collection branch reads them).
    std::unordered_map<std::string, std::string> guidByAbs;
    // The reverse map (guid -> abs path): the .tour folder view resolves
    // its bake refs (the tour's editorSet bake.*.guid keys) through the
    // assetdb — moves/renames heal through the rescan.
    std::unordered_map<std::string, std::string> absByGuid;
    std::unordered_map<std::string, std::string> kindByGuid;
    std::vector<smvk::cache::AssetRecord> assetRows;
    if (m_assetDb.IsOpen()) {
        std::string derr;
        if (m_assetDb.All(assetRows, derr)) {
            for (const auto& r : assetRows) {
                const std::string abs = m_assetDb.ResolvePath(r.path);
                guidByAbs[abs] = r.guid;
                absByGuid[r.guid] = abs;
                kindByGuid[r.guid] = r.kind;
            }
        }
    }
    // A .tour with only hidden bake references remains a normal scene
    // tile (not a folder containing just a back button + itself).
    const auto visibleBakeRef = [&](const std::string& guid) {
        const auto path = absByGuid.find(guid);
        if (path == absByGuid.end())
            return false;
        const auto kind = kindByGuid.find(guid);
        if (HideAssetBrowserFile(path->second, kind == kindByGuid.end() ? "" : kind->second))
            return false;
        std::error_code ec;
        return fs::is_regular_file(path->second, ec) && !ec;
    };
    // The tour's bake-asset references (RecordBakeAssetsFromCook writes
    // them; SaveEnvToDoc persists them into the tour): the editorSet
    // guids the .tour folder view resolves through the assetdb. Tolerant
    // — an unreadable/unref'd tour presents as a plain openable file.
    auto tourBakeRefs = [](const std::string& tourAbs, std::vector<std::string>& out) {
        smvk::tour::Doc td;
        std::string terr;
        if (!smvk::tour::LoadTour(tourAbs, td, terr))
            return;
        for (const auto& [k, v] : td.editorSettings) {
            if (k != "bake.gicook.guid" && k != "bake.cubemap.guid")
                continue;
            std::string g = v;
            // The tokenizer glues the statement ';' onto the last
            // quoted -v token (see the tour tests); trim it.
            while (!g.empty() && (g.back() == ';' || g.back() == ' ' || g.back() == '\r'))
                g.pop_back();
            if (!g.empty())
                out.push_back(g);
        }
    };
    // Entries: directories first, then files, both by name (skip
    // .meta sidecars + dotfiles).
    // FRAME-ARENA STRINGS (the App's pattern — Memory/arena.h): the
    // per-frame entry storage rides smvk::FrameArena() (AllocateString
    // copies in, NUL-terminated, zero heap churn; the arena resets at
    // the frame boundary — MainLoop's ResetFrameArena). The views are
    // FRAME-SCOPED: everything that OUTLIVES the frame (the selection,
    // the expand state, the drag payload SINKS) copies out via Str().
    auto& fArena = smvk::FrameArena();
    auto put = [&](const std::string& s) { return fArena.AllocateString(smvk::StringViewT(s)); };
    struct Entry {
        smvk::StringViewT name;
        smvk::StringViewT abs;
        bool dir = false;
        IconType icon{};
        bool gltf = false; // .gltf with a manifest: presents as folder
        bool tour = false; // .tour with bake refs: presents as folder
        bool back = false; // the expanded view's ".." tile
        // Drag-drop payload kinds ("" = not draggable).
        bool dragMesh = false;
        bool dragMaterial = false;
        // >= 0: a VIRTUAL FOLDER tile — the canon collection view of
        // that filter index (see the kVFolders table below). Chrome,
        // not an asset: never selects, never deletes, never drags.
        int vfilter = -1;
    };
    std::vector<Entry> dirs, files;
    // ---- VIRTUAL FOLDERS (the canon collection views) ---------------
    // At the project root, the header filter views appear as FOLDER
    // TILES in the grid: each enters its collection
    // on double-click; the active view's ".." leaves back to All. The
    // tile ids ride the "vfolder://<n>" scheme — never a real path
    // (plus the per-tile numeric uid below makes every tile id unique
    // regardless — the duplicate-abs conflict fix).
    {
        struct VFolderDef {
            const char* label;
            int filter;
            IconType icon;
        };
        static const VFolderDef kVFolders[6] = {
            {"Meshes", 1, IconType::Model},
            {"Materials", 2, IconType::Texture},
            {"Animations", 3, IconType::Animation},
            {"GI bakes", 4, IconType::GI},
            {"Lightmap cooks", 5, IconType::GI},
            {"GI volumes", 6, IconType::GI},
        };
        if (m_assetsFilter != 0) {
            // The active collection view's leave tile (".." -> All).
            // (The expanded glTF/tour views build their own back tiles
            // in their branches below — no second one here.)
            Entry back{};
            back.name = "..";
            back.abs = put("vfolder://up");
            back.dir = true;
            back.back = true;
            back.icon = IconType::Folder;
            dirs.push_back(back);
        } else if (ShowAssetBrowserCollectionFolders(m_projectRoot,
                                                     m_assetsDir,
                                                     !m_assetsExpandGltf.empty(),
                                                     !m_assetsExpandTour.empty())) {
            // Only the project root shows collection tiles; keep them
            // ahead of real directories in the stable sort below.
            for (const VFolderDef& v : kVFolders) {
                Entry t{};
                t.name = put(v.label);
                t.abs = put("vfolder://" + std::to_string(v.filter));
                t.dir = true;
                t.vfilter = v.filter;
                t.icon = v.icon;
                dirs.push_back(t);
            }
        }
    }
    const auto iconForAsset = [&](smvk::assets::AssetKind k) {
        switch (k) {
        case smvk::assets::AssetKind::Geometry:
            return IconType::Model;
        case smvk::assets::AssetKind::Material:
            return IconType::Texture;
        case smvk::assets::AssetKind::Anim:
            return IconType::Animation;
        }
        return IconType::File;
    };
    // A glTF's derived assets (its manifest children), for BOTH the
    // expanded view and the collection filters.
    const std::string expandGltf = m_assetsExpandGltf;
    const std::string expandTour = m_assetsExpandTour;
    auto manifestChildren =
        [&](const std::string& gltfAbs, int kindFilter, std::vector<Entry>& out) {
            smvk::assets::AssetManifest man;
            std::string merr;
            if (!smvk::assets::LoadAssetManifest(gltfAbs, man, merr))
                return;
            const std::string base = fs::path(gltfAbs).parent_path().generic_string();
            for (const auto& e : man.entries) {
                if (kindFilter >= 0 && int(e.kind) != kindFilter)
                    continue;
                const fs::path assetPath = fs::path(base) / e.file;
                if (HideAssetBrowserFile(assetPath))
                    continue;
                Entry t{};
                t.name = put(e.name.empty() ? e.file : e.name);
                t.abs = put(assetPath.generic_string());
                t.icon = iconForAsset(e.kind);
                t.dragMesh = (e.kind == smvk::assets::AssetKind::Geometry);
                t.dragMaterial = (e.kind == smvk::assets::AssetKind::Material);
                out.push_back(std::move(t));
            }
        };
    // Recursive .gltf.meta scan under the current directory (the
    // collection views list every derived asset of the kind).
    std::vector<std::string> gltfMetas;
    if (m_assetsFilter != 0 || !expandGltf.empty()) {
        std::vector<fs::path> stack{fs::path(m_assetsDir)};
        while (!stack.empty()) {
            const fs::path d = stack.back();
            stack.pop_back();
            std::error_code sec;
            for (fs::directory_iterator it(d, sec), end; !sec && it != end; it.increment(sec)) {
                if (sec)
                    break;
                std::error_code fec;
                if (it->is_directory(fec) && !fec) {
                    stack.push_back(it->path());
                    continue;
                }
                if (!it->is_regular_file(fec) || fec)
                    continue;
                const std::string n = it->path().filename().string();
                if (n.size() > 10 && n.compare(n.size() - 10, 10, ".gltf.meta") == 0)
                    gltfMetas.push_back(it->path().string());
            }
        }
    }
    if (m_assetsFilter != 0) {
        if (m_assetsFilter <= 3) {
            // Collection view: every derived asset of the kind under the
            // current directory tree.
            for (const std::string& meta : gltfMetas) {
                const std::string gltf = meta.substr(0, meta.size() - 5); // strip ".meta"
                manifestChildren(gltf, m_assetsFilter - 1, files);
            }
        } else {
            // Visible bake/cook collections (GI bakes / lightmap cooks /
            // GI volumes): the assetdb rows adopted from
            // the .meta sidecars (see Engine/Asset/bake_asset.h for the
            // bake rows, Engine/Asset/cook_asset.h for the cook rows) —
            // moves/renames heal through the rescan, so these stay
            // correct where stem-named loose listings would rot. Rows
            // whose file is gone are skipped (the db keeps them as
            // retargets).
            const char* want = (m_assetsFilter == 4)   ? "gi"
                               : (m_assetsFilter == 5) ? "lmcook"
                                                       : "givolume";
            for (const auto& r : assetRows) {
                if (r.kind != want)
                    continue;
                const std::string abs = m_assetDb.ResolvePath(r.path);
                if (HideAssetBrowserFile(abs, r.kind))
                    continue;
                std::error_code fec;
                if (abs.empty() || !fs::is_regular_file(abs, fec) || fec)
                    continue;
                Entry t{};
                t.name = put(fs::path(abs).filename().generic_string());
                t.abs = put(abs);
                t.icon = IconType::GI;
                files.push_back(std::move(t));
            }
        }
    } else if (!expandGltf.empty()) {
        // Expanded glTF: its derived assets + a back tile (Unity-style
        // folder view; the glTF itself stays listed as the source).
        Entry back{};
        back.name = "..";
        back.abs = put(expandGltf);
        back.dir = true;  // plain-directory styling
        back.back = true; // double-click = leave the folder
        back.icon = IconType::Folder;
        dirs.push_back(back);
        manifestChildren(expandGltf, -1, files);
        Entry src{};
        src.name = put(fs::path(expandGltf).filename().string());
        src.abs = put(expandGltf);
        src.icon = IconType::Model;
        src.gltf = false; // inside the folder, double-click opens the source
        files.push_back(src);
    } else if (!expandTour.empty()) {
        // Expanded .tour: the SAME folder presentation, over the tour's
        // BAKE ASSETS — the refs (RecordBakeAssetsFromCook) resolve
        // through the assetdb (moves/renames heal through the rescan).
        // The tour itself stays listed as the source tile: double-click
        // OPENS it (isOpenable — the tour remains openable from the
        // browser, nested inside its own folder view).
        Entry back{};
        back.name = "..";
        back.abs = put(expandTour);
        back.dir = true;
        back.back = true;
        back.icon = IconType::Folder;
        dirs.push_back(back);
        std::vector<std::string> bakeRefs;
        tourBakeRefs(expandTour, bakeRefs);
        for (const std::string& guid : bakeRefs) {
            if (!visibleBakeRef(guid))
                continue; // hidden or unavailable bake artifact
            const auto git = absByGuid.find(guid);
            std::error_code fec;
            if (!fs::is_regular_file(git->second, fec) || fec)
                continue;
            Entry t{};
            t.name = put(fs::path(git->second).filename().generic_string());
            t.abs = put(git->second);
            t.icon = (t.name.size() > 13 &&
                      t.name.Str().compare(t.name.size() - 13, 13, ".cubemanifest") == 0)
                         ? IconType::Env
                         : IconType::GI;
            files.push_back(std::move(t));
        }
        // The source itself: a plain openable .tour tile (double-click
        // opens the scene — see the tile handler's isOpenable branch).
        Entry src{};
        src.name = put(fs::path(expandTour).filename().string());
        src.abs = put(expandTour);
        src.icon = IconTypeFor(src.name.Str(), false);
        files.push_back(src);
    } else {
        for (fs::directory_iterator it(m_assetsDir, ec), end; !ec && it != end; it.increment(ec)) {
            if (ec)
                break;
            std::error_code fec;
            const bool isDir = it->is_directory(fec) && !fec;
            if (!isDir && (!it->is_regular_file(fec) || fec))
                continue;
            const fs::path& p = it->path();
            std::string name = p.filename().string();
            if (name.empty() || name[0] == '.')
                continue;
            bool isGltf = false;
            bool isTour = false;
            bool dragMesh = false;
            bool dragMaterial = false;
            IconType icon = IconTypeFor(name, isDir);
            if (!isDir) {
                std::string e = p.extension().string();
                for (char& c : e)
                    c = char(std::tolower((unsigned char)c));
                // The ordinary folder lists authored source files only.
                // Derived geometry/material/animation files remain accessible
                // as logical subassets through their source manifest, rather
                // than appearing again as loose sibling files.
                const bool generated =
                    e == ".geometry" || e == ".mesh" || e == ".material" || e == ".anim";
                if (e == ".meta" || generated || HideAssetBrowserFile(p))
                    continue; // generated output or internal bake artifact
                if (smvk::assets::IsMeshSourceExtension(e)) {
                    // With a manifest the source presents as a FOLDER
                    // of its derived assets; without one it opens as
                    // a scene (the legacy path).
                    std::string merr;
                    smvk::assets::AssetManifest man;
                    isGltf = smvk::assets::LoadAssetManifest(p.generic_string(), man, merr);
                    if (isGltf)
                        icon = IconType::Folder;
                } else if (e == ".tour") {
                    // With bake-asset refs (a cook ran —
                    // RecordBakeAssetsFromCook) the tour presents as a
                    // FOLDER of its bakes, mirroring the glTF-with-manifest
                    // presentation; without refs it stays a plain openable
                    // file (the legacy behavior).
                    std::vector<std::string> refs;
                    tourBakeRefs(p.generic_string(), refs);
                    isTour = std::any_of(refs.begin(), refs.end(), visibleBakeRef);
                    if (isTour)
                        icon = IconType::Folder;
                } else if (e == ".geometry" || e == ".mesh")
                    dragMesh = true;
                else if (e == ".material")
                    dragMaterial = true;
            }
            (isDir ? dirs : files)
                .push_back({put(name),
                            put(p.generic_string()),
                            isDir,
                            icon,
                            isGltf,
                            isTour,
                            false, // back
                            dragMesh,
                            dragMaterial});
        }
    }
    const auto byName = [](const Entry& a, const Entry& b) {
        std::string la = a.name.Str(), lb = b.name.Str();
        for (char& c : la)
            c = char(std::tolower((unsigned char)c));
        for (char& c : lb)
            c = char(std::tolower((unsigned char)c));
        return la < lb;
    };
    // The canon VIRTUAL FOLDERS stay PINNED to the front of the dirs
    // row (pre-sort push order preserved: a stable partition — the
    // vfolders first in their authored order, the real directories
    // byName behind them). The Unity feel: the collection roots are
    // the affordance, the real folders follow.
    const auto vfolderFirst = [](const Entry& a, const Entry& b) {
        if ((a.vfilter >= 0) != (b.vfilter >= 0))
            return a.vfilter >= 0;
        if (a.vfilter >= 0 && b.vfilter >= 0)
            return a.vfilter < b.vfilter;
        return false;
    };
    std::stable_sort(dirs.begin(), dirs.end(), [&](const Entry& a, const Entry& b) {
        if (vfolderFirst(a, b))
            return true;
        if (vfolderFirst(b, a))
            return false;
        return byName(a, b);
    });
    std::sort(files.begin(), files.end(), byName);

    const float tileW = 96.0f, tileH = 104.0f, iconS = 56.0f;
    int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / tileW));
    int col = 0;
    // Collection views can produce duplicate paths/names. A per-frame
    // ordinal scopes EVERY tile uniquely, including filter/back tiles;
    // the remaining path/name IDs still distinguish its child controls.
    int tileOrdinal = 0;
    auto tile = [&](const Entry& e) {
        ImGui::PushID(tileOrdinal++);
        ImGui::PushID(e.abs.data());
        ImGui::PushID(e.name.data());
        ImGui::PushID(e.back ? 1 : 0);
        ImGui::PushID(e.vfilter);
        // The SET highlight (the multi-selection); the primary
        // (m_assetsSel) is redundant inside the set on plain clicks.
        // NAVIGATION CHROME NEVER HIGHLIGHTS: the ".." back tile
        // carries the SAME abs as the expanded view's SOURCE tile
        // (both = the glTF path) — selecting the source lit the
        // ".." too ("the .. up directory gets highlighted"); the
        // VIRTUAL FOLDERS are chrome the same way.
        const bool chrome = e.back || e.vfilter >= 0;
        const bool sel =
            !chrome && (std::find(m_assetsSelSet.begin(), m_assetsSelSet.end(), e.abs.Str()) !=
                            m_assetsSelSet.end() ||
                        m_assetsSel == e.abs.Str());
        const ImVec2 tileSz(tileW - 8.0f, tileH);
        (void)ImGui::InvisibleButton("##tile", tileSz);
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool dbl = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const ImVec2 bmin = ImGui::GetItemRectMin();
        const ImVec2 bmax = ImGui::GetItemRectMax();
        // hover/selection chrome under the icon art.
        if (sel) {
            dl->AddRectFilled(bmin, bmax, IM_COL32(70, 130, 190, 60), 6.0f);
            dl->AddRect(bmin, bmax, IM_COL32(120, 200, 255, 220), 6.0f, 0, 1.5f);
        } else if (hovered) {
            dl->AddRectFilled(bmin, bmax, IM_COL32(255, 255, 255, 16), 6.0f);
        }
        // icon centered (SVG texture, drawn shapes as fallback).
        const ImVec2 ico(bmin.x + (tileSz.x - iconS) * 0.5f, bmin.y + 6.0f);
        // THE .material SIDECAR PREVIEW (the matball pass's PNG): the
        // ball-under-default-sky thumbnail replaces the generic icon
        // when the sidecar exists (the Material tab's Ball preview
        // button renders it). Missing/stale = the generic icon (the
        // button regenerates on demand).
        ImTextureID sidecar = 0;
        if (!e.dir) {
            const std::string name = e.name.Str();
            std::string ext = fs::path(name).extension().string();
            for (char& c : ext)
                c = char(std::tolower((unsigned char)c));
            if (ext == ".material")
                sidecar = AssetSidecarThumb(e.abs.Str() + ".png");
            else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
                     ext == ".tga")
                sidecar = AssetSidecarThumb(e.abs.Str());
        }
        if (sidecar != 0) {
            dl->AddImage(sidecar, ico, ImVec2(ico.x + iconS, ico.y + iconS));
        } else {
            const IconTex& icon = m_icons[size_t(e.icon)];
            if (icon.imgui != 0) {
                dl->AddImage(icon.imgui, ico, ImVec2(ico.x + iconS, ico.y + iconS));
            } else {
                DrawAssetIconFallback(dl, ico, iconS, e.icon);
            }
        }
        // Registered-asset badge (green dot, bottom-right of icon).
        if (!e.dir && guidByAbs.contains(e.abs.Str())) {
            dl->AddCircleFilled(ImVec2(ico.x + iconS - 3.0f, ico.y + iconS - 3.0f),
                                3.5f,
                                IM_COL32(90, 220, 120, 255),
                                10);
        }
        // Label: up to two centered lines, clipped with "...".
        {
            ImFont* font = ImGui::GetFont();
            const float fsize = ImGui::GetFontSize();
            const float maxW = (tileSz.x) - 10.0f;
            auto textW = [&](const char* s) {
                return font->CalcTextSizeA(fsize, 10000.0f, 0.0f, s).x;
            };
            std::string l1 = e.name.Str(), l2;
            if (textW(l1.c_str()) > maxW) {
                // Longest fitting prefix, preferring a separator.
                std::string growing;
                size_t best = 0;
                for (size_t i = 0; i < e.name.size(); ++i) {
                    growing.push_back(e.name[i]);
                    if (textW(growing.c_str()) > maxW)
                        break;
                    best = i + 1;
                }
                size_t cut = best;
                for (size_t i = best; i > 1; --i)
                    if (e.name[i - 1] == '_' || e.name[i - 1] == '-' || e.name[i - 1] == ' ' ||
                        e.name[i - 1] == '.') {
                        cut = i;
                        break;
                    }
                l1 = e.name.Str().substr(0, cut);
                l2 = e.name.Str().substr(cut);
                if (textW(l2.c_str()) > maxW) {
                    while (!l2.empty() && textW((l2 + "...").c_str()) > maxW)
                        l2.pop_back();
                    l2 += "...";
                }
            }
            const ImU32 tcol = ImGui::GetColorU32(ImGuiCol_Text);
            dl->PushClipRect(bmin, bmax, true);
            const float yBase = bmax.y - 2.0f * fsize - 3.0f;
            const float cx = bmin.x + tileSz.x * 0.5f;
            if (l2.empty()) {
                dl->AddText(font,
                            fsize,
                            ImVec2(cx - textW(l1.c_str()) * 0.5f, yBase + fsize * 0.5f),
                            tcol,
                            l1.c_str());
            } else {
                dl->AddText(
                    font, fsize, ImVec2(cx - textW(l1.c_str()) * 0.5f, yBase), tcol, l1.c_str());
                dl->AddText(font,
                            fsize,
                            ImVec2(cx - textW(l2.c_str()) * 0.5f, yBase + fsize + 1.0f),
                            tcol,
                            l2.c_str());
            }
            dl->PopClipRect();
        }
        // ---- SELECTION: single / ctrl-toggle / shift-range ----------
        // The SET is the browser's selection (m_assetsSel stays the
        // PRIMARY for the single-flow consumers: drags, the header
        // buttons). A plain click replaces the set; ctrl toggles the
        // entry; shift selects the anchor..entry RANGE over the
        // display order (dirs then files — the vectors in scope).
        // NAVIGATION TILES NEVER JOIN: the virtual-folder chrome (the
        // ".." back tile, the canon VIRTUAL FOLDERS) is a move, not
        // an asset — chrome entering the set made ranges span it +
        // the delete target the folder's PARENT.
        if (clicked && !e.back && e.vfilter < 0) {
            const bool ctrl = ImGui::GetIO().KeyCtrl;
            const bool shift = ImGui::GetIO().KeyShift;
            m_assetsSel = e.abs.Str();
            if (e.back) {
                // Navigation-only; handled on double-click below.
            } else if (ctrl) {
                // Toggle; the anchor moves (the next shift ranges
                // from HERE).
                auto it = std::find(m_assetsSelSet.begin(), m_assetsSelSet.end(), e.abs.Str());
                if (it != m_assetsSelSet.end())
                    m_assetsSelSet.erase(it);
                else
                    m_assetsSelSet.push_back(e.abs.Str());
                m_assetsAnchor = e.abs.Str();
            } else if (shift && !m_assetsAnchor.empty()) {
                // The anchor..entry range over the display order.
                const auto span = [&](const Entry& en) {
                    return en.abs.Str() == e.abs.Str() || en.abs.Str() == m_assetsAnchor;
                };
                auto collectRange = [&](const std::vector<Entry>& v) {
                    auto first = std::find_if(v.begin(), v.end(), span);
                    if (first == v.end())
                        return;
                    auto last = std::find_if(v.rbegin(), v.rend(), span).base();
                    for (auto it = first; it != last; ++it)
                        if (!(it->back || it->vfilter >= 0)
                            // chrome never ranges in
                            && std::find(m_assetsSelSet.begin(),
                                         m_assetsSelSet.end(),
                                         it->abs.Str()) == m_assetsSelSet.end())
                            m_assetsSelSet.push_back(it->abs.Str());
                };
                // Clear the non-range picks? Shift-select ADDS the
                // range to the set (the ctrl-picks survive — the
                // standard additive model).
                collectRange(dirs);
                collectRange(files);
            } else {
                m_assetsSelSet = {e.abs.Str()};
                m_assetsAnchor = e.abs.Str();
            }
        }
        if (dbl) {
            if (e.vfilter >= 0) {
                // ENTER THE CANON COLLECTION VIEW (the virtual
                // folder): the filter combo and the tile are the same
                // knob.
                m_assetsFilter = e.vfilter;
                clearAssetSelection();
            } else {
                if (!e.back) {
                    m_assetsSel = e.abs.Str();
                    m_assetsSelSet = {e.abs.Str()};
                    m_assetsAnchor = e.abs.Str();
                }
                if (e.back) {
                    // Leave the expanded glTF/tour folder OR the canon
                    // collection view (".." -> All).
                    m_assetsExpandGltf.clear();
                    m_assetsExpandTour.clear();
                    m_assetsFilter = 0;
                    clearAssetSelection();
                } else if (e.gltf) {
                    // A glTF with a manifest: open the FOLDER (the derived
                    // assets) — never the scene (the .tour references the
                    // .mesh asset; the glTF is the container/source).
                    m_assetsExpandGltf = e.abs.Str();
                    clearAssetSelection();
                } else if (e.tour) {
                    // A .tour with bake refs: open the FOLDER (its bake
                    // assets). The tour itself stays openable INSIDE (the
                    // source tile's isOpenable branch below) and via the
                    // header Open button.
                    m_assetsExpandTour = e.abs.Str();
                    clearAssetSelection();
                } else if (e.dir) {
                    navigateAssetDirectory(e.abs.Str());
                } else if (isOpenable(e.abs.Str())) {
                    if (!m_cookRunning)
                        OpenAnySceneFile(e.abs.Str());
                    else
                        Log("Assets: a cook is running - wait for it "
                            "to finish.");
                }
            }
        }
        // Drag-and-drop sources (mesh -> viewport adds a scene
        // reference; material -> the selection assigns it).
        // (The canon VIRTUAL FOLDERS never drag — chrome.)
        if ((e.dragMesh || e.dragMaterial) && e.vfilter < 0) {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                const char* payloadType = e.dragMesh ? "VT_ASSET_MESH" : "VT_ASSET_MAT";

                ImGui::SetDragDropPayload(
                    payloadType, e.abs.data(), e.abs.size() + 1, ImGuiCond_Once);

                ImGui::TextUnformatted(e.name.data());
                ImGui::Separator();

                ImGui::TextDisabled(e.dragMesh ? "Drop on viewport to add scene reference"
                                               : "Drop on viewport to assign material");

                ImGui::EndDragDropSource();
            }
        }
        if (hovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (e.vfilter >= 0) {
                ImGui::SetTooltip("View the %s collection.\n"
                                  "Double-click to open.",
                                  e.name.data());
            } else if (e.back) {
                ImGui::SetTooltip("Double-click to return.");
            } else {
                const auto git = guidByAbs.find(e.abs.Str());
                ImGui::SetTooltip("%s\n%s\n%s",
                                  e.name.data(),
                                  e.abs.data(),
                                  (git != guidByAbs.end() && !git->second.empty())
                                      ? git->second.c_str()
                                      : "(no asset uuid)");
            }
        }
        // Navigation chrome (back/collection) has no Delete menu.
        // Real assets use the same confirm dialog as the header.
        if (!e.back && e.vfilter < 0 && ImGui::BeginPopupContextItem("##assettile_ctx")) {
            // DCC link resolves manifest-derived tiles to their source.
            std::error_code dccEc;
            if (fs::is_regular_file(e.abs.Str(), dccEc) && !dccEc) {
                const std::string assetExt = fs::path(e.abs.Str()).extension().string();
                if (ImGui::MenuItem("Reimport",
                                    nullptr,
                                    false,
                                    !m_cookRunning &&
                                        (smvk::assets::IsMeshSourceFile(fs::path(e.abs.Str())) ||
                                         assetExt == ".texture")))
                    ReimportAsset(e.abs.Str());
                if (assetExt == ".tour" &&
                    ImGui::MenuItem(
                        "Edit in main editor", nullptr, false, m_fullPrefabParentSnapshot.empty()))
                    RequestFullPrefabEdit(e.abs.Str());
                if (ImGui::MenuItem("Open in DCC"))
                    OpenAssetInDcc(e.abs.Str());
                if (ImGui::MenuItem("Link DCC source..."))
                    LinkAssetToDcc(e.abs.Str());
                if (ImGui::MenuItem("Publish glTF", nullptr, false, !m_dccPublishRunning))
                    PublishAssetFromDcc(e.abs.Str());
                if (ImGui::MenuItem("Unlink DCC source"))
                    UnlinkAssetFromDcc(e.abs.Str());
                ImGui::Separator();
            }
            if (ImGui::MenuItem(
                    "Rename", nullptr, false, !m_cookRunning && m_assetsSelSet.size() <= 1))
                requestRename(e.abs.Str());
            // MULTI-AWARE: deleting a tile that sits in a multi-
            // selection (>1) requests the WHOLE set (one confirm,
            // every plan executes); a single/unselected tile keeps
            // the single flow.
            const bool inSet =
                std::find(m_assetsSelSet.begin(), m_assetsSelSet.end(), e.abs.Str()) !=
                m_assetsSelSet.end();
            if (ImGui::MenuItem(m_assetsSelSet.size() > 1 && inSet ? "Delete (selection)"
                                                                   : "Delete")) {
                m_assetsSel = e.abs.Str();
                if (inSet && m_assetsSelSet.size() > 1)
                    tileDeleteMulti = true;
                else
                    tileDeleteRequested = e.abs.Str();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    };
    // Folder/collection count reflects the visible entries, not hidden
    // sidecars or generated artifacts. A virtual ".." tile is navigation.
    const size_t visibleItems =
        dirs.size() + files.size() -
        size_t(std::count_if(dirs.begin(), dirs.end(), [](const Entry& e) { return e.back; }));
    ImGui::TextDisabled("%zu item%s", visibleItems, visibleItems == 1 ? "" : "s");
    for (const Entry& e : dirs) {
        if (col != 0)
            ImGui::SameLine(0.0f, 4.0f);
        tile(e);
        if (++col == cols)
            col = 0;
    }
    for (const Entry& e : files) {
        if (col != 0)
            ImGui::SameLine(0.0f, 4.0f);
        tile(e);
        if (++col == cols)
            col = 0;
    }
    if (dirs.empty() && files.empty())
        ImGui::TextDisabled("(empty folder)");
    ImGui::EndChild(); // ##assetgrid
    // MULTIDELETE on the Delete KEY while the grid holds the focus
    // (the key + the context menu are the two delete gestures).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_F2) && m_assetsSelSet.size() == 1)
        requestRename(m_assetsSelSet.front());
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (m_assetsSelSet.size() > 1)
            tileDeleteMulti = true;
        else if (!m_assetsSel.empty())
            tileDeleteRequested = m_assetsSel;
    }
    // THE DEFERRED OPEN: the request records the plans + the flag
    // from the tile-menu / key paths (deeper or mid-close scopes);
    // the OPEN itself lives INSIDE DrawAssetDeleteModal, on the line
    // directly above the BeginPopupModal — the same function, the
    // same id stack BY CONSTRUCTION (no intervening statement can
    // ever shift the stack again).
    if (tileDeleteMulti && !m_assetsSelSet.empty())
        RequestAssetDelete(m_assetsSelSet.data(), m_assetsSelSet.size());
    else if (!tileDeleteRequested.empty())
        RequestAssetDelete(&tileDeleteRequested, 1);
    DrawAssetDeleteModal();
    // Popup is hosted at the same ID depth on every frame (outside
    // the grid child), including the context-menu request frame.
    if (openRename)
        ImGui::OpenPopup("Rename asset##browser");
    ui::PrepareSystemModal();
    if (ImGui::BeginPopupModal("Rename asset##browser",
                               nullptr,
                               ui::kSystemModalFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ui::SystemModalCloseRequested())
            ImGui::CloseCurrentPopup();
        else {
            ImGui::TextDisabled("Renaming may break path-based references.");
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText(
                "Name", renameName, sizeof(renameName), ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter || ImGui::Button("Rename")) {
                std::string result, err;
                if (RenameBrowserAsset(renameTarget, m_projectRoot, renameName, result, err)) {
                    const auto retarget = [&](std::string& path) {
                        if (path == renameTarget)
                            path = result;
                        else if (path.size() > renameTarget.size() &&
                                 path.compare(0, renameTarget.size(), renameTarget) == 0 &&
                                 path[renameTarget.size()] == '/')
                            path = result + path.substr(renameTarget.size());
                    };
                    retarget(m_assetsSel);
                    retarget(m_assetsAnchor);
                    retarget(m_assetsDir);
                    retarget(m_assetsExpandGltf);
                    retarget(m_assetsExpandTour);
                    for (auto& path : m_assetsSelSet)
                        retarget(path);
                    m_assetInspectPath.clear();
                    m_assetInspectMat.reset();
                    m_assetInspectMesh.reset();
                    m_assetInspectGiCook.reset();
                    m_assetInspectLmCook.reset();
                    if (m_assetDb.IsOpen()) {
                        smvk::cache::RescanStats stats;
                        std::string dbErr;
                        if (!m_assetDb.RescanProject(stats, dbErr))
                            Log("Rename: asset DB rescan failed: " + dbErr);
                    }
                    Log("Renamed asset to '" + result + "'.");
                    renameTarget.clear();
                    ImGui::CloseCurrentPopup();
                } else {
                    Log("Rename: " + err);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                renameTarget.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild(); // ##assets_root (non-scrolling header)
}

// THE delete request — ONE method, targets* + targetCount: the
// single delete is count 1; the multi passes the selection set. Each
// target validates (project containment, not the open scene,
// planable); the plans ride m_assetsDeletePending (a vector; a
// refused target logs + drops, the rest proceed) + the ONE confirm
// modal opens.

} // namespace editor
