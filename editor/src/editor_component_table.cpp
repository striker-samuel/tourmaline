// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Houdini-style component spreadsheet. Each row is a scene node with the
// selected component; columns are the component's fields. Cells use the
// same EditCmd history and attribute codecs as the Inspector.

#include "editor_app.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

namespace editor {
namespace {
enum class ColumnKind { Float, UInt, Bool, String };
struct Column {
    const char* title;
    const char* attr;
    ColumnKind kind;
    float minimum;
    float maximum;
};
constexpr Column kGi[] = {
    {"Priority", "priority", ColumnKind::UInt, 0, 1000000},
    {"Spacing", "spacing", ColumnKind::Float, 0.001f, 100},
    {"Spacing low", "spacinglow", ColumnKind::Float, 0, 100},
    {"Spacing high", "spacinghigh", ColumnKind::Float, 0, 100},
    {"Inside fade", "inside", ColumnKind::Float, 0, 100},
    {"Outside fade", "outside", ColumnKind::Float, 0, 100},
};
constexpr Column kRoom[] = {
    {"Enabled", "enabled", ColumnKind::Bool, 0, 1},
    {"Priority", "priority", ColumnKind::UInt, 0, 1000000},
    {"Inside fade", "insideFade", ColumnKind::Float, 0, 100},
    {"Outside fade", "outsideFade", ColumnKind::Float, 0, 100},
    {"Wet", "wet", ColumnKind::Float, 0, 1},
    {"RT60", "rt60", ColumnKind::Float, 0.15f, 8},
    {"Damping", "damping", ColumnKind::Float, 0, 1},
    {"Transition", "transitionSeconds", ColumnKind::Float, 0.001f, 10},
};
constexpr Column kTrigger[] = {
    {"Enabled", "enabled", ColumnKind::Bool, 0, 1},
    {"Once", "once", ColumnKind::Bool, 0, 1},
    {"Event tag", "tag", ColumnKind::String, 0, 0},
};
constexpr Column kSource[] = {
    {"Volume", "volume", ColumnKind::Float, 0, 16},
    {"Spatial", "spatial", ColumnKind::Bool, 0, 1},
    {"Play on load", "playOnLoad", ColumnKind::Bool, 0, 1},
    {"Min distance", "minDistance", ColumnKind::Float, 0, 10000},
    {"Max distance", "maxDistance", ColumnKind::Float, 0.001f, 10000},
};

constexpr Column kLight[] = {
    {"Intensity", "intensity", ColumnKind::Float, 0, 100000},
    {"Range", "range", ColumnKind::Float, 0, 100000},
    {"Inner cone", "inner", ColumnKind::Float, 0, 89},
    {"Outer cone", "outer", ColumnKind::Float, 0, 89},
    {"Static", "static", ColumnKind::Bool, 0, 1},
    {"Cast shadow", "shadow", ColumnKind::Bool, 0, 1},
    {"Function", "function", ColumnKind::UInt, 0, 3},
    {"Speed", "functionSpeed", ColumnKind::Float, 0.01f, 100},
    {"Depth", "functionScale", ColumnKind::Float, 0, 1},
};
constexpr Column kCamera[] = {
    {"Projection", "projection", ColumnKind::UInt, 0, 1},
    {"FOV Y", "fovY", ColumnKind::Float, 1, 179},
    {"Ortho height", "orthoHalfH", ColumnKind::Float, 0.01f, 10000},
    {"Near clip", "near", ColumnKind::Float, 0.001f, 10000},
    {"Far clip", "far", ColumnKind::Float, 0.01f, 100000},
};
constexpr Column kCubemap[] = {
    {"Radius", "radius", ColumnKind::Float, 0.01f, 10000},
};
constexpr Column kTransform[] = {
    {"Name", "name", ColumnKind::String, 0, 0},
    {"TX", "tx", ColumnKind::Float, -100000, 100000},
    {"TY", "ty", ColumnKind::Float, -100000, 100000},
    {"TZ", "tz", ColumnKind::Float, -100000, 100000},
    {"RX", "rx", ColumnKind::Float, -36000, 36000},
    {"RY", "ry", ColumnKind::Float, -36000, 36000},
    {"RZ", "rz", ColumnKind::Float, -36000, 36000},
    {"SX", "sx", ColumnKind::Float, 0.001f, 10000},
    {"SY", "sy", ColumnKind::Float, 0.001f, 10000},
    {"SZ", "sz", ColumnKind::Float, 0.001f, 10000},
};

std::string Lower(std::string value) {
    for (char& c : value)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
} // namespace

void EditorApp::DrawComponentTable() {
    if (!ImGui::Begin(
            "Component Spreadsheet", &m_showComponentTable, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (!m_world) {
        ImGui::TextDisabled("Open a scene to view component data.");
        ImGui::End();
        return;
    }
    Scene& scene = m_world->scene;
    // A scene-relative component type determines the column schema,
    // just like choosing point/primitive/detail attributes in Houdini.
    static int kind = 0;
    ImGui::SetNextItemWidth(170);
    ImGui::Combo(
        "Component",
        &kind,
        "GI Volume\0Audio Room\0Audio Source\0Trigger Volume\0Light\0Camera\0Cubemap\0Transform\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##component_filter",
                             "Filter node name or ID",
                             m_componentTableFilter,
                             sizeof(m_componentTableFilter));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        m_componentTableFilter[0] = '\0';
    ImGui::SameLine();
    ImGui::TextDisabled("Enter: commit cell  |  Ctrl+Z: undo");

    const Column* columns = kind == 0   ? kGi
                            : kind == 1 ? kRoom
                            : kind == 2 ? kSource
                            : kind == 3 ? kTrigger
                            : kind == 4 ? kLight
                            : kind == 5 ? kCamera
                            : kind == 6 ? kCubemap
                                        : kTransform;
    const size_t count = kind == 0   ? sizeof(kGi) / sizeof(kGi[0])
                         : kind == 1 ? sizeof(kRoom) / sizeof(kRoom[0])
                         : kind == 2 ? sizeof(kSource) / sizeof(kSource[0])
                         : kind == 3 ? sizeof(kTrigger) / sizeof(kTrigger[0])
                         : kind == 4 ? sizeof(kLight) / sizeof(kLight[0])
                         : kind == 5 ? sizeof(kCamera) / sizeof(kCamera[0])
                         : kind == 6 ? sizeof(kCubemap) / sizeof(kCubemap[0])
                                     : sizeof(kTransform) / sizeof(kTransform[0]);
    const EditVerb verb = kind == 0   ? EditVerb::SetGiVolumeAttr
                          : kind == 1 ? EditVerb::SetAudioRoomAttr
                          : kind == 2 ? EditVerb::SetAudioAttr
                          : kind == 3 ? EditVerb::SetTriggerVolumeAttr
                          : kind == 4 ? EditVerb::SetLightAttr
                          : kind == 5 ? EditVerb::SetCamAttr
                          : kind == 6 ? EditVerb::SetCubemapAttr
                                      : EditVerb::Translate;

    auto read = [&](uint32_t node,
                    const char* attr,
                    glm::vec4& vec,
                    double& num,
                    bool& flag,
                    std::string& name) {
        switch (kind) {
        case 0:
            return ReadNodeGiVolumeAttr(node, attr, vec, num, name);
        case 1:
            return ReadNodeAudioRoomAttr(node, attr, vec, num, flag, name);
        case 2:
            return ReadNodeAudioSourceAttr(node, attr, vec, num, flag, name);
        case 3:
            return ReadNodeTriggerVolumeAttr(node, attr, num, flag, name);
        case 4:
            return ReadNodeLightAttr(node, attr, vec, num, flag, name);
        case 5:
            return ReadNodeCameraAttr(node, attr, vec, num, name);
        case 6:
            return ReadNodeCubemapAttr(node, attr, vec, num, name);
        default: {
            if (std::string(attr) == "name") {
                name = getNodeName(scene, int(node));
                return true;
            }
            glm::vec3 t, rot, scale;
            ReadLocalTRS(node, t, rot, scale);
            const char group = attr[0];
            const int axis = attr[1] == 'x' ? 0 : attr[1] == 'y' ? 1 : 2;
            if (group == 't')
                num = t[axis];
            else if (group == 'r')
                num = rot[axis];
            else if (group == 's')
                num = scale[axis];
            else
                return false;
            return true;
        }
        }
    };
    auto hasComponent = [&](uint32_t node) {
        switch (kind) {
        case 0:
            return FindGiVolume(scene, node) != nullptr;
        case 1:
            return tour::scene::FindAudioRoom(scene, node) != nullptr;
        case 2:
            return FindAudioSource(scene, node) != nullptr;
        case 3:
            return tour::scene::FindTriggerVolume(scene, node) != nullptr;
        case 4:
            return FindLight(scene, node) != nullptr;
        case 5:
            return FindCamera(scene, node) != nullptr;
        case 6:
            return FindCubemap(scene, node) != nullptr;
        default:
            return IsNodeAlive(scene, node);
        }
    };
    std::vector<uint32_t> nodes;
    const std::string filter = Lower(m_componentTableFilter);
    for (uint32_t node = 0; node < scene.hierarchy.size(); ++node) {
        if (m_deletedNodes.contains(node) || !hasComponent(node))
            continue;
        const std::string name = getNodeName(scene, int(node));
        if (!filter.empty() && Lower(name).find(filter) == std::string::npos &&
            std::to_string(node).find(filter) == std::string::npos)
            continue;
        nodes.push_back(node);
    }
    ImGui::TextDisabled("%zu instances  |  click a row to select its node", nodes.size());

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##scene_component_spreadsheet", int(count) + 2, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(2, 1);
        ImGui::TableSetupColumn(
            "Node ID", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        for (size_t i = 0; i < count; ++i)
            ImGui::TableSetupColumn(columns[i].title, ImGuiTableColumnFlags_WidthFixed, 125.0f);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& sort = specs->Specs[0];
                const int by = sort.ColumnIndex;
                const bool descending = sort.SortDirection == ImGuiSortDirection_Descending;
                std::stable_sort(nodes.begin(), nodes.end(), [&](uint32_t a, uint32_t b) {
                    bool less = false;
                    bool greater = false;
                    if (by == 0) {
                        less = a < b;
                        greater = a > b;
                    } else if (by == 1) {
                        const std::string aa = getNodeName(scene, int(a));
                        const std::string bb = getNodeName(scene, int(b));
                        less = aa < bb;
                        greater = aa > bb;
                    } else if (by >= 2 && size_t(by - 2) < count) {
                        glm::vec4 v(0);
                        double an = 0, bn = 0;
                        bool af = false, bf = false;
                        std::string unused;
                        read(a, columns[by - 2].attr, v, an, af, unused);
                        read(b, columns[by - 2].attr, v, bn, bf, unused);
                        if (columns[by - 2].kind == ColumnKind::Bool) {
                            an = af ? 1 : 0;
                            bn = bf ? 1 : 0;
                        }
                        if (columns[by - 2].kind == ColumnKind::String) {
                            std::string as, bs;
                            read(a, columns[by - 2].attr, v, an, af, as);
                            read(b, columns[by - 2].attr, v, bn, bf, bs);
                            less = as < bs;
                            greater = as > bs;
                        } else {
                            less = an < bn;
                            greater = an > bn;
                        }
                    }
                    return descending ? greater : less;
                });
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin(int(nodes.size()));
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const uint32_t node = nodes[size_t(row)];
                ImGui::PushID(int(node));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char idText[32];
                std::snprintf(idText, sizeof(idText), "%u", node);
                if (ImGui::Selectable(idText, m_selected == node)) {
                    m_selected = node;
                    m_worldSelected = false;
                    m_selection.clear();
                    m_selection.push_back(node);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(getNodeName(scene, int(node)).c_str());
                for (size_t index = 0; index < count; ++index) {
                    const Column& col = columns[index];
                    glm::vec4 oldVec(0);
                    double oldNum = 0;
                    bool oldFlag = false;
                    std::string oldText;
                    if (!read(node, col.attr, oldVec, oldNum, oldFlag, oldText))
                        continue;
                    ImGui::TableSetColumnIndex(int(index) + 2);
                    ImGui::PushID(int(index));
                    ImGui::SetNextItemWidth(-1.0f);
                    bool committed = false;
                    double value = oldNum;
                    bool flagValue = oldFlag;
                    char stringValue[257];
                    std::snprintf(stringValue, sizeof(stringValue), "%s", oldText.c_str());
                    if (col.kind == ColumnKind::String) {
                        committed = ImGui::InputText("##cell",
                                                     stringValue,
                                                     sizeof(stringValue),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
                    } else if (col.kind == ColumnKind::Bool)
                        committed = ImGui::Checkbox("##cell", &flagValue);
                    else if (col.kind == ColumnKind::UInt) {
                        int v = int(oldNum);
                        committed = ImGui::InputInt(
                            "##cell", &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
                        value = std::clamp(double(v), double(col.minimum), double(col.maximum));
                    } else {
                        float v = float(oldNum);
                        committed = ImGui::InputFloat(
                            "##cell", &v, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
                        value = std::clamp(double(v), double(col.minimum), double(col.maximum));
                    }
                    if (committed) {
                        EditCmd cmd{};
                        cmd.verb = verb;
                        cmd.node = node;
                        cmd.beforeVec4 = oldVec;
                        cmd.beforeNum = oldNum;
                        cmd.beforeFlag = oldFlag;
                        cmd.beforeText = oldText;
                        cmd.number = value;
                        cmd.afterFlag = flagValue;
                        if (col.kind == ColumnKind::String)
                            cmd.afterText = stringValue;
                        switch (kind) {
                        case 0:
                            cmd.giVolumeAttr = col.attr;
                            break;
                        case 1:
                            cmd.roomAttr = col.attr;
                            break;
                        case 2:
                            cmd.audioAttr = col.attr;
                            break;
                        case 3:
                            cmd.triggerVolumeAttr = col.attr;
                            break;
                        case 4:
                            cmd.lightAttr = col.attr;
                            break;
                        case 5:
                            cmd.camAttr = col.attr;
                            break;
                        case 6:
                            cmd.cubemapAttr = col.attr;
                            break;
                        default: {
                            cmd.beforeMat = scene.localTransform[node];
                            if (col.kind == ColumnKind::String) {
                                cmd.verb = EditVerb::Rename;
                            } else {
                                glm::vec3 t, rot, scale;
                                ReadLocalTRS(node, t, rot, scale);
                                glm::vec3* v = col.attr[0] == 't'   ? &t
                                               : col.attr[0] == 'r' ? &rot
                                                                    : &scale;
                                const int axis = col.attr[1] == 'x'   ? 0
                                                 : col.attr[1] == 'y' ? 1
                                                                      : 2;
                                (*v)[axis] = float(value);
                                cmd.verb = col.attr[0] == 't'   ? EditVerb::Translate
                                           : col.attr[0] == 'r' ? EditVerb::Rotate
                                                                : EditVerb::Scale;
                                cmd.afterVec = *v;
                            }
                            break;
                        }
                        }
                        Do(cmd);
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    ImGui::End();
}
} // namespace editor
