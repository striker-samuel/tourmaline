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
void EditorApp::DrawOutliner() {
    // The scene graph. The old cooked-VFS/pak browsing tab is GONE:
    // paks run in the App (the editor is WYSIWYG authoring over source
    // assets); tools/pak_browser.py remains the standalone inspector.
    DrawSceneOutliner();
}

void EditorApp::ClearNonDagSelection() {
    m_nonDagSel = NonDagSel{};
    m_imageFocus = UINT32_MAX;
}

void EditorApp::SelectNonDag(NonDagKind kind, uint32_t index) {
    // Single selection, mutually exclusive with node/World picks
    // (SetSelection fans out to the non-DAG clear first).
    SetSelection(UINT32_MAX);
    m_nonDagSel.valid = true;
    m_nonDagSel.kind = kind;
    m_nonDagSel.index = index;
    if (kind == NonDagKind::Material && m_world && index < m_world->materialLocations.size()) {
        m_matEditorSel = index;
        if (m_materialFocus != index) {
            if (m_materialFocus != UINT32_MAX)
                m_materialTrail.push_back(m_materialFocus);
            m_materialFocus = index; // opens the material page
        }
    } else if (kind == NonDagKind::Image) {
        m_imageFocus = index; // Images-tab highlight (sticky)
    }
}

void EditorApp::CollectNonDagEntries(std::vector<NonDagEntry>& out) const {
    out.clear();
    // Future node sets: add a NonDagKind value + one block here; the
    // section rendering and picking below stay kind-driven.
    for (size_t i = 0; i < m_images.size(); ++i) {
        NonDagEntry e;
        e.kind = NonDagKind::Image;
        e.index = uint32_t(i);
        const std::string& name = m_images[i].name;
        std::string base = m_images[i].path;
        const size_t s = base.find_last_of("/\\");
        if (s != std::string::npos)
            base = base.substr(s + 1);
        e.label = "[file] " + (name.empty() ? base : name);
        out.push_back(std::move(e));
    }
    if (m_world) {
        for (uint32_t i = 0; i < uint32_t(m_world->materialLocations.size()); ++i) {
            const auto& loc = m_world->materialLocations[i];
            if (!loc.cluster || !loc.handle.valid())
                continue;
            NonDagEntry e;
            e.kind = NonDagKind::Material;
            e.index = i;
            e.label = "[mat] mat." + std::to_string(i) + " " +
                      std::string(loc.cluster->GetMaterialUri(loc.handle).Str());
            out.push_back(std::move(e));
        }
    }
}

void EditorApp::DrawNonDagSection() {
    SetupDefaultCameras();
    std::vector<NonDagEntry> entries;
    CollectNonDagEntries(entries);
    const bool filtering = m_filterBuf[0] != '\0';
    // Editor orbit cameras (virtual rows): click binds the ACTIVE
    // viewport (same as its chip combo). These are session view state,
    // never scene nodes — they don't select, don't cook, and stay out
    // of the shift-range order.
    auto drawEditorCamRow = [&](size_t ci) {
        const EditorCam& ec = m_editorCams[ci];
        const std::string label = "[editor] " + ec.name;
        if (!filtering || label.find(m_filterBuf) != std::string::npos) {
            ImGui::PushID(int(2000000 + ci));
            const bool active =
                m_viewports[m_activeVp].camIdx == int(ci) && m_viewports[m_activeVp].boundNode < 0;
            if (ImGui::Selectable(label.c_str(), active)) {
                m_viewports[m_activeVp].camIdx = int(ci);
                m_viewports[m_activeVp].boundNode = -1;
                Log("Viewport " + std::to_string(m_activeVp) + " orbit camera: '" + ec.name + "'.");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Editor orbit camera (session view "
                                  "state, never cooked). Click binds the active "
                                  "viewport; orbit with Alt+LMB/MMB/RMB.");
            ImGui::PopID();
        }
    };
    if (filtering) {
        // Flat rows (same child as the node hits).
        for (const NonDagEntry& e : entries) {
            if (e.label.find(m_filterBuf) == std::string::npos)
                continue;
            std::string label = e.label;
            if (e.kind == NonDagKind::Image) {
                std::error_code ec;
                if (!std::filesystem::exists(std::filesystem::path(m_images[size_t(e.index)].path),
                                             ec))
                    label += " [missing]";
            }
            ImGui::PushID(int(1000000 + int(e.kind) * 100000 + e.index));
            const bool sel =
                m_nonDagSel.valid && m_nonDagSel.kind == e.kind && m_nonDagSel.index == e.index;
            if (ImGui::Selectable(label.c_str(), sel))
                SelectNonDag(e.kind, e.index);
            ImGui::PopID();
        }
        for (size_t ci = 0; ci < m_editorCams.size(); ++ci)
            drawEditorCamRow(ci);
        return;
    }
    ImGui::PushID("nondag");
    const bool open = ImGui::TreeNodeEx(
        "##root", ImGuiTreeNodeFlags_DefaultOpen, "Non-DAG (%zu)", entries.size());
    if (!open) {
        ImGui::PopID();
        return;
    }
    auto drawGroup = [&](const char* title, NonDagKind kind) {
        size_t n = 0;
        for (const NonDagEntry& e : entries)
            if (e.kind == kind)
                ++n;
        if (n == 0)
            return;
        ImGui::PushID(title);
        const bool gopen =
            ImGui::TreeNodeEx("##g", ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu)", title, n);
        if (gopen) {
            for (const NonDagEntry& e : entries) {
                if (e.kind != kind)
                    continue;
                std::string label = e.label;
                if (e.kind == NonDagKind::Image) {
                    std::error_code ec;
                    if (!std::filesystem::exists(
                            std::filesystem::path(m_images[size_t(e.index)].path), ec))
                        label += " [missing]";
                }
                ImGui::PushID(int(e.index));
                const bool sel =
                    m_nonDagSel.valid && m_nonDagSel.kind == e.kind && m_nonDagSel.index == e.index;
                if (ImGui::Selectable(label.c_str(), sel))
                    SelectNonDag(e.kind, e.index);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    drawGroup("Images", NonDagKind::Image);
    drawGroup("Materials", NonDagKind::Material);
    drawGroup("World dressing", NonDagKind::Terrain);
    ImGui::PushID("editorcams");
    const bool eopen = ImGui::TreeNodeEx(
        "##g", ImGuiTreeNodeFlags_DefaultOpen, "Editor (%zu)", m_editorCams.size());
    if (eopen) {
        for (size_t ci = 0; ci < m_editorCams.size(); ++ci)
            drawEditorCamRow(ci);
        ImGui::TreePop();
    }
    ImGui::PopID();
    ImGui::TreePop();
    ImGui::PopID();
}

void EditorApp::DrawSceneOutliner() {
    if (!m_world || m_world->scene.hierarchy.empty()) {
        return;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "filter nodes...", m_filterBuf, sizeof(m_filterBuf));
    const bool filtering = m_filterBuf[0] != '\0';
    std::string selName = "-";
    if (m_worldSelected)
        selName = "World";
    else if (m_selected != UINT32_MAX)
        selName = getNodeName(m_world->scene, int(m_selected));
    ImGui::TextDisabled("nodes: %zu%s  selected: %s",
                        m_world->scene.hierarchy.size(),
                        m_deletedNodes.empty()
                            ? ""
                            : (" (" + std::to_string(m_deletedNodes.size()) + " deleted)").c_str(),
                        selName.c_str());
    ImGui::Separator();
    ImGui::Separator();
    m_outlinerOrder.clear(); // rebuilt below (tree XOR filter rows)
    ImGui::BeginChild("##tree");
    // World pseudo-node: global render settings. Pinned above the tree,
    // never part of it — no children, no drag&drop, no rename/delete.
    // (Filtered out by the node filter like everything else.)
    if (!filtering || std::string("World").find(m_filterBuf) != std::string::npos) {
        ImGui::PushID("worldnode");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.0f, 1.0f));
        if (ImGui::Selectable("World [globals]", m_worldSelected))
            SelectWorldNode();
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (!filtering)
            ImGui::Separator();
    }
    if (m_showNonDag && !filtering)
        DrawNonDagSection();
    if (filtering) {
        for (uint32_t n = 0; n < uint32_t(m_world->scene.hierarchy.size()); ++n) {
            if (m_deletedNodes.contains(n))
                continue; // deleted (Maya delete): out of the live scene
            if (InDisabledRefSubtree(n))
                continue; // disabled reference tree: not shown, not read
            const std::string name = getNodeName(m_world->scene, int(n));
            if (name.find(m_filterBuf) == std::string::npos)
                continue;
            m_outlinerOrder.push_back(n); // visible-row order (shift-range)
            ImGui::PushID(int(n));
            const bool sel = IsSelected(n);
            char label[300];
            std::snprintf(label,
                          sizeof(label),
                          "     %u: %s%s%s%s%s%s%s%s%s",
                          n,
                          name.empty() ? "(unnamed)" : name.c_str(),
                          FindNodeLight(n) ? " [light]" : "",
                          (FindCubemap(m_world->scene, n) != nullptr) ? " [cube]" : "",
                          (FindCamera(m_world->scene, n) != nullptr) ? " [camera]" : "",
                          (FindExport(m_world->scene, n) != nullptr) ? " [export]" : "",
                          IsEnvNode(n) ? " [env]" : "",
                          (IsLocatorNode(m_world->scene, n) && !IsRefPlaceholder(n)) ? " [locator]"
                                                                                     : "",
                          IsRefPlaceholder(n) ? " [ref]" : "",
                          !IsVisibleNode(m_world->scene, int(n)) ? " [hidden]" : "");
            const bool meshNode = FindMesh(m_world->scene, n) != nullptr;
            const bool giNode = FindGiVolume(m_world->scene, n) != nullptr;
            const IconType rowIcon =
                giNode ? IconType::GI : (meshNode ? IconType::Model : IconType::File);
            if (ImGui::Selectable((std::string(label) + "###outliner_node").c_str(), sel)) {
                // Click = replace; ctrl-click = toggle one;
                // shift-click = contiguous visible range.
                if (ImGui::GetIO().KeyShift)
                    SelectRange(n);
                else if (ImGui::GetIO().KeyCtrl)
                    ToggleSelection(n);
                else
                    SetSelection(n);
                m_revealNode = n;
            }
            // The node type lives before the label, not in a trailing [mesh]/[giv] tag.
            const ImVec2 rowPos = ImGui::GetItemRectMin();
            const float iconSide = ImGui::GetFontSize();
            DrawAssetIconFallback(ImGui::GetWindowDrawList(),
                                  ImVec2(rowPos.x + ImGui::GetStyle().FramePadding.x,
                                         rowPos.y + (ImGui::GetItemRectSize().y - iconSide) * 0.5f),
                                  iconSide,
                                  rowIcon);
            if (sel && n == m_revealNode)
                ImGui::SetScrollHereY(0.5f);
            ImGui::PopID();
        }
        if (m_showNonDag)
            DrawNonDagSection();
    } else {
        // Reveal a viewport pick: force-open the ancestor chain so the
        // selected node is visible, then scroll to it.
        std::unordered_set<uint32_t> expand;
        if (m_revealNode != UINT32_MAX) {
            uint32_t n = m_revealNode;
            while (n < uint32_t(m_world->scene.hierarchy.size())) {
                expand.insert(n);
                const int p = m_world->scene.hierarchy[n].parent;
                if (p < 0)
                    break;
                n = uint32_t(p);
            }
        }
        // FLAT WORLD: top level = every live ROOT (parent == -1), not
        // the children of a synthetic node 0 (removed with the synthetic
        // world root). Slot order keeps the tree stable across resolves.
        for (uint32_t n = 0; n < uint32_t(m_world->scene.hierarchy.size()); ++n) {
            if (m_world->scene.hierarchy[n].parent != -1)
                continue;
            if (m_deletedNodes.contains(n))
                continue; // deleted (Maya delete): out of the live scene
            if (InDisabledRefSubtree(n))
                continue; // disabled reference tree: not shown, not read
            DrawOutlinerNode(n, expand.empty() ? nullptr : &expand);
        }
    }
    ImGui::EndChild();
    m_revealNode = UINT32_MAX;
}

void EditorApp::DrawOutlinerNode(uint32_t node, const std::unordered_set<uint32_t>* expandSet) {
    if (m_deletedNodes.contains(node))
        return;                      // deleted (Maya delete): out of the live scene
    m_outlinerOrder.push_back(node); // visible-row order (shift-range)
    const Scene& scene = m_world->scene;
    const std::string name = getNodeName(scene, int(node));
    const bool hasMesh = FindMesh(scene, node) != nullptr;
    const bool sel = IsSelected(node);
    ImGui::PushID(int(node));
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (sel)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (scene.hierarchy[node].firstChild < 0)
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (expandSet && expandSet->contains(node))
        ImGui::SetNextItemOpen(true);
    const ::SceneLight* nodeLight = FindNodeLight(node);
    char lightTag[16] = "";
    if (nodeLight) {
        std::snprintf(lightTag,
                      sizeof(lightTag),
                      " [%s]",
                      nodeLight->type == ::SceneLight::Type::Directional ? "sun"
                      : nodeLight->type == ::SceneLight::Type::Spot      ? "spot"
                                                                         : "point");
    }
    char label[300];
    std::snprintf(label,
                  sizeof(label),
                  "      %s%s%s%s%s%s%s%s%s%s%s%s%s",
                  name.empty() ? "(node)" : name.c_str(),
                  lightTag,
                  (FindCubemap(scene, node) != nullptr) ? " [cube]" : "",
                  (FindCamera(scene, node) != nullptr) ? " [camera]" : "",
                  (FindExport(scene, node) != nullptr) ? " [export]" : "",
                  IsNoLightmapNode(scene, int(node)) ? " [noLM]" : "",
                  IsLightmapNode(scene, int(node)) ? " [LM]" : "",
                  IsDitherCloseupNode(scene, int(node)) ? " [dither]" : "",
                  IsStaticSubtreeRoot(scene, int(node)) ? " [static]" : "",
                  IsEnvNode(node) ? " [env]" : "",
                  (IsLocatorNode(scene, node) && !IsRefPlaceholder(node)) ? " [locator]" : "",
                  IsRefPlaceholder(node) ? " [ref]" : "",
                  !IsVisibleNode(scene, int(node))
                      ? (IsHiddenNode(scene, int(node)) ? " [hidden]" : " [hidden*]")
                      : "");
    const IconType rowIcon =
        FindGiVolume(scene, node) != nullptr
            ? IconType::GI
            : (hasMesh ? IconType::Model : (nodeLight ? IconType::Light : IconType::File));
    const bool open = ImGui::TreeNodeEx((std::string(label) + "###outliner_node").c_str(), flags);
    // TreeNodeEx owns the entire row (selection, reparent and expansion).
    // Paint the icon in its label padding without creating another widget.
    const ImVec2 rowPos = ImGui::GetItemRectMin();
    const float iconSide = ImGui::GetFontSize();
    DrawAssetIconFallback(ImGui::GetWindowDrawList(),
                          ImVec2(rowPos.x + ImGui::GetTreeNodeToLabelSpacing() + 1.0f,
                                 rowPos.y + (ImGui::GetItemRectSize().y - iconSide) * 0.5f),
                          iconSide,
                          rowIcon);
    // Drag source (reparent by drop).
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload("EDITOR_NODE", &node, sizeof(node));
        ImGui::Text("%s", label);
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("EDITOR_NODE")) {
            const uint32_t src = *static_cast<const uint32_t*>(p->Data);
            EditCmd c{};
            c.verb = EditVerb::Parent;
            c.node = src;
            c.node2 = node;
            c.beforeParent = uint32_t(m_world->scene.hierarchy[src].parent);
            Do(c);
            Log("Reparented node " + std::to_string(src) + " under " + std::to_string(node));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        // Click = replace; ctrl-click = toggle one; shift-click =
        // contiguous visible range from the primary.
        if (ImGui::GetIO().KeyShift)
            SelectRange(node);
        else if (ImGui::GetIO().KeyCtrl)
            ToggleSelection(node);
        else
            SetSelection(node);
    }
    if (sel && node == m_revealNode)
        ImGui::SetScrollHereY(0.5f);
    if (open) {
        // Disabled references keep their placeholder row (the anchor
        // for re-enabling) but the TREE is not shown — the nodes stay
        // in the scene (stable ids), just out of sight.
        if (m_disabledRefRoots.contains(node)) {
            ImGui::TextDisabled("    (reference disabled — %zu node(s) "
                                "hidden)",
                                RefSubtreeSizeForOutliner(node));
        } else {
            for (int c = scene.hierarchy[node].firstChild; c != -1;
                 c = scene.hierarchy[c].nextSibling)
                DrawOutlinerNode(uint32_t(c), expandSet);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// Whether `node` sits inside (or is the root of) a DISABLED reference
// subtree: the placeholder row stays, the tree does not.
bool EditorApp::InDisabledRefSubtree(uint32_t node) const {
    if (m_disabledRefRoots.empty() || !m_world || node >= m_world->scene.hierarchy.size())
        return false;
    // Walk ancestors (the placeholder itself counts as inside).
    for (uint32_t n = node; n < m_world->scene.hierarchy.size();) {
        if (m_disabledRefRoots.contains(n))
            return true;
        const int p = m_world->scene.hierarchy[n].parent;
        if (p < 0)
            return false;
        n = uint32_t(p);
    }
    return false;
}

// Subtree size for the disabled-reference hint (child count below the
// placeholder, excluding other deleted nodes).
size_t EditorApp::RefSubtreeSizeForOutliner(uint32_t root) const {
    if (!m_world || root >= m_world->scene.hierarchy.size())
        return 0;
    size_t n = 0;
    std::vector<uint32_t> stack;
    for (int c = m_world->scene.hierarchy[root].firstChild; c >= 0;
         c = m_world->scene.hierarchy[size_t(c)].nextSibling)
        stack.push_back(uint32_t(c));
    while (!stack.empty()) {
        const uint32_t v = stack.back();
        stack.pop_back();
        if (!m_deletedNodes.contains(v))
            ++n;
        for (int c = m_world->scene.hierarchy[v].firstChild; c >= 0;
             c = m_world->scene.hierarchy[size_t(c)].nextSibling)
            stack.push_back(uint32_t(c));
    }
    return n;
}

// ---- Diffuse texture preview ------------------------------------------
// Resolves the material's baseColor slot (values.textures.x, an RHI pool
// index) back to its asset URI via the owning cluster's texture registry,
// then CPU-decodes the stashed .ktx2 source (same libktx path as the GI
// bakers) into a small sRGB thumbnail. Single-entry cache: selections
// change rarely, decodes cost a few ms once.

} // namespace editor
