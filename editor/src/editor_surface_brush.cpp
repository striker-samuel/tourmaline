// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "editor_surface_brush.h"
#include "editor_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
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
} // namespace

void EditorApp::SurfaceBrushSample(int viewport, float x, float y) {
    if (!m_world || viewport < 0 || viewport >= ViewportCount())
        return;
    const EditorViewport& vp = m_viewports[viewport];
    const smvk::Extent2D ext{uint32_t(vp.rect.w), uint32_t(vp.rect.h)};
    if (!ext.width || !ext.height || x < 0 || y < 0 || x >= vp.rect.w || y >= vp.rect.h)
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

void EditorApp::SurfaceBrushInput(int viewport,
                                  float x,
                                  float y,
                                  bool pressed,
                                  bool held,
                                  bool allowed) {
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
        if (ImGui::RadioButton("Points", mode == 0))
            mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Spline", mode == 1))
            mode = 1;
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
        if (ImGui::Button("Use cloud in Placement Graph") && !m_surfaceStrokes.empty()) {
            // One-click path: paint -> PointCloud domain -> existing
            // density/prototype/scale/min-spacing placement pipeline.
            // Make a fresh graph so we never overwrite an authored one.
            m_placement.pushUndo();
            m_placement.addGraph("Surface Brush");
            if (placement::GraphDoc* graph = m_placement.current()) {
                uint32_t layer = 0;
                for (const auto& [id, node] : graph->nodes)
                    if (node.kind == placement::NodeKind::Layer)
                        layer = id;
                if (layer != 0) {
                    if (const placement::PLink* old = graph->inputLink(layer, "domain")) {
                        const uint32_t oldLink = old->id;
                        graph->removeLink(oldLink);
                    }
                    const uint32_t cloudNode =
                        graph->addNode(placement::NodeKind::PointCloud, 20, 120);
                    graph->addLink(cloudNode, "domain", layer, "domain");
                    m_placement.selected = int(cloudNode);
                }
            }
            m_placement.showGraph = true;
            m_placement.showOverlay = true;
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
            Log("Surface brush: baked " + std::to_string(created) + " parented locator(s).");
        }
        // Particle source graph: same parented brush data as PlacementGraph,
        // but a separate typed points -> filter -> emitter evaluation chain.
        ImGui::SeparatorText("Particle source graph");
        ImGui::TextDisabled("Typed plugs: points -> sources; stable node IDs");
        if (ImGui::Button("Add painted source")) {
            const uint32_t id = m_particleSourceGraph.AddSource(tour::particles::kPainted);
            m_particleSourceGraph.Connect(id, 2);
            m_particleSourceSelected = id;
            AutosaveTour();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add terrain source")) {
            const uint32_t id = m_particleSourceGraph.AddSource(tour::particles::kHeightfield);
            m_particleSourceGraph.Connect(id, 2);
            m_particleSourceSelected = id;
            AutosaveTour();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add placement source")) {
            const uint32_t id = m_particleSourceGraph.AddSource(tour::particles::kPlacement);
            m_particleSourceGraph.Connect(id, 2);
            m_particleSourceSelected = id;
            AutosaveTour();
        }
        for (const auto& [id, node] : m_particleSourceGraph.topology.nodes) {
            if (ImGui::RadioButton((std::to_string(id) + ": " + node.type).c_str(),
                                   m_particleSourceSelected == id))
                m_particleSourceSelected = id;
        }
        if (auto it = m_particleSourceGraph.attrs.find(m_particleSourceSelected);
            it != m_particleSourceGraph.attrs.end()) {
            auto& a = it->second;
            const std::string& type = m_particleSourceGraph.topology.nodes.at(it->first).type;
            bool edited = false;
            if (type == tour::particles::kPainted || type == tour::particles::kHeightfield ||
                type == tour::particles::kPlacement) {
                edited |= ImGui::InputInt("Source selector (-1 = all)", &a.selector);
                if (ImGui::Button("Insert height filter")) {
                    const uint32_t src = m_particleSourceSelected;
                    const uint32_t filter = m_particleSourceGraph.AddFilter();
                    for (auto link = m_particleSourceGraph.topology.links.begin();
                         link != m_particleSourceGraph.topology.links.end();) {
                        if (link->second.from == src && link->second.to == 2)
                            link = m_particleSourceGraph.topology.links.erase(link);
                        else
                            ++link;
                    }
                    m_particleSourceGraph.Connect(src, filter, "points");
                    m_particleSourceGraph.Connect(filter, 2);
                    m_particleSourceSelected = filter;
                    edited = true;
                }
            } else if (type == tour::particles::kHeightFilter) {
                edited |= ImGui::DragFloat("Minimum world Y", &a.minY, 0.1f);
                edited |= ImGui::DragFloat("Maximum world Y", &a.maxY, 0.1f);
            } else if (type == tour::particles::kEmitter) {
                int renderMode = int(a.renderMode);
                if (ImGui::Combo(
                        "Particle render type", &renderMode, "Billboard\0Mesh instances\0")) {
                    a.renderMode = uint32_t(renderMode);
                    edited = true;
                }
                if (a.renderMode == tour::particles::kMeshRender) {
                    int meshBin = int(a.meshId);
                    if (ImGui::SliderInt("Mesh prototype bin",
                                         &meshBin,
                                         0,
                                         int(smvk::kMeshParticlesMaxBins) - 1)) {
                        a.meshId = uint32_t(meshBin);
                        edited = true;
                    }
                    ImGui::TextDisabled(
                        "Uses indexed mesh prototypes registered with GPU Placement.");
                }
                edited |= ImGui::DragFloat("Total spawn rate (/s)", &a.rate, 0.1f, 0.0f, 500.0f);
                edited |= ImGui::DragFloat("Start size (m)", &a.sizeStart, 0.01f, 0.001f, 5.0f);
                edited |= ImGui::DragFloat("End size (m)", &a.sizeEnd, 0.01f, 0.001f, 8.0f);
                edited |= ImGui::DragFloat("Lifetime (s)", &a.lifetime, 0.1f, 0.1f, 20.0f);
                edited |= ImGui::DragFloat("Wind speed", &a.speed, 0.01f, 0.0f, 10.0f);
                edited |= ImGui::ColorEdit3("Dust tint", &a.color.x);
            }
            if (edited)
                AutosaveTour();
            if (m_particleSourceSelected > 2 && ImGui::Button("Remove selected node")) {
                m_particleSourceGraph.Remove(m_particleSourceSelected);
                m_particleSourceSelected = 1;
                AutosaveTour();
            }
        }
        for (const auto& [id, link] : m_particleSourceGraph.topology.links)
            ImGui::TextDisabled(
                "#%u.%s -> #%u.%s", link.from, link.output.c_str(), link.to, link.input.c_str());
        if (!m_particleSourceError.empty())
            ImGui::TextWrapped("Particle graph: %s", m_particleSourceError.c_str());
        ImGui::TextDisabled("PlacementGraph accepts points as GPU scatter candidates.");
        ImGui::TextDisabled("Baking locators is optional.");
    }
    ImGui::End();
    if (!open) {
        SurfaceBrushFinish();
        m_surfaceBrushEnabled = false;
    }
}

void EditorApp::UpdateParticleSourcePreview() {
    if (!m_wysiwyg || !m_wysiwygValid || !m_world)
        return;
    using namespace tour::particles;
    auto provider = [this](const std::string& type,
                           int selector,
                           std::vector<Point>& points,
                           std::string& error) -> bool {
        if (type == kPainted) {
            if (m_surfaceStrokes.empty())
                return true;
            smvk::placement::PlacementPointCloudData cloud;
            if (!BuildPlacementPointCloud(selector, cloud, error))
                return false;
            for (const auto& p : cloud.positions)
                points.push_back({p});
            return true;
        }
        if (type == kHeightfield) {
            // The editor's live terrain-group heightfield is the same grid
            // consumed by terrain painting and PlacementGraph previews.
            const smvk::placement::HeightfieldData* field = nullptr;
            uint32_t selectedGroup = UINT32_MAX;
            for (const auto& [id, group] : m_terrainGroups) {
                if (group.deleted || !group.data.heights.valid() ||
                    (selector >= 0 && uint32_t(selector) != id))
                    continue;
                // Unordered-map iteration is not an authoring order; use
                // the smallest stable world-node ID when selector == -1.
                if (id < selectedGroup) {
                    selectedGroup = id;
                    field = &group.data.heights;
                }
            }
            if (!field) {
                error = "Heightfield source requires a live terrain group";
                return false;
            }
            // Representative 4x4 grid: bounded by the GPU emitter budget.
            for (int z = 0; z < 4; ++z)
                for (int x = 0; x < 4; ++x) {
                    const float wx = field->origin.x + field->spacing.x * float(field->width - 1) *
                                                           (float(x) + 0.5f) / 4.0f;
                    const float wz = field->origin.z + field->spacing.y * float(field->height - 1) *
                                                           (float(z) + 0.5f) / 4.0f;
                    points.push_back({glm::vec3(wx, field->sampleHeight(wx, wz) + 0.35f, wz)});
                }
            return true;
        }
        if (type == kPlacement) {
            const auto& preview = m_placement.preview;
            if (!preview.ok || preview.dirty) {
                error = "Placement source needs an up-to-date PlacementGraph preview";
                return false;
            }
            for (const auto& candidate : preview.accepted)
                points.push_back({candidate.p});
            return true;
        }
        error = "Unregistered particle source: " + type;
        return false;
    };
    // Demo emitters keep their first five slots when enabled; reserve
    // remaining rows rather than silently exceeding MAX_PARTICLE_EMITTERS.
    const auto evaluated = Evaluate(m_particleSourceGraph, provider, 11);
    m_particleSourceError = evaluated.ok ? evaluated.warning : evaluated.error;
    m_wysiwyg->SetExternalParticleEmitters(evaluated.ok ? evaluated.emitters
                                                        : std::vector<smvk::ParticleEmitterGpu>{});
    m_wysiwyg->SetExternalMeshParticleEmitters(
        evaluated.ok ? evaluated.meshEmitters : std::vector<smvk::MeshParticleEmitterGpu>{});
}

void EditorApp::SaveSurfaceBrushToDoc(smvk::tour::Doc& doc) const {
    for (const auto& [id, node] : m_particleSourceGraph.topology.nodes) {
        const auto& a = m_particleSourceGraph.attrs.at(id);
        doc.editorSettings.emplace_back("particle.source.node." + std::to_string(id),
                                        tour::particles::SerializeAttributes(node.type, a));
    }
    for (const auto& [id, link] : m_particleSourceGraph.topology.links) {
        doc.editorSettings.emplace_back("particle.source.link." + std::to_string(id),
                                        std::to_string(link.from) + " " + link.output + " " +
                                            std::to_string(link.to) + " " + link.input);
    }
    for (size_t i = 0; i < m_surfaceStrokes.size(); ++i) {
        const auto& stroke = m_surfaceStrokes[i];
        if (stroke.points.empty() || stroke.parentUuid.empty())
            continue;
        const std::string base = "surface.stroke." + std::to_string(i);
        doc.editorSettings.emplace_back(base + ".meta", surface::SerializeMeta(stroke));
        for (size_t j = 0; j < stroke.points.size(); ++j)
            doc.editorSettings.emplace_back(base + ".point." + std::to_string(j),
                                            surface::SerializePoint(stroke.points[j]));
    }
}

void EditorApp::LoadSurfaceBrushFromDoc(const smvk::tour::Doc& doc) {
    m_surfaceStrokes.clear();
    m_surfaceActive = {};
    m_surfaceRedo.clear();
    m_surfaceDrawing = false;
    m_particleSourceGraph = tour::particles::Graph::Default();
    m_particleSourceSelected = 1;
    // Parse particle graph independently of painted stroke ordering.
    // Malformed topology falls back to the safe default, never a partial graph.
    tour::particles::Graph loaded;
    bool foundGraph = false;
    for (const auto& [key, value] : doc.editorSettings) {
        auto nodePrefix = std::string("particle.source.node.");
        auto linkPrefix = std::string("particle.source.link.");
        try {
            if (key.rfind(nodePrefix, 0) == 0) {
                uint32_t id = uint32_t(std::stoul(key.substr(nodePrefix.size())));
                std::string type;
                tour::particles::Attributes a;
                if (!id || !tour::particles::ParseAttributes(value, type, a))
                    continue;
                loaded.topology.nodes[id] = tour::particles::Graph::MakeNode(id, type);
                loaded.attrs[id] = a;
                foundGraph = true;
            } else if (key.rfind(linkPrefix, 0) == 0) {
                uint32_t id = uint32_t(std::stoul(key.substr(linkPrefix.size())));
                std::istringstream in(value);
                tour::graph::Link l{};
                std::string extra;
                if (!id || !(in >> l.from >> l.output >> l.to >> l.input) || (in >> extra))
                    continue;
                l.id = id;
                loaded.topology.links[id] = l;
            }
        } catch (...) {
            continue;
        }
    }
    if (foundGraph && tour::graph::Validate(loaded.topology).ok) {
        bool complete = true;
        for (const auto& [id, node] : loaded.topology.nodes)
            complete &= loaded.attrs.contains(id);
        if (complete)
            m_particleSourceGraph = std::move(loaded);
    }
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
        } catch (...) {
            continue;
        }
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
            } catch (...) {
                continue;
            }
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
                         ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h),
                         true);
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
                    dl->AddCircleFilled(
                        screen, stroke.mode == surface::Mode::Points ? 3.0f : 2.0f, color);
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
