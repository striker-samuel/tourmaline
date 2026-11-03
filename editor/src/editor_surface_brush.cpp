// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "editor_app.h"
#include "editor_surface_brush.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include <glm/gtc/matrix_inverse.hpp>

namespace editor {
namespace {
uint32_t ResolveStrokeParent(const Scene& scene, const surface::Stroke& stroke) {
    if (stroke.parentUuid.empty())
        return UINT32_MAX;
    for (uint32_t i = 0; i < scene.hierarchy.size(); ++i)
        if (IsNodeAlive(scene, i) && GetNodeUuid(scene, i) == stroke.parentUuid)
            return i;
    return UINT32_MAX;
}
}

void EditorApp::SurfaceBrushSample(int viewport, float x, float y) {
    if (!m_world || viewport < 0 || viewport >= ViewportCount())
        return;
    const EditorViewport& vp = m_viewports[viewport];
    const smvk::Extent2D ext{uint32_t(vp.rect.w), uint32_t(vp.rect.h)};
    if (!ext.width || !ext.height || x < 0 || y < 0 ||
        x >= vp.rect.w || y >= vp.rect.h)
        return;
    // The renderer's depth is reversed-Z. Existing PickNode uses the
    // same camera convention and returns the frontmost visible triangle,
    // avoiding a GPU synchronization/readback stall for each brush dab.
    const auto hit = PickNode(viewport, x, y, ext);
    if (!hit || hit->node >= m_world->scene.globalTransform.size() ||
        m_deletedNodes.contains(hit->node))
        return;
    Scene& scene = m_world->scene;
    const glm::mat4 parentWorld = scene.globalTransform[hit->node];
    // Mint an identity only for touched nodes. SaveTour already emits
    // SetUuid for metadata UUIDs, so this survives scene re-imports.
    std::string uuid = GetNodeUuid(scene, hit->node);
    if (uuid.empty()) {
        uuid = smvk::tour::GenerateUuid();
        SetNodeUuidRaw(scene, hit->node, uuid);
    }
    glm::vec3 origin;
    const glm::vec3 ray = MouseRay(x, y, ext, vp.viewProj, origin);
    const glm::vec3 point = origin + ray * hit->t;
    // One segment per hit parent. Moving across another object creates
    // a separate parented segment instead of joining unrelated frames.
    if (m_surfaceActive.points.empty() || m_surfaceActive.parentUuid != uuid) {
        if (!m_surfaceActive.points.empty())
            SurfaceBrushFinish();
        m_surfaceActive = {};
        m_surfaceActive.parentUuid = uuid;
        m_surfaceActive.mode = m_surfaceMode;
    }
    surface::AppendWorldHit(m_surfaceActive, point, parentWorld);
}

void EditorApp::SurfaceBrushFinish() {
    if (!m_surfaceActive.points.empty()) {
        if (m_surfaceStrokes.size() >= surface::kMaxStrokes)
            m_surfaceStrokes.erase(m_surfaceStrokes.begin());
        m_surfaceStrokes.push_back(std::move(m_surfaceActive));
        m_surfaceRedo.clear();
        MarkPlacementDirty();
        AutosaveTour();
    }
    m_surfaceActive = {};
    m_surfaceLastPx = glm::vec2(-1e9f);
}

void EditorApp::SurfaceBrushInput(int viewport, float x, float y,
                                  bool pressed, bool held, bool allowed) {
    if (!m_surfaceBrushEnabled || !m_world)
        return;
    if (!held) {
        if (m_surfaceDrawing)
            SurfaceBrushFinish();
        m_surfaceDrawing = false;
        return;
    }
    if (!m_surfaceDrawing && pressed && allowed) {
        m_surfaceDrawing = true;
        m_surfaceViewport = viewport;
        m_surfaceLastPx = glm::vec2(-1e9f);
    }
    if (!m_surfaceDrawing || viewport != m_surfaceViewport)
        return;
    if (!allowed) {
        // Still suppress scene-selection while dragging outside the viewport;
        // don't stamp into panels or on camera-navigation gestures.
        return;
    }
    const glm::vec2 px(x, y);
    const float step = std::clamp(m_surfaceSpacingPx, 2.0f, 128.0f);
    if (m_surfaceLastPx.x < -1e8f || glm::distance(px, m_surfaceLastPx) >= step) {
        SurfaceBrushSample(viewport, x, y);
        m_surfaceLastPx = px;
    }
}

void EditorApp::DrawSurfaceBrushPanel() {
    if (!m_surfaceBrushEnabled)
        return;
    bool open = m_surfaceBrushEnabled;
    ImGui::SetNextWindowSize(ImVec2(315, 180), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Surface Brush", &open)) {
        int mode = int(m_surfaceMode);
        if (ImGui::RadioButton("Points", mode == 0)) mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Spline", mode == 1)) mode = 1;
        m_surfaceMode = surface::Mode(mode);
        ImGui::SliderFloat("Spacing (px)", &m_surfaceSpacingPx, 2.0f, 64.0f, "%.0f");
        ImGui::TextDisabled("LMB: paint visible meshes | Alt: orbit | Esc: exit");
        ImGui::Text("%zu strokes (editor-only)", m_surfaceStrokes.size());
        if (ImGui::Button("Undo stroke") && !m_surfaceStrokes.empty()) {
            m_surfaceRedo.push_back(std::move(m_surfaceStrokes.back()));
            m_surfaceStrokes.pop_back();
            MarkPlacementDirty();
            AutosaveTour();
        }
        ImGui::SameLine();
        if (ImGui::Button("Redo stroke") && !m_surfaceRedo.empty()) {
            m_surfaceStrokes.push_back(std::move(m_surfaceRedo.back()));
            m_surfaceRedo.pop_back();
            MarkPlacementDirty();
            AutosaveTour();
        }
        if (ImGui::Button("Bake points as locators") && m_world) {
            // Ordinary undoable scene nodes, parented under the exact
            // surface object; positions are already parent-local.
            uint32_t created = 0;
            for (const surface::Stroke& s : m_surfaceStrokes) {
                const uint32_t parent = ResolveStrokeParent(m_world->scene, s);
                if (parent == UINT32_MAX || m_deletedNodes.contains(parent))
                    continue;
                for (const glm::vec3& local : s.points) {
                    EditCmd c{};
                    c.verb = EditVerb::CreateTransform;
                    c.node = UINT32_MAX;
                    c.node2 = parent;
                    c.afterText = UniqueLocatorName();
                    c.afterVec = local;
                    c.svec = glm::vec3(1.0f);
                    Do(c);
                    ++created;
                }
            }
            Log("Surface brush: baked " + std::to_string(created) +
                " parented locator(s).");
        }
        ImGui::TextDisabled("Bake points to scene nodes; spline guides stay editor-only.");
    }
    ImGui::End();
    if (!open) {
        SurfaceBrushFinish();
        m_surfaceBrushEnabled = false;
    }
}

void EditorApp::SaveSurfaceBrushToDoc(smvk::tour::Doc& doc) const {
    for (size_t i = 0; i < m_surfaceStrokes.size(); ++i) {
        const auto& stroke = m_surfaceStrokes[i];
        if (stroke.points.empty() || stroke.parentUuid.empty())
            continue;
        const std::string base = "surface.stroke." + std::to_string(i);
        doc.editorSettings.emplace_back(base + ".meta", surface::SerializeMeta(stroke));
        for (size_t j = 0; j < stroke.points.size(); ++j)
            doc.editorSettings.emplace_back(
                base + ".point." + std::to_string(j),
                surface::SerializePoint(stroke.points[j]));
    }
}

void EditorApp::LoadSurfaceBrushFromDoc(const smvk::tour::Doc& doc) {
    m_surfaceStrokes.clear();
    m_surfaceActive = {};
    m_surfaceRedo.clear();
    m_surfaceDrawing = false;
    // A temporary sparse map protects against out-of-order editorSettings.
    std::map<size_t, surface::Stroke> strokes;
    std::map<size_t, std::map<size_t, glm::vec3>> samples;
    constexpr char prefix[] = "surface.stroke.";
    for (const auto& [key, value] : doc.editorSettings) {
        if (key.rfind(prefix, 0) != 0)
            continue;
        const std::string suffix = key.substr(sizeof(prefix) - 1);
        const size_t dot = suffix.find('.');
        if (dot == std::string::npos)
            continue;
        size_t idx = 0;
        try {
            const std::string number = suffix.substr(0, dot);
            if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
                continue;
            idx = std::stoull(number);
        } catch (...) { continue; }
        if (idx >= surface::kMaxStrokes)
            continue;
        const std::string field = suffix.substr(dot + 1);
        if (field == "meta") {
            surface::Stroke s;
            if (surface::ParseMeta(value, s))
                strokes[idx] = std::move(s);
        } else if (field.rfind("point.", 0) == 0) {
            size_t pointIdx = 0;
            try {
                const std::string number = field.substr(6);
                if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
                    continue;
                pointIdx = std::stoull(number);
            } catch (...) { continue; }
            if (pointIdx >= surface::kMaxPointsPerStroke)
                continue;
            glm::vec3 p;
            if (surface::ParsePoint(value, p))
                samples[idx][pointIdx] = p;
        }
    }
    for (auto& [id, stroke] : strokes) {
        for (const auto& [index, point] : samples[id])
            stroke.points.push_back(point);
        if (!stroke.points.empty())
            m_surfaceStrokes.push_back(std::move(stroke));
    }
}

void EditorApp::DrawSurfaceBrushOverlay() {
    if (m_surfaceStrokes.empty() && m_surfaceActive.points.empty())
        return;
    if (!m_world)
        return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int vpIndex = 0; vpIndex < ViewportCount(); ++vpIndex) {
        const EditorViewport& vp = m_viewports[vpIndex];
        dl->PushClipRect(ImVec2(vp.rect.x, vp.rect.y),
            ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h), true);
        auto project = [&](const glm::vec3& p, ImVec2& out) {
            const glm::vec4 clip = vp.viewProj * glm::vec4(p, 1.0f);
            if (clip.w <= 1e-6f || clip.z < 0 || clip.z > clip.w)
                return false;
            out = ImVec2(vp.rect.x + (clip.x / clip.w * 0.5f + 0.5f) * vp.rect.w,
                         vp.rect.y + (clip.y / clip.w * 0.5f + 0.5f) * vp.rect.h);
            return true;
        };
        auto draw = [&](const surface::Stroke& stroke, ImU32 color) {
            const uint32_t parent = ResolveStrokeParent(m_world->scene, stroke);
            if (parent == UINT32_MAX || m_deletedNodes.contains(parent) ||
                parent >= m_world->scene.globalTransform.size() ||
                !IsVisibleNode(m_world->scene, int(parent)))
                return;
            const glm::mat4& transform = m_world->scene.globalTransform[parent];
            ImVec2 last{};
            bool lastValid = false;
            for (const glm::vec3& local : stroke.points) {
                ImVec2 screen{};
                bool valid = project(glm::vec3(transform * glm::vec4(local, 1.0f)), screen);
                if (valid) {
                    dl->AddCircleFilled(screen, stroke.mode == surface::Mode::Points ? 3.0f : 2.0f, color);
                    if (stroke.mode == surface::Mode::Spline && lastValid)
                        dl->AddLine(last, screen, color, 2.0f);
                }
                last = screen;
                lastValid = valid;
            }
        };
        for (const auto& s : m_surfaceStrokes)
            draw(s, IM_COL32(70, 235, 180, 225));
        if (!m_surfaceActive.points.empty())
            draw(m_surfaceActive, IM_COL32(255, 220, 110, 255));
        dl->PopClipRect();
    }
}

} // namespace editor
