// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Purely presentational ImGui node canvas for tour::graph::Document.
// Unlike a graph compiler, this widget knows nothing of placement,
// ecotopes, animation, transitions, or any domain-specific semantics.
#include <Engine/Graph/graph.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <map>
#include <string>

namespace editor::graphui {

struct CanvasPosition {
    float x = 0, y = 0;
};
struct CanvasState {
    std::map<uint32_t, CanvasPosition> positions;
    float panX = 32, panY = 32, zoom = 1.0f;
    int selectedNode = -1, selectedLink = -1;
    int pendingFrom = -1;
    std::string pendingPort;
};
struct CanvasEvent {
    bool moved = false;
    bool connect = false;
    uint32_t from = UINT32_MAX, to = UINT32_MAX;
    std::string output, input;
};

inline CanvasEvent DrawCanvas(const tour::graph::Document& doc,
                              CanvasState& state,
                              const char* id,
                              ImVec2 size) {
    CanvasEvent event;
    ImGui::BeginChild(
        id, size, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsWindowHovered();
    draw->PushClipRect(origin, ImVec2(origin.x + area.x, origin.y + area.y), true);
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        state.panX += ImGui::GetIO().MouseDelta.x;
        state.panY += ImGui::GetIO().MouseDelta.y;
    }
    if (hovered && ImGui::GetIO().MouseWheel != 0) {
        const float old = state.zoom;
        state.zoom =
            std::clamp(old * (ImGui::GetIO().MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.4f, 2.2f);
        const float ratio = state.zoom / old;
        state.panX = mouse.x - origin.x - (mouse.x - origin.x - state.panX) * ratio;
        state.panY = mouse.y - origin.y - (mouse.y - origin.y - state.panY) * ratio;
    }
    const auto screen = [&](CanvasPosition p) {
        return ImVec2(origin.x + state.panX + p.x * state.zoom,
                      origin.y + state.panY + p.y * state.zoom);
    };
    constexpr float nodeW = 178.0f;
    const float scale = state.zoom;
    const float gridStep = 32.0f * scale;
    for (float x = std::fmod(state.panX, gridStep); x < area.x; x += gridStep)
        for (float y = std::fmod(state.panY, gridStep); y < area.y; y += gridStep)
            draw->AddCircleFilled(
                ImVec2(origin.x + x, origin.y + y), 1.0f, IM_COL32(170, 180, 195, 34));
    // A caller may replace a document (open/reload) without reinitializing
    // layout: stable graph node IDs keep their positions.
    uint32_t ordinal = 0;
    for (const auto& [idNode, n] : doc.nodes) {
        (void)n;
        if (!state.positions.contains(idNode))
            state.positions[idNode] = {float(ordinal % 4) * 255.0f, float(ordinal / 4) * 152.0f};
        ++ordinal;
    }
    const auto pin = [&](uint32_t nodeId,
                         const std::string& name,
                         tour::graph::PortDirection direction) {
        auto it = doc.nodes.find(nodeId);
        if (it == doc.nodes.end())
            return screen({0, 0});
        const tour::graph::Node& n = it->second;
        unsigned row = 0;
        for (const auto& p : n.ports)
            if (p.direction == direction) {
                if (p.name == name)
                    break;
                ++row;
            }
        ImVec2 pos = screen(state.positions[nodeId]);
        return ImVec2(pos.x + (direction == tour::graph::PortDirection::Output ? nodeW * scale : 0),
                      pos.y + (43.0f + 20.0f * row) * scale);
    };
    // Wires behind nodes. Curve-distance picking is approximate but keeps
    // the UI independent of ImGui internals.
    for (const auto& [linkId, link] : doc.links) {
        const ImVec2 a = pin(link.from, link.output, tour::graph::PortDirection::Output);
        const ImVec2 b = pin(link.to, link.input, tour::graph::PortDirection::Input);
        const float dx = std::max(38.0f, std::abs(b.x - a.x) * .46f);
        const ImVec2 c1(a.x + dx, a.y), c2(b.x - dx, b.y);
        const bool selected = state.selectedLink == int(linkId);
        draw->AddBezierCubic(a,
                             c1,
                             c2,
                             b,
                             selected ? IM_COL32(255, 205, 92, 255) : IM_COL32(120, 193, 240, 255),
                             selected ? 3.3f : 2.0f);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            float distance2 = 9.0f * 9.0f;
            for (int step = 0; step <= 24; ++step) {
                const float t = float(step) / 24, u = 1.0f - t;
                const float x =
                    u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x;
                const float y =
                    u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y;
                const float d = (mouse.x - x) * (mouse.x - x) + (mouse.y - y) * (mouse.y - y);
                if (d < distance2) {
                    distance2 = d;
                    state.selectedLink = int(linkId);
                    state.selectedNode = -1;
                }
            }
        }
    }
    if (state.pendingFrom >= 0) {
        const auto a =
            pin(uint32_t(state.pendingFrom), state.pendingPort, tour::graph::PortDirection::Output);
        draw->AddLine(a, mouse, IM_COL32(245, 200, 95, 255), 2.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            state.pendingFrom = -1;
    }
    for (const auto& [nodeId, n] : doc.nodes) {
        ImVec2 pos = screen(state.positions[nodeId]);
        uint32_t count = 0, inCount = 0, outCount = 0;
        for (const auto& p : n.ports) {
            if (p.direction == tour::graph::PortDirection::Input)
                ++inCount;
            else
                ++outCount;
        }
        count = std::max(inCount, outCount);
        const float h = (57.0f + 20.0f * count) * scale, w = nodeW * scale;
        const ImVec2 bottom(pos.x + w, pos.y + h);
        const bool selected = state.selectedNode == int(nodeId);
        draw->AddRectFilled(pos, bottom, IM_COL32(28, 33, 41, 250), 6.0f);
        draw->AddRectFilled(pos,
                            ImVec2(bottom.x, pos.y + 29.0f * scale),
                            selected ? IM_COL32(90, 104, 156, 255) : IM_COL32(58, 69, 91, 255),
                            6.0f);
        draw->AddRect(pos,
                      bottom,
                      selected ? IM_COL32(245, 212, 113, 255) : IM_COL32(91, 100, 116, 255),
                      6.0f,
                      0,
                      selected ? 2.0f : 1.0f);
        draw->AddText(nullptr,
                      14.0f * scale,
                      ImVec2(pos.x + 9.0f * scale, pos.y + 6.0f * scale),
                      IM_COL32(242, 243, 247, 255),
                      n.type.c_str());
        ImGui::PushID(int(nodeId));
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##nodebody", ImVec2(w, h));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            state.selectedNode = int(nodeId);
            state.selectedLink = -1;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f) &&
            state.pendingFrom < 0) {
            auto& place = state.positions[nodeId];
            place.x += ImGui::GetIO().MouseDelta.x / scale;
            place.y += ImGui::GetIO().MouseDelta.y / scale;
            event.moved = true;
        }
        for (const auto& port : n.ports) {
            const ImVec2 p = pin(nodeId, port.name, port.direction);
            draw->AddCircleFilled(p, 5.4f * scale, IM_COL32(117, 208, 227, 255));
            const bool output = port.direction == tour::graph::PortDirection::Output;
            if (!output)
                draw->AddText(ImVec2(p.x + 10.0f * scale, p.y - 6.0f * scale),
                              IM_COL32(203, 211, 222, 255),
                              port.name.c_str());
            else {
                const float textW = ImGui::CalcTextSize(port.name.c_str()).x * scale;
                draw->AddText(ImVec2(p.x - 10.0f * scale - textW, p.y - 6.0f * scale),
                              IM_COL32(203, 211, 222, 255),
                              port.name.c_str());
            }
            ImGui::PushID(port.name.c_str());
            ImGui::SetCursorScreenPos(ImVec2(p.x - 9.0f * scale, p.y - 9.0f * scale));
            ImGui::InvisibleButton(output ? "##output" : "##input",
                                   ImVec2(18.0f * scale, 18.0f * scale));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                if (output) {
                    state.pendingFrom = int(nodeId);
                    state.pendingPort = port.name;
                } else if (state.pendingFrom >= 0 &&
                           doc.FindPort(uint32_t(state.pendingFrom),
                                        state.pendingPort,
                                        tour::graph::PortDirection::Output) &&
                           doc.FindPort(uint32_t(state.pendingFrom),
                                        state.pendingPort,
                                        tour::graph::PortDirection::Output)
                                   ->type == port.type) {
                    event.connect = true;
                    event.from = uint32_t(state.pendingFrom);
                    event.to = nodeId;
                    event.output = state.pendingPort;
                    event.input = port.name;
                    state.pendingFrom = -1;
                }
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    draw->PopClipRect();
    ImGui::EndChild();
    return event;
}

} // namespace editor::graphui
