// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "editor_app.h"
#include <Engine/Geometry/intersection_curves.h>
#include <Engine/Geometry/cgal_intersection.h>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace editor {

// Offline, non-destructive tool. Writes ordinary source OBJ files to the
// project content directory; the existing import/cook pipeline owns them.
// Extracted curve state is transient so no .tour schema changes are needed.
void EditorApp::DrawIntersectionToolsWindow() {
    if (!m_showIntersectionToolsPanel) return;
    namespace geo = smvk::geometry;
    static char sourceA[1024] = "";
    static char sourceB[1024] = "";
    static char output[1024] = "";
    static int mode = 0;       // terrain or mesh/mesh
    static int product = 0;    // terrain ribbon or vertical skirt
    static geo::BakeSettings settings;
    static geo::CurveSet curves;
    static geo::RibbonMesh geometry;
    static std::string status;
    static bool hasCurves = false;
    static uint32_t bakedGroup = UINT32_MAX;

    ImGui::SetNextWindowSize(ImVec2(620, 610),ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Intersection Curves / Seam Bake",
        &m_showIntersectionToolsPanel)) { ImGui::End(); return; }

    const char* modes[] = {"Mesh / painted terrain","Mesh / mesh (CGAL)"};
    ImGui::Combo("Intersection backend",&mode,modes,2);
    ImGui::InputText("Source mesh A (.obj)",sourceA,sizeof(sourceA));
    if (mode == 1) ImGui::InputText("Source mesh B (.obj)",sourceB,sizeof(sourceB));
    const uint32_t selectedGroup = TerrainGroupOf(m_selected);
    const uint32_t terrainGroup = (selectedGroup != UINT32_MAX)
        ? selectedGroup : m_worldMapGroup;
    const auto groupIt = m_terrainGroups.find(terrainGroup);
    const bool terrainReady = groupIt != m_terrainGroups.end() &&
        !groupIt->second.deleted && groupIt->second.data.heights.valid();
    if (mode == 0) {
        if (terrainReady) ImGui::Text("Terrain group: %u (painted heightfield)",
                                      terrainGroup);
        else ImGui::TextDisabled("Select a terrain group/tile in the Outliner "
                                 "or World Map before extracting.");
    }
    const char* products[] = {"Terrain-conforming blend ribbon","Vertical skirt"};
    ImGui::Combo("Bake product",&product,products,2);
    if (mode == 1 && product == 0)
        ImGui::TextDisabled("Mesh/mesh curves can produce a skirt; terrain "
                            "conforming ribbons require a terrain receiver.");
    ImGui::DragFloat("Ribbon width (m)",&settings.width,.05f,.01f,100.0f);
    ImGui::DragFloat("Sample spacing (m)",&settings.step,.02f,.01f,10.0f);
    ImGui::DragFloat("Weld tolerance (m)",&settings.weldTolerance,.001f,
                     .0001f,1.0f,"%.4f");
    if (product == 0) ImGui::DragFloat("Terrain lift (m)",&settings.lift,
                                      .001f,0.0f,.25f,"%.3f");
    if (product == 1) ImGui::DragFloat("Skirt depth (m)",&settings.skirtDepth,
                                      .05f,.01f,100.0f);

    if (ImGui::Button("Extract intersection curves")) {
        geo::TriangleSoup a,b;
        std::string err;
        bool ok = geo::LoadObjTriangles(sourceA,a,err);
        if (ok) {
            if (mode == 0) {
                if (!terrainReady) { ok = false; err = "Select a valid terrain"; }
                else {
                    curves = geo::ExtractTerrainIntersection(
                        groupIt->second.data.heights,a,settings);
                    bakedGroup = terrainGroup;
                }
            } else {
                ok = geo::LoadObjTriangles(sourceB,b,err);
                if (ok) {
#ifdef TOURMALINE_EDITOR_WITH_CGAL
                    ok = geo::ExtractMeshIntersectionsCGAL(a,b,curves,err);
                    bakedGroup = UINT32_MAX;
#else
                    ok = false;
                    err = "CGAL backend not enabled. Configure with "
                          "-DTOURMALINE_EDITOR_CGAL_INTERSECTIONS=ON";
#endif
                }
            }
        }
        hasCurves = ok;
        geometry = {};
        status = ok
            ? "Extracted " + std::to_string(curves.curves.size())
              + " polylines, " + std::to_string(curves.ambiguous)
              + " ambiguous faces."
            : err;
        Log("Intersection tools: " + status);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        curves = {}; geometry = {}; hasCurves = false; bakedGroup = UINT32_MAX;
        status.clear();
    }
    if (hasCurves) {
        ImGui::Text("Curves: %zu | ambiguous: %u | skipped: %u",
            curves.curves.size(),curves.ambiguous,curves.skipped);
        bool canRibbon = product == 1;
        if (product == 0 && bakedGroup != UINT32_MAX) {
            const auto it = m_terrainGroups.find(bakedGroup);
            canRibbon = it != m_terrainGroups.end() && !it->second.deleted &&
                it->second.data.heights.valid();
        }
        if (!canRibbon) ImGui::BeginDisabled();
        if (ImGui::Button("Build bake geometry")) {
            if (product == 1)
                geometry = geo::BakeVerticalSkirt(curves,settings);
            else
                geometry = geo::BakeTerrainRibbon(curves,
                    m_terrainGroups.at(bakedGroup).data.heights,settings);
            status = "Generated " + std::to_string(geometry.vertices.size())
                + " vertices and " + std::to_string(geometry.indices.size()/3)
                + " triangles";
            Log("Intersection tools: " + status);
        }
        if (!canRibbon) ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Baked source assets");
    ImGui::InputText("Output OBJ path",output,sizeof(output));
    ImGui::TextWrapped("Write .obj files under your project's content "
        "folder and import them as regular geometry. Curves use OBJ line "
        "primitives; ribbons/skirts use indexed triangles with UVs and normals.");
    if (!hasCurves) ImGui::BeginDisabled();
    if (ImGui::Button("Save curves (.obj lines)")) {
        std::string err;
        if (geo::SaveCurvesObj(output,curves,err))
            status = std::string("Saved curve source: ") + output;
        else status = err;
        Log("Intersection tools: " + status);
    }
    if (!hasCurves) ImGui::EndDisabled();
    if (geometry.indices.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Save baked ribbon/skirt (.obj)")) {
        std::string err;
        if (geo::SaveRibbonObj(output,geometry,err))
            status = std::string("Saved baked geometry: ") + output;
        else status = err;
        Log("Intersection tools: " + status);
    }
    if (geometry.indices.empty()) ImGui::EndDisabled();
    if (!status.empty()) ImGui::TextWrapped("%s",status.c_str());

    // Lightweight 2D curve preview, XZ top-down. No GPU readback.
    if (hasCurves && !curves.curves.empty()) {
        ImGui::SeparatorText("XZ curve preview");
        glm::vec2 lo(1e30f),hi(-1e30f);
        for (const auto& c : curves.curves)
            for (const auto& p : c.points) {
                lo = glm::min(lo,glm::vec2(p.x,p.z));
                hi = glm::max(hi,glm::vec2(p.x,p.z));
            }
        ImVec2 box = ImGui::GetContentRegionAvail();
        box.y = std::clamp(box.y,100.0f,230.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("curvePreview",box);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin,ImVec2(origin.x+box.x,origin.y+box.y),
                            IM_COL32(32,37,42,255));
        const glm::vec2 span = glm::max(hi-lo,glm::vec2(0.001f));
        const float scale = std::min((box.x-16.0f)/span.x,(box.y-16.0f)/span.y);
        auto point = [&](const glm::vec3& p) -> ImVec2 {
            return ImVec2(origin.x+8+(p.x-lo.x)*scale,
                          origin.y+box.y-8-(p.z-lo.y)*scale);
        };
        for (const auto& c : curves.curves) {
            for (size_t i=1;i<c.points.size();++i)
                draw->AddLine(point(c.points[i-1]),point(c.points[i]),
                              IM_COL32(245,190,95,255),2.0f);
            if (c.closed && c.points.size()>2)
                draw->AddLine(point(c.points.back()),point(c.points.front()),
                              IM_COL32(245,190,95,255),2.0f);
        }
    }
    ImGui::End();
}
} // namespace editor
