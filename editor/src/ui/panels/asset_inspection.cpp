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
void EditorApp::RevealInExplorer(const std::string& absPath) {
    // explorer /select needs backslashes on Windows.
#ifdef _WIN32
    std::string win = absPath;
    for (char& c : win)
        if (c == '/')
            c = '\\';
    // "explorer /select," fails for non-existing paths: open the
    // parent folder instead, plain-open the file's dir as the last
    // resort. A non-zero "success" HINSTANCE just means it launched.
    std::error_code ec;
    if (std::filesystem::is_regular_file(absPath, ec)) {
        // /select,<path> pre-selects the file in an Explorer window.
        if (reinterpret_cast<INT_PTR>(::ShellExecuteA(
                nullptr, "open", "explorer", ("/select," + win).c_str(), nullptr, SW_SHOWNORMAL)) <=
            32)
            Log("Reveal failed: '" + absPath + "'.");
    } else {
        const std::string dir = std::filesystem::path(absPath).parent_path().string();
        if (reinterpret_cast<INT_PTR>(::ShellExecuteA(
                nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            Log("Reveal failed: '" + dir + "'.");
    }
#else
    Log("Reveal in Explorer is Windows-only.");
#endif
}

void EditorApp::DrawAssetGenericPage() {
    // Fallback for every browser entry without a specialized inspector.
    // Never read arbitrary files on every frame; keep previews bounded.
    namespace fs = std::filesystem;
    const fs::path path(m_assetsSel);
    if (m_assetGenericPath != m_assetsSel) {
        m_assetGenericPath = m_assetsSel;
        m_assetGenericPreview = ReadBrowserFilePreview(path);
        m_animationInspectDoc.reset();
        m_animationInspectError.clear();
        m_animationInspectWarnings.clear();
        m_animationInspectClip = 0;
        m_animationInspectTrack = 0;
    }
    // Animation assets have their own multi-clip inspector, not just a
    // truncated source preview. Parse once per asset selection.
    if (m_assetGenericPath == m_assetsSel) {
        const std::string ext = path.extension().string();
        if (ext == ".tourAnim" || ext == ".touranim") {
            if (!m_animationInspectDoc && m_animationInspectError.empty()) {
                m_animationInspectDoc.emplace();
                if (!tour::asset::LoadTourAnim(path.string(),
                                               *m_animationInspectDoc,
                                               m_animationInspectError,
                                               &m_animationInspectWarnings))
                    m_animationInspectDoc.reset();
            }
        } else {
            m_animationInspectDoc.reset();
            m_animationInspectError.clear();
            m_animationInspectWarnings.clear();
        }
    }
    const auto& preview = m_assetGenericPreview;
    ImGui::TextWrapped("%s", path.filename().string().c_str());
    ImGui::TextDisabled("%s", m_assetsSel.c_str());
    ImGui::Separator();
    ImGui::Text("Type: %s",
                preview.isDirectory
                    ? "Directory"
                    : (path.extension().empty() ? "File" : path.extension().string().c_str()));
    if (preview.readOk && !preview.isDirectory)
        ImGui::Text("Size: %llu bytes", static_cast<unsigned long long>(preview.fileSize));
    if (!preview.error.empty())
        ImGui::TextWrapped("%s", preview.error.c_str());
    if (preview.isDirectory) {
        ImGui::TextDisabled("Double-click in Assets to open this folder.");
        return;
    }
    ImGui::Separator();
    if (path.extension() == ".tour" && preview.readOk) {
        if (ImGui::Button("Edit as prefab world..."))
            RequestOpenPrefab(path.string());
        ImGui::SameLine();
        ImGui::TextDisabled("Opens a separate live-edit world");
    }
    if (!preview.readOk)
        return;
    if (m_animationInspectDoc || !m_animationInspectError.empty()) {
        ImGui::SeparatorText("Animation clips");
        if (!m_animationInspectError.empty()) {
            ImGui::TextWrapped("Cannot parse animation: %s", m_animationInspectError.c_str());
            if (ImGui::Button("Retry loading animation")) {
                m_animationInspectError.clear();
                m_animationInspectDoc.reset();
            }
            return;
        }
        const auto& doc = *m_animationInspectDoc;
        ImGui::Text("Source skeleton: %s",
                    doc.source.empty() ? "(not specified)" : doc.source.c_str());
        ImGui::Text("%zu clips", doc.clips.size());
        for (const auto& warning : m_animationInspectWarnings)
            ImGui::TextWrapped("Warning: %s", warning.c_str());
        if (doc.clips.empty()) {
            ImGui::TextDisabled("This animation file contains no clips.");
            return;
        }
        m_animationInspectClip = std::clamp(m_animationInspectClip, 0, int(doc.clips.size()) - 1);
        if (ImGui::BeginListBox("##animationClips",
                                ImVec2(-1, std::min(170.0f, 30.0f * doc.clips.size() + 8.0f)))) {
            for (int i = 0; i < int(doc.clips.size()); ++i) {
                const auto& clip = doc.clips[size_t(i)];
                ImGui::PushID(i);
                const std::string name =
                    clip.name.empty() ? "Unnamed clip " + std::to_string(i + 1) : clip.name;
                if (ImGui::Selectable(name.c_str(), i == m_animationInspectClip)) {
                    m_animationInspectClip = i;
                    m_animationInspectTrack = 0;
                }
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        const auto& clip = doc.clips[size_t(m_animationInspectClip)];
        ImGui::Text("Duration: %.3f s  |  %.1f FPS", clip.duration, clip.fps);
        ImGui::Text("%zu tracks", clip.tracks.size());
        if (ImGui::Button("Preview clip in prefab world")) {
            size_t previewCluster = 0;
            bool hasSkeleton = false;
            if (m_prefab.world) {
                for (size_t i = 0; i < m_prefab.world->clusters.size(); ++i) {
                    if (m_prefab.world->clusters[i]->hasSkeleton) {
                        previewCluster = i;
                        hasSkeleton = true;
                        break;
                    }
                }
            }
            if (hasSkeleton) {
                // Never modify the parent scene. Keep the original
                // prefab locals so switching and scrubbing remain pure.
                if (!m_prefabAnimationPreview || m_prefabAnimationClusterIndex != previewCluster)
                    m_prefabAnimationRest =
                        m_prefab.world->clusters[previewCluster]->scene.localTransform;
                m_prefabAnimationClusterIndex = previewCluster;
                m_prefabAnimationClip = clip;
                m_prefabAnimationSource = path.string();
                m_prefabAnimationTime = 0.0f;
                m_prefabAnimationPlaying = true;
                m_prefabAnimationPreview = true;
                m_prefab.world->clusters[previewCluster]->SetAnimClip(clip);
                m_showPrefabWindow = true;
            } else {
                ImGui::OpenPopup("##animationPreviewHelp");
            }
        }
        if (ImGui::BeginPopup("##animationPreviewHelp")) {
            ImGui::TextWrapped("Open a .tour prefab with a skinned mesh "
                               "and skeleton first (select its .tour asset and choose "
                               "'Edit as prefab world...'). Then preview this clip. "
                               "The clip joint IDs must match the prefab source skeleton.");
            ImGui::EndPopup();
        }
        ImGui::TextDisabled("Read-only tracks; select a clip to preview.");
        if (ImGui::BeginTable("##animationTracks",
                              4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(0, 175))) {
            ImGui::TableSetupColumn("Joint");
            ImGui::TableSetupColumn("Channel");
            ImGui::TableSetupColumn("Keys");
            ImGui::TableSetupColumn("Interpolation");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < clip.tracks.size(); ++i) {
                const auto& track = clip.tracks[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u %s", track.joint, track.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(track.channel == tour::asset::Channel::Rotate ? "Rotate"
                                       : track.channel == tour::asset::Channel::Scale
                                           ? "Scale"
                                           : "Translate");
                ImGui::TableNextColumn();
                ImGui::Text("%zu", track.keys.size());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(track.spline == tour::asset::Spline::Step ? "Step"
                                       : track.spline == tour::asset::Spline::Linear
                                           ? "Linear"
                                           : "Catmull-Rom");
            }
            ImGui::EndTable();
        }
        if (m_prefabAnimationPreview && m_prefabAnimationSource == path.string()) {
            ImGui::TextDisabled("Playback controls are in the Prefab World pane.");
        }
        return;
    }
    if (!preview.isText) {
        ImGui::TextDisabled("Binary or non-UTF-8 file (metadata only).");
        return;
    }
    if (path.extension() == ".shape") {
        // Per-selection editing state: changes are only persisted on Save.
        // A truncated preview cannot safely round-trip the source.
        static std::string shapePath;
        static tour::asset::ShapeAsset shape;
        static std::string shapeError;
        static bool shapeLoaded = false;
        static bool shapeDirty = false;
        if (shapePath != m_assetsSel) {
            shapePath = m_assetsSel;
            shapeLoaded =
                !preview.truncated && tour::asset::ParseShapeAsset(preview.text, shape, shapeError);
            shapeDirty = false;
            if (preview.truncated)
                shapeError = "Shape file exceeds the 64 KiB inspector limit.";
        }
        if (!shapeLoaded) {
            ImGui::TextWrapped("Shape parse error: %s", shapeError.c_str());
            return;
        }
        ImGui::Text(
            "Shape: %s%s", tour::asset::ShapeKindName(shape.kind), shapeDirty ? " (unsaved)" : "");
        // Lightweight CPU preview, intentionally independent of the
        // Vulkan scene: usable before line/point draw pipelines exist.
        // Project onto a fixed oblique view (X + 0.4 Z, Y - 0.3 Z).
        {
            tour::asset::ShapeGeometry geometry;
            std::string tessError;
            if (tour::asset::TessellateShape(shape, geometry, tessError) &&
                !geometry.vertices.empty()) {
                const float width =
                    std::max(80.0f, std::min(ImGui::GetContentRegionAvail().x, 440.0f));
                const float height = 180.0f;
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const size_t maxScan = std::min(geometry.vertices.size(), size_t(200000));
                float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
                auto project = [](const tour::asset::ShapePoint& p) {
                    return ImVec2(p.x + 0.4f * p.z, p.y - 0.3f * p.z);
                };
                for (size_t i = 0; i < maxScan; ++i) {
                    const ImVec2 p = project(geometry.vertices[i]);
                    minX = std::min(minX, p.x);
                    maxX = std::max(maxX, p.x);
                    minY = std::min(minY, p.y);
                    maxY = std::max(maxY, p.y);
                }
                const float scale = std::min((width - 24.0f) / std::max(0.01f, maxX - minX),
                                             (height - 24.0f) / std::max(0.01f, maxY - minY));
                auto screen = [&](uint32_t idx) {
                    const ImVec2 p = project(geometry.vertices[idx]);
                    return ImVec2(origin.x + width * 0.5f + (p.x - (minX + maxX) * 0.5f) * scale,
                                  origin.y + height * 0.5f - (p.y - (minY + maxY) * 0.5f) * scale);
                };
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(origin,
                                    ImVec2(origin.x + width, origin.y + height),
                                    IM_COL32(25, 28, 34, 255),
                                    5.0f);
                const ImU32 tint = IM_COL32(110, 205, 220, 255);
                if (geometry.topology == tour::asset::ShapeGeometry::Topology::Points) {
                    for (size_t i = 0; i < std::min(geometry.vertices.size(), size_t(2048)); ++i)
                        draw->AddCircleFilled(screen(static_cast<uint32_t>(i)), 2.0f, tint);
                } else if (geometry.topology == tour::asset::ShapeGeometry::Topology::Lines) {
                    for (size_t i = 0; i + 1 < geometry.indices.size() && i < 8192; i += 2)
                        draw->AddLine(screen(geometry.indices[i]),
                                      screen(geometry.indices[i + 1]),
                                      tint,
                                      1.4f);
                } else {
                    for (size_t i = 0; i + 2 < geometry.indices.size() && i < 12000; i += 3) {
                        const ImVec2 a = screen(geometry.indices[i]);
                        const ImVec2 b = screen(geometry.indices[i + 1]);
                        const ImVec2 c = screen(geometry.indices[i + 2]);
                        draw->AddLine(a, b, tint);
                        draw->AddLine(b, c, tint);
                        draw->AddLine(c, a, tint);
                    }
                }
                ImGui::Dummy(ImVec2(width, height));
            } else if (!tessError.empty())
                ImGui::TextDisabled("Preview: %s", tessError.c_str());
        }
        if (shape.kind == tour::asset::ShapeKind::Spline)
            shapeDirty |= ImGui::Checkbox("Closed", &shape.closed);
        int segments = static_cast<int>(shape.segments);
        if (ImGui::SliderInt("Segments", &segments, 1, 256)) {
            shape.segments = static_cast<uint32_t>(segments);
            shapeDirty = true;
        }
        if (shape.kind == tour::asset::ShapeKind::Sphere ||
            shape.kind == tour::asset::ShapeKind::Cylinder ||
            shape.kind == tour::asset::ShapeKind::Cone) {
            shapeDirty |= ImGui::DragFloat("Radius", &shape.radius, 0.02f, 0.01f, 10000.0f);
            if (shape.kind != tour::asset::ShapeKind::Sphere)
                shapeDirty |= ImGui::DragFloat("Height", &shape.height, 0.02f, 0.01f, 10000.0f);
        }
        if (shape.kind == tour::asset::ShapeKind::Spline ||
            shape.kind == tour::asset::ShapeKind::PointCloud) {
            ImGui::Text("Control points: %zu", shape.points.size());
            if (ImGui::Button("Add point") && shape.points.size() < 100000) {
                shape.points.push_back(shape.points.empty() ? tour::asset::ShapePoint{}
                                                            : shape.points.back());
                shapeDirty = true;
            }
            if (shape.points.size() > 256)
                ImGui::TextDisabled("Showing first 256 points (of %zu).", shape.points.size());
            ImGui::BeginChild("##shape_points", ImVec2(0, 220), true);
            const size_t shown = std::min(shape.points.size(), size_t(256));
            for (size_t i = 0; i < shown; ++i) {
                ImGui::PushID(static_cast<int>(i));
                float xyz[3] = {shape.points[i].x, shape.points[i].y, shape.points[i].z};
                if (ImGui::DragFloat3("XYZ", xyz, 0.02f)) {
                    shape.points[i] = {xyz[0], xyz[1], xyz[2]};
                    shapeDirty = true;
                }
                ImGui::SameLine();
                const bool canRemove =
                    shape.points.size() > (shape.kind == tour::asset::ShapeKind::PointCloud
                                               ? 1u
                                               : (shape.closed ? 3u : 2u));
                ImGui::BeginDisabled(!canRemove);
                if (ImGui::SmallButton("X")) {
                    shape.points.erase(shape.points.begin() + static_cast<std::ptrdiff_t>(i));
                    shapeDirty = true;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        std::string validation;
        const bool valid = tour::asset::ValidShape(shape, validation);
        if (!valid)
            ImGui::TextWrapped("Invalid shape: %s", validation.c_str());
        ImGui::BeginDisabled(!shapeDirty || !valid);
        if (ImGui::Button("Save shape")) {
            // Only this authored source file is written; no cooked
            // geometry or GPU resource is modified by the inspector.
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << tour::asset::SerializeShapeAsset(shape);
            file.close();
            if (file) {
                shapeDirty = false;
                m_assetGenericPreview = ReadBrowserFilePreview(path);
                Log("Saved shape: " + path.generic_string());
            } else {
                Log("Failed to save shape: " + path.generic_string());
            }
        }
        ImGui::EndDisabled();
        if (shape.kind != tour::asset::ShapeKind::Spline &&
            shape.kind != tour::asset::ShapeKind::PointCloud) {
            ImGui::SameLine();
            ImGui::BeginDisabled(!valid || shapeDirty);
            if (ImGui::Button("Export OBJ...")) {
                std::string obj, exportError;
                const fs::path destination =
                    UniqueAssetCreatePath(path.parent_path(), path.stem().string(), ".obj");
                if (tour::asset::ShapeToObj(shape, obj, exportError) && !destination.empty()) {
                    std::ofstream file(destination, std::ios::binary);
                    file << obj;
                    file.close();
                    if (file)
                        Log("Exported procedural OBJ: " + destination.generic_string());
                    else
                        Log("Failed to export OBJ: " + destination.generic_string());
                } else {
                    Log("OBJ export failed: " + exportError);
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            shapeLoaded = tour::asset::ParseShapeAsset(preview.text, shape, shapeError);
            shapeDirty = false;
        }
        return;
    }
    if (ClassifyBrowserInspector(path) == BrowserInspectorKind::SpriteSet) {
        ::tour::asset::SpriteSet sheet;
        std::string parseError;
        if (!::tour::asset::ParseSpriteSetText(preview.text, sheet, parseError)) {
            ImGui::TextWrapped("SpriteSet error: %s", parseError.c_str());
        } else {
            ImGui::Text("Texture: %s (%u x %u)", sheet.texture.c_str(), sheet.width, sheet.height);
            ImGui::Text(
                "%zu sprites / %zu animations", sheet.sprites.size(), sheet.animations.size());
            const float width = std::min(360.0f, ImGui::GetContentRegionAvail().x);
            const float scale = width / float(sheet.width);
            const float height = float(sheet.height) * scale;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(
                origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(28, 31, 37, 255));
            for (const auto& sprite : sheet.sprites) {
                const auto& r = sprite.rect;
                const ImVec2 a(origin.x + r.x * scale, origin.y + r.y * scale);
                const ImVec2 b(a.x + r.width * scale, a.y + r.height * scale);
                draw->AddRect(a, b, IM_COL32(120, 220, 170, 255));
                if (r.width * scale >= 40)
                    draw->AddText(a, IM_COL32(235, 240, 245, 255), sprite.name.c_str());
            }
            ImGui::Dummy(ImVec2(width, height));
            if (ImGui::BeginTable("##sprites",
                                  3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 160))) {
                ImGui::TableSetupColumn("Sprite");
                ImGui::TableSetupColumn("Rect");
                ImGui::TableSetupColumn("Pivot");
                ImGui::TableHeadersRow();
                for (const auto& sprite : sheet.sprites) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(sprite.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%u,%u %ux%u",
                                sprite.rect.x,
                                sprite.rect.y,
                                sprite.rect.width,
                                sprite.rect.height);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f, %.2f", sprite.pivotX, sprite.pivotY);
                }
                ImGui::EndTable();
            }
        }
    }
    if (path.extension() == ".statemachine") {
        if (ImGui::Button("Edit state machine graph...")) {
            if (!m_stateGraph.show || m_stateGraph.Path() != path.string())
                m_stateGraph.Open(path.string());
            m_stateGraph.show = true;
        }
        if (m_stateGraph.Dirty() && m_stateGraph.Path() == path.string())
            ImGui::TextDisabled("This graph has unsaved edits in the graph window.");
    }
    ImGui::TextDisabled("Text preview (read-only)%s", preview.truncated ? " - first 64 KiB" : "");
    ImGui::BeginChild(
        "##assettextpreview", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(preview.text.c_str(), preview.text.c_str() + preview.text.size());
    ImGui::EndChild();
}

void EditorApp::DrawAssetReadonlyPage() {
    // READ-ONLY asset inspector (a .material/.mesh/.gicook/.lmcook click
    // in the asset browser): view parameters only — edits happen on
    // SCENES (the tour-level overrides/assignments), never on the
    // derived/cook assets (derived assets regenerate on reimport; cook
    // assets rebake through the bake execution). The loaded asset caches
    // per selection (never per frame: .mesh blobs can be megabytes).
    // EXCEPTION — .texture (user-authored, not derived): its IMPORT
    // SETTINGS are the authoring, so LDR sources edit them here (a
    // KTX2 source pre-decided them in its header — read-only rows).
    const auto kind = ClassifyBrowserInspector(m_assetsSel);
    const bool wantTex = kind == BrowserInspectorKind::Texture;
    const bool wantMat = kind == BrowserInspectorKind::Material;
    const bool wantMesh = kind == BrowserInspectorKind::Geometry;
    const bool wantGi = kind == BrowserInspectorKind::GiCook;
    const bool wantLm = kind == BrowserInspectorKind::LmCook;
    const bool wantCm = kind == BrowserInspectorKind::Cubemap;
    const bool wantGiv = kind == BrowserInspectorKind::Givolume;
    if (!wantTex && !wantMat && !wantMesh && !wantGi && !wantLm && !wantCm && !wantGiv)
        return;
    // Cache refresh on selection change (or a failed load retries never:
    // a re-derive/re-bake + re-click reloads).
    if (m_assetInspectPath != m_assetsSel) {
        m_assetInspectPath = m_assetsSel;
        m_assetInspectMat.reset();
        m_assetInspectMesh.reset();
        m_assetInspectGiCook.reset();
        m_assetInspectLmCook.reset();
        m_assetInspectCubemap.reset();
        m_assetInspectGivolume.reset();
        m_assetInspectTex.reset();
        m_assetInspectTexInfoOk = false;
        std::string err;
        if (wantTex) {
            m_assetInspectTex.emplace();
            if (!smvk::assets::LoadTextureAsset(m_assetsSel, *m_assetInspectTex, err)) {
                Log("Assets: texture inspect failed: " + err);
                m_assetInspectTex.reset();
            } else {
                // Probe the SOURCE (beside the .texture): the read-only
                // header facts for KTX2, the dims for LDR.
                std::ifstream in(std::filesystem::path(m_assetsSel).parent_path() /
                                     m_assetInspectTex->source,
                                 std::ios::binary);
                if (in) {
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                               std::istreambuf_iterator<char>());
                    m_assetInspectTexInfoOk =
                        ProbeTextureSource(bytes.data(), bytes.size(), m_assetInspectTexInfo);
                }
                if (!m_assetInspectTexInfoOk)
                    Log("Assets: texture source not probeable: " + m_assetInspectTex->source);
            }
        } else if (wantMat) {
            m_assetInspectMat.emplace();
            if (!smvk::assets::LoadMaterialAsset(m_assetsSel, *m_assetInspectMat, err)) {
                Log("Assets: material inspect failed: " + err);
                m_assetInspectMat.reset();
            }
        } else if (wantMesh) {
            m_assetInspectMesh.emplace();
            if (!smvk::assets::LoadMeshAsset(m_assetsSel, *m_assetInspectMesh, err)) {
                Log("Assets: mesh inspect failed: " + err);
                m_assetInspectMesh.reset();
            }
        } else if (wantGi) {
            m_assetInspectGiCook.emplace();
            if (!smvk::cook::LoadGiCookAsset(m_assetsSel, *m_assetInspectGiCook, err)) {
                Log("Assets: GI cook inspect failed: " + err);
                m_assetInspectGiCook.reset();
            }
        } else if (wantCm) {
            m_assetInspectCubemap.emplace();
            if (!smvk::cook::LoadCubemapAsset(m_assetsSel, *m_assetInspectCubemap, err)) {
                Log("Assets: cubemap inspect failed: " + err);
                m_assetInspectCubemap.reset();
            }
        } else if (wantGiv) {
            m_assetInspectGivolume.emplace();
            if (!smvk::cook::LoadGivolumeAsset(m_assetsSel, *m_assetInspectGivolume, err)) {
                Log("Assets: GI volume inspect failed: " + err);
                m_assetInspectGivolume.reset();
            }
        } else {
            m_assetInspectLmCook.emplace();
            if (!smvk::cook::LoadLightmapCookAsset(m_assetsSel, *m_assetInspectLmCook, err)) {
                Log("Assets: lightmap cook inspect failed: " + err);
                m_assetInspectLmCook.reset();
            }
        }
    }
    // TEXTURE ASSET page: user-authored import settings (the authoring
    // surface, not read-only). KTX2 sources show the header-decided rows
    // READ-ONLY; LDR sources edit them (saved into the .texture; applied
    // at the next import/cook — materials referencing this asset pick the
    // settings up through ImportTextureAssetToCluster).
    if (m_assetInspectTex) {
        smvk::assets::TextureAsset& t = *m_assetInspectTex;
        const TextureSourceInfo& info = m_assetInspectTexInfo;
        const bool infoOk = m_assetInspectTexInfoOk;
        ImGui::Text("Texture asset: '%s'",
                    t.name.empty() ? std::filesystem::path(m_assetsSel).stem().string().c_str()
                                   : t.name.c_str());
        ImGui::TextDisabled("guid %s", t.guid.c_str());
        ImGui::TextDisabled("source %s (%s)",
                            t.source.c_str(),
                            infoOk ? (info.isKtx2 ? "KTX2" : "LDR image") : "not probeable");
        if (infoOk)
            ImGui::TextDisabled(
                "%ux%u%s",
                info.width,
                info.height,
                info.isKtx2 ? ""
                            : (info.components ? (info.components == 4 ? " RGBA" : " RGB") : ""));
        ImGui::Separator();
        if (infoOk && info.isKtx2) {
            // The .ktx2 pre-decided these in its header — read-only rows.
            ImGui::TextDisabled("colorSpace: %s (from the .ktx2 header)",
                                info.srgb ? "sRGB" : "linear");
            ImGui::TextDisabled("format: %s (vkFormat %u)",
                                info.bcn ? "BCn block-compressed"
                                         : (info.vkFormat == 37
                                                ? "rgba8"
                                                : (info.vkFormat == 43 ? "rgba8 sRGB" : "other")),
                                info.vkFormat);
            if (info.basis)
                ImGui::TextDisabled("payload: Basis-family (the cook "
                                    "transcodes to per-semantic BCn)");
            ImGui::TextDisabled("mip levels: %u (the .ktx2 chain)", info.mipLevels);
            ImGui::Separator();
            ImGui::TextWrapped("The KTX2 source carries its own encoding: "
                               "colorSpace, format and mip levels are read-only. The "
                               "sampler rows still apply.");
        } else {
            // LDR source: the import settings ARE the authoring (the
            // sampler rows below are shared with KTX2 sources).
            bool changed = false;
            {
                const char* items[] = {"sRGB (color)", "linear (data)"};
                int cs = t.colorspace == 1 ? 1 : 0;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("colorSpace##texasset", &cs, items, 2)) {
                    t.colorspace = cs;
                    changed = true;
                }
            }
            {
                const char* items[] = {"rgba8 (uncompressed)",
                                       "bc1 (RGB)",
                                       "bc3 (RGBA)",
                                       "bc4 (R)",
                                       "bc5 (RG)",
                                       "bc7 (RGBA hq)"};
                int fmt = std::clamp(t.format, 0, 5);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("format##texasset", &fmt, items, 6)) {
                    t.format = fmt;
                    changed = true;
                }
            }
            {
                int mips = t.mipLevels;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputInt("mip levels (0 = auto)##texasset", &mips, 1, 1)) {
                    t.mipLevels = std::clamp(mips, 0, 32);
                    changed = true;
                }
            }
            if (changed) {
                std::string serr;
                if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
                    Log("Texture asset saved: " +
                        std::filesystem::path(m_assetsSel).filename().string() +
                        " (applies at the next import/cook).");
                else
                    Log("Texture asset save failed: " + serr);
            }
        }
        // Sampler rows apply to BOTH source kinds (never baked into a
        // KTX2 payload; they ride the pak's per-texture sampler record).
        {
            const char* items[] = {"linear", "nearest"};
            int f = t.filter != 0 ? 1 : 0;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("filter##texassetsmp", &f, items, 2)) {
                t.filter = f;
                std::string serr;
                if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
                    Log("Texture asset saved: " +
                        std::filesystem::path(m_assetsSel).filename().string() + " (sampler).");
            }
            const char* wraps[] = {"repeat", "clamp", "mirror"};
            int w = std::clamp(t.wrap, 0, 2);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("wrap##texassetsmp", &w, wraps, 3)) {
                t.wrap = w;
                std::string serr;
                if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
                    Log("Texture asset saved: " +
                        std::filesystem::path(m_assetsSel).filename().string() + " (sampler).");
            }
        }
        ImGui::Separator();
        ImGui::TextWrapped("Materials reference this asset by file name: "
                           "tex.baseColor=<name>.texture in a .material (the import "
                           "routes through these settings). The cook embeds the "
                           "converted payload as KTX2 in the pak.");
        return;
    }
    ImGui::TextDisabled("Asset (read-only) - edits happen on scenes");
    ImGui::Separator();
    if (m_assetInspectMat) {
        const smvk::assets::MaterialAsset& m = *m_assetInspectMat;
        ImGui::Text("Material: '%s'", m.name.c_str());
        ImGui::TextDisabled("guid %s", m.guid.c_str());
        const glm::vec4& bc = m.info.baseColorFactor;
        ImGui::ColorEdit4("Base color##ro",
                          const_cast<float*>(&bc.x),
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker |
                              ImGuiColorEditFlags_NoAlpha);
        ImGui::TextDisabled(
            "metallic: %.2f  roughness: %.2f", m.info.metallicFactor, m.info.roughnessFactor);
        const glm::vec3& em = m.info.emissiveFactor;
        ImGui::ColorEdit3("Emissive##ro",
                          const_cast<float*>(&em.x),
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker);
        const char* alpha = (m.info.alphaMode == smvk::AlphaMode::Mask)    ? "mask"
                            : (m.info.alphaMode == smvk::AlphaMode::Blend) ? "blend"
                                                                           : "opaque";
        ImGui::TextDisabled("alpha: %s (%.2f cutoff)  %s",
                            alpha,
                            m.info.alphaCutoff,
                            m.info.doubleSided ? "double-sided" : "single-sided");
        ImGui::Separator();
        ImGui::Text("Textures");
        auto tex = [&](const char* slot, const smvk::assets::MaterialAsset::TexRef& t) {
            ImGui::TextDisabled("%-18s %s", slot, t.path.empty() ? "-" : t.path.c_str());
        };
        tex("baseColor", m.baseColor);
        tex("metallicRoughness", m.metallicRoughness);
        tex("normal", m.normalTex);
        tex("occlusion", m.occlusion);
        tex("emissive", m.emissiveTex);
        ImGui::Separator();
        ImGui::TextDisabled("Assign: drag onto a viewport (selection) or "
                            "use a scene's material override.");
    } else if (m_assetInspectMesh) {
        const smvk::assets::MeshAsset& k = *m_assetInspectMesh;
        ImGui::Text("Mesh: '%s'", std::filesystem::path(m_assetsSel).stem().string().c_str());
        uint64_t tris = 0, verts = 0;
        for (const auto& g : k.geometry) {
            verts += g.positions.size();
            tris += g.indices.size() / 3;
        }
        ImGui::TextDisabled("%u nodes, %u instances, %zu blobs, "
                            "%llu verts, %llu tris",
                            uint32_t(k.nodes.size()),
                            uint32_t(k.instances.size()),
                            k.geometry.size(),
                            (unsigned long long)verts,
                            (unsigned long long)tris);
        uint32_t lights = uint32_t(k.lights.size());
        uint32_t cams = uint32_t(k.cameras.size());
        ImGui::TextDisabled("%u light(s), %u camera(s)", lights, cams);
        ImGui::Separator();
        ImGui::Text("Hierarchy (first 32)");
        ImGui::BeginChild("##meshnodes", ImVec2(0, 0), true);
        const size_t n = std::min(k.nodes.size(), size_t(32));
        for (size_t i = 0; i < n; ++i) {
            ImGui::TextDisabled("%2zu p%-4d %s",
                                i,
                                k.nodes[i].parent,
                                k.names[i].empty() ? "(node)" : k.names[i].c_str());
        }
        if (k.nodes.size() > n)
            ImGui::TextDisabled("... (%zu more)", k.nodes.size() - n);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::TextDisabled("Drop onto a viewport to add a scene "
                            "reference; the tour binds it by GUID.");
        if (m_world && !m_cookRunning && ImGui::Button("Add to scene")) {
            if (AddSceneReference(m_assetsSel, {}, true))
                Log("Assets: added geometry asset '" + m_assetsSel + "' (asset bind).");
        }
    } else if (m_assetInspectGiCook) {
        // GI cook asset (settings + blob result): the Bake GI execution
        // that fills the blob is the follow-up — this page only reports
        // WHAT would bake and WHERE the result points.
        const smvk::cook::GiCookAsset& c = *m_assetInspectGiCook;
        ImGui::Text("GI cook: '%s'", c.name.c_str());
        ImGui::TextDisabled("guid %s", c.guid.c_str());
        ImGui::TextDisabled("scene: %s", c.scene.empty() ? "(unbound)" : c.scene.c_str());
        ImGui::TextDisabled("convex volumes: %s   (the world-field "
                            "flag is retired with the octree)",
                            c.giVolumes ? "on" : "off");
        ImGui::Separator();
        if (c.blobGuid.empty()) {
            ImGui::TextDisabled("blob: unbaked (no result yet)");
        } else {
            const bool stale = smvk::cook::GiCookBlobStale(c);
            ImGui::TextDisabled("blob: %s (%s)%s",
                                c.blobGuid.c_str(),
                                c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
                                stale ? "  STALE (settings changed)" : "  fresh");
        }
    } else if (m_assetInspectLmCook) {
        // Lightmap cook asset (settings + blob result): same contract —
        // the atlas bake that fills the blob is the follow-up.
        const smvk::cook::LightmapCookAsset& c = *m_assetInspectLmCook;
        ImGui::Text("Lightmap cook: '%s'", c.name.c_str());
        ImGui::TextDisabled("guid %s", c.guid.c_str());
        ImGui::TextDisabled("scene: %s", c.scene.empty() ? "(unbound)" : c.scene.c_str());
        ImGui::TextDisabled("density: %.3f m/texel  lobes: %u", c.lmMetersPerTexel, c.lmLobes);
        ImGui::TextDisabled(
            "atlas width: %u  rays: %u  proxy: %d", c.lmAtlasWidth, c.lmRays, c.lmProxyRes);
        if (c.lmExcludeSmall > 0.0f || !c.lmExcludeKw.empty()) {
            ImGui::TextDisabled(
                "exclude: small < %.3f  kw: '%s'", c.lmExcludeSmall, c.lmExcludeKw.c_str());
        }
        ImGui::TextDisabled("sun: az %.1f el %.1f int %.0f  sky %.0f",
                            c.sunAzimuth,
                            c.sunElevation,
                            c.sunIntensity,
                            c.skyIntensity);
        ImGui::Separator();
        if (c.blobGuid.empty()) {
            ImGui::TextDisabled("blob: unbaked (no result yet)");
        } else {
            const bool stale = smvk::cook::LightmapCookBlobStale(c);
            ImGui::TextDisabled("blob: %s (%s)%s",
                                c.blobGuid.c_str(),
                                c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
                                stale ? "  STALE (settings changed)" : "  fresh");
        }
    } else if (m_assetInspectCubemap) {
        const smvk::cook::CubemapAsset& c = *m_assetInspectCubemap;
        ImGui::Text("Cubemap: '%s'", c.name.c_str());
        ImGui::TextDisabled("guid %s", c.guid.c_str());
        ImGui::TextDisabled("scene: %s", c.scene.empty() ? "(unbound)" : c.scene.c_str());
        ImGui::TextDisabled("capture: %s  size: %d  rays: %d",
                            c.capture.empty() ? "(scene stem)" : c.capture.c_str(),
                            c.size,
                            c.rays);
        ImGui::Separator();
        if (c.blobGuid.empty()) {
            ImGui::TextDisabled("blob: unbaked (no result yet)");
        } else {
            const bool stale = smvk::cook::CubemapBlobStale(c);
            ImGui::TextDisabled("blob: %s (%s)%s",
                                c.blobGuid.c_str(),
                                c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
                                stale ? "  STALE (settings changed)" : "  fresh");
        }
    } else if (m_assetInspectGivolume) {
        const smvk::cook::GivolumeAsset& c = *m_assetInspectGivolume;
        ImGui::Text("GI volume: '%s'", c.name.c_str());
        ImGui::TextDisabled("guid %s", c.guid.c_str());
        ImGui::TextDisabled("scene: %s", c.scene.empty() ? "(unbound)" : c.scene.c_str());
        ImGui::TextDisabled("node: %u  priority: %u", c.node, c.priority);
        ImGui::Separator();
        if (c.blobGuid.empty()) {
            ImGui::TextDisabled("blob: unbaked (no result yet)");
        } else {
            const bool stale = smvk::cook::GivolumeBlobStale(c);
            ImGui::TextDisabled("blob: %s (%s)%s",
                                c.blobGuid.c_str(),
                                c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
                                stale ? "  STALE (settings changed)" : "  fresh");
        }
    } else {
        ImGui::TextDisabled("(asset load failed - see the console)");
    }
}

void EditorApp::DrawImportableTexturePage() {
    // IMPORTABLE SOURCE page: a supported image format selected in the
    // browser but not yet imported as a .texture asset. Probes the file
    // (dims + kind) and offers the import button — the .texture wrap
    // carries the import settings (colorSpace/format/mipLevels/filter/
    // wrap) from here on. A project is required (the .texture lands
    // beside the source, adopted into the assetdb).
    namespace fs = std::filesystem;
    ImGui::Text("Texture source: '%s'", fs::path(m_assetsSel).filename().string().c_str());
    ImGui::TextDisabled("%s", m_assetsSel.c_str());
    ImGui::Separator();
    // Probe on selection change (cached like the asset pages).
    if (m_assetInspectPath != m_assetsSel) {
        m_assetInspectPath = m_assetsSel;
        m_assetInspectTex.reset();
        m_assetInspectTexInfoOk = false;
        std::ifstream in(m_assetsSel, std::ios::binary);
        if (in) {
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            m_assetInspectTexInfoOk =
                ProbeTextureSource(bytes.data(), bytes.size(), m_assetInspectTexInfo);
        }
        if (!m_assetInspectTexInfoOk)
            Log("Assets: texture source not probeable: " + m_assetsSel);
    }
    if (m_assetInspectTexInfoOk) {
        const TextureSourceInfo& info = m_assetInspectTexInfo;
        ImGui::TextDisabled("%ux%u %s",
                            info.width,
                            info.height,
                            info.isKtx2 ? "KTX2 (settings read-only at import)"
                                        : (info.components == 4 ? "RGBA" : "RGB"));
        ImGui::TextWrapped("Importing wraps this source in a .texture "
                           "asset: import settings (colorSpace, format, mip levels, "
                           "filter, wrap) %s, identity (.meta + assetdb), and "
                           "materials reference it as tex.<slot>=<name>.texture.",
                           info.isKtx2 ? "read from the .ktx2 header (it pre-decided them)"
                                       : "you author in the Inspector after the import");
    }
    ImGui::Separator();
    ImGui::BeginDisabled(m_projectRoot.empty());
    if (ImGui::Button("Import as .texture")) {
        // The .texture lands beside the source (same dir — the copy in
        // CreateTextureAsset is a no-op for an in-project file).
        const std::string dir = fs::path(m_assetsSel).parent_path().generic_string();
        CreateTextureAsset(m_assetsSel, dir);
    }
    ImGui::EndDisabled();
    if (m_projectRoot.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("Open or create a project first (File > New / "
                          "Open Project...).");
}

// DCC edits the authoring file; the explicit build stage exports
// a validated glTF publication. The content server observes the
// atomic replace and routes it through normal selective reimport.

} // namespace editor
