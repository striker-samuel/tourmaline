// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "editor_state_machine.h"
#include <Engine/Asset/state_machine_writer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace editor {
using namespace tour::asset;
namespace {
namespace fs = std::filesystem;

template <size_t N> void ToBuffer(std::array<char, N>& dst, const std::string& text) {
    dst.fill('\0');
    std::strncpy(dst.data(), text.c_str(), N - 1);
}
bool ValidFilename(const fs::path& p) {
    return p.extension() == ".statemachine" && p.has_filename() && !p.parent_path().empty();
}
StateCompare DefaultCompare(StateParamKind kind) {
    return kind == StateParamKind::Trigger ? StateCompare::Fired
           : kind == StateParamKind::Bool  ? StateCompare::Equal
                                           : StateCompare::Greater;
}
constexpr const char* kParamKinds[] = {"bool", "int", "float", "trigger"};
constexpr const char* kCompares[] = {"==", "!=", "<", "<=", ">", ">="};
} // namespace

void StateMachineGraphEditor::PushUndo() {
    if (m_undo.size() == 80)
        m_undo.erase(m_undo.begin());
    m_undo.push_back({m_doc, m_canvas});
    m_redo.clear();
    m_dirty = true;
}
void StateMachineGraphEditor::Undo() {
    if (m_undo.empty())
        return;
    m_redo.push_back({m_doc, m_canvas});
    auto state = std::move(m_undo.back());
    m_undo.pop_back();
    m_doc = std::move(state.doc);
    m_canvas = std::move(state.canvas);
    m_canvas.pendingFrom = -1;
    m_canvas.selectedNode = -1;
    m_canvas.selectedLink = -1;
    m_dirty = true;
}
void StateMachineGraphEditor::Redo() {
    if (m_redo.empty())
        return;
    m_undo.push_back({m_doc, m_canvas});
    auto state = std::move(m_redo.back());
    m_redo.pop_back();
    m_doc = std::move(state.doc);
    m_canvas = std::move(state.canvas);
    m_canvas.pendingFrom = -1;
    m_canvas.selectedNode = -1;
    m_canvas.selectedLink = -1;
    m_dirty = true;
}
void StateMachineGraphEditor::ResetHistory() {
    m_undo.clear();
    m_redo.clear();
    m_canvas.selectedNode = -1;
    m_canvas.selectedLink = -1;
    m_canvas.pendingFrom = -1;
    m_renameTarget = -1;
}

bool StateMachineGraphEditor::Open(const std::string& file, bool discard) {
    show = true;
    if (m_dirty && !discard && m_hasDoc) {
        m_error = "Unsaved changes: save or discard before switching files.";
        return false;
    }
    std::error_code ec;
    const fs::path path(file);
    if (!ValidFilename(path) || !fs::is_regular_file(path, ec) || ec) {
        m_error = "Select an existing .statemachine file.";
        return false;
    }
    const auto length = fs::file_size(path, ec);
    if (ec || length > 1024 * 1024) {
        m_error = "State machine file is too large or unreadable (1 MiB limit).";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        m_error = "Cannot open file for reading.";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    StateMachineAsset parsed;
    std::string error;
    if (!ParseStateMachineText(text, parsed, error)) {
        m_error = error;
        return false;
    }
    graphui::CanvasState canvas;
    std::istringstream source(text);
    source.imbue(std::locale::classic());
    std::string line;
    while (std::getline(source, line)) {
        std::istringstream fields(line);
        fields.imbue(std::locale::classic());
        std::string marker, key, name;
        float x = 0, y = 0;
        if (!(fields >> marker >> key >> name >> x >> y) || marker != "#" || key != "editor-pos" ||
            !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 100000 || std::abs(y) > 100000)
            continue;
        uint32_t id = parsed.State(name);
        if (id != UINT32_MAX)
            ++id;
        else if (name == "*")
            id = uint32_t(parsed.states.size() + 1);
        else
            continue;
        canvas.positions[id] = {x, y};
    }
    m_doc = std::move(parsed);
    m_canvas = std::move(canvas);
    m_path = file;
    ToBuffer(m_pathField, file);
    m_hasDoc = true;
    m_dirty = false;
    m_error.clear();
    ResetHistory();
    return true;
}

void StateMachineGraphEditor::New(bool discard) {
    show = true;
    if (m_dirty && !discard && m_hasDoc) {
        m_error = "Unsaved changes: save or discard before creating a new graph.";
        return;
    }
    m_doc = StateMachineAsset{};
    m_doc.states.push_back("idle");
    m_doc.entry = 0;
    m_canvas = graphui::CanvasState{};
    m_path.clear();
    m_pathField.fill('\0');
    ToBuffer(m_newState, "new_state");
    m_hasDoc = true;
    m_dirty = true;
    m_error.clear();
    ResetHistory();
}

bool StateMachineGraphEditor::Save() {
    if (!m_hasDoc) {
        m_error = "No graph is open.";
        return false;
    }
    const fs::path target(m_pathField.data());
    if (!ValidFilename(target)) {
        m_error = "Enter a .statemachine path (including a parent directory).";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(target.parent_path(), ec) || ec) {
        m_error = "The destination directory does not exist.";
        return false;
    }
    std::string text = SerializeStateMachineText(m_doc);
    if (text.empty()) {
        m_error = "State machine has invalid references/entry.";
        return false;
    }
    // Layout metadata is comments so the cook/runtime grammar remains
    // byte-for-byte compatible; avoid a sidecar editor asset kind.
    std::ostringstream positions;
    positions.imbue(std::locale::classic());
    positions << std::setprecision(9);
    for (const auto& [id, pos] : m_canvas.positions) {
        std::string name;
        if (id >= 1 && id <= m_doc.states.size())
            name = m_doc.states[id - 1];
        else if (id == m_doc.states.size() + 1)
            name = "*";
        if (!name.empty() && std::isfinite(pos.x) && std::isfinite(pos.y))
            positions << "# editor-pos " << name << " " << pos.x << " " << pos.y << "\n";
    }
    text += positions.str();
    StateMachineAsset check;
    std::string reason;
    if (!ParseStateMachineText(text, check, reason)) {
        m_error = "Save refused: " + reason;
        return false;
    }
    // Write a sibling temporary first. Failure never truncates the
    // original asset. On Windows explicitly replace the destination.
    fs::path tmp = target;
    tmp += std::string(".tmp-") +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out.write(text.data(), std::streamsize(text.size())))) {
            fs::remove(tmp, ec);
            m_error = "Failed to write temporary graph file.";
            return false;
        }
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            m_error = "Failed to flush temporary graph file.";
            return false;
        }
    }
#ifdef _WIN32
    const bool renamed = ::MoveFileExW(tmp.c_str(),
                                       target.c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!renamed)
        ec = std::make_error_code(std::errc::io_error);
#else
    fs::rename(tmp, target, ec);
    const bool renamed = !ec;
#endif
    if (!renamed) {
        std::error_code cleanup;
        fs::remove(tmp, cleanup);
        m_error = "Atomic graph file replace failed: " + ec.message();
        return false;
    }
    m_path = target.string();
    m_dirty = false;
    m_error.clear();
    ResetHistory();
    return true;
}

void StateMachineGraphEditor::RemoveState(uint32_t index) {
    if (index >= m_doc.states.size() || m_doc.states.size() <= 1)
        return;
    PushUndo();
    m_doc.states.erase(m_doc.states.begin() + index);
    if (m_doc.entry == index)
        m_doc.entry = 0;
    else if (m_doc.entry > index)
        --m_doc.entry;
    auto& transitions = m_doc.transitions;
    transitions.erase(std::remove_if(transitions.begin(),
                                     transitions.end(),
                                     [index](const StateTransition& t) {
                                         return t.to == index || t.from == index;
                                     }),
                      transitions.end());
    for (auto& t : transitions) {
        if (t.to > index)
            --t.to;
        if (t.from != UINT32_MAX && t.from > index)
            --t.from;
    }
    // Node IDs are derived from the current state index; keep authored
    // canvas positions aligned with the states after index compaction.
    auto previous = std::move(m_canvas.positions);
    m_canvas.positions.clear();
    for (const auto& [id, pos] : previous) {
        if (id == index + 1)
            continue;
        if (id <= m_doc.states.size() + 1) {
            const uint32_t updated = id > index + 1 ? id - 1 : id;
            m_canvas.positions[updated] = pos;
        }
        // 'any' used to be N+1, now N after deleting a state.
        else if (id == m_doc.states.size() + 2)
            m_canvas.positions[uint32_t(m_doc.states.size() + 1)] = pos;
    }
    m_canvas.selectedNode = -1;
    m_canvas.selectedLink = -1;
    m_canvas.pendingFrom = -1;
}
void StateMachineGraphEditor::RemoveParameter(uint32_t index) {
    if (index >= m_doc.parameters.size())
        return;
    PushUndo();
    m_doc.parameters.erase(m_doc.parameters.begin() + index);
    auto& transitions = m_doc.transitions;
    transitions.erase(
        std::remove_if(transitions.begin(),
                       transitions.end(),
                       [index](const StateTransition& t) { return t.parameter == index; }),
        transitions.end());
    for (auto& t : transitions)
        if (t.parameter > index)
            --t.parameter;
    m_canvas.selectedLink = -1;
}

void StateMachineGraphEditor::DrawParams() {
    if (!ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    for (size_t i = 0; i < m_doc.parameters.size(); ++i) {
        auto& p = m_doc.parameters[i];
        ImGui::PushID(int(i));
        ImGui::Text("%s (%s)", p.name.c_str(), kParamKinds[int(p.kind)]);
        if (p.kind == StateParamKind::Bool) {
            bool current = p.initial != 0;
            if (ImGui::Checkbox("Default", &current)) {
                PushUndo();
                p.initial = current ? 1 : 0;
            }
        } else if (p.kind == StateParamKind::Integer) {
            int current = int(p.initial);
            if (ImGui::InputInt("Default", &current)) {
                PushUndo();
                p.initial = current;
            }
        } else if (p.kind == StateParamKind::Float) {
            double current = p.initial;
            if (ImGui::InputDouble("Default", &current)) {
                if (std::isfinite(current)) {
                    PushUndo();
                    p.initial = current;
                }
            }
        }
        if (ImGui::SmallButton("Remove parameter")) {
            RemoveParameter(uint32_t(i));
            ImGui::PopID();
            break;
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::InputText("New parameter", m_newParam.data(), m_newParam.size());
    ImGui::Combo("Type", &m_newParamKind, kParamKinds, 4);
    if (ImGui::Button("Add parameter")) {
        const std::string name(m_newParam.data());
        if (!state_detail::Name(name) || m_doc.Parameter(name) != UINT32_MAX) {
            m_error = "Parameter names must be unique identifiers.";
        } else {
            PushUndo();
            m_doc.parameters.push_back({name, StateParamKind(m_newParamKind), 0});
            m_error.clear();
        }
    }
}

void StateMachineGraphEditor::DrawSelection() {
    const int selected = m_canvas.selectedNode;
    if (selected > 0 && selected <= int(m_doc.states.size())) {
        const uint32_t index = uint32_t(selected - 1);
        if (m_renameTarget != selected) {
            ToBuffer(m_renameField, m_doc.states[index]);
            m_renameTarget = selected;
        }
        ImGui::SeparatorText("Selected state");
        ImGui::InputText("Name", m_renameField.data(), m_renameField.size());
        if (ImGui::Button("Rename")) {
            const std::string name(m_renameField.data());
            if (!state_detail::Name(name) ||
                (m_doc.State(name) != UINT32_MAX && m_doc.State(name) != index)) {
                m_error = "State names must be unique identifiers.";
            } else {
                PushUndo();
                m_doc.states[index] = name;
                m_error.clear();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Make entry")) {
            PushUndo();
            m_doc.entry = index;
        }
        if (index == m_doc.entry)
            ImGui::TextDisabled("Entry state");
        if (ImGui::Button("Delete state")) {
            if (m_doc.states.size() > 1)
                RemoveState(index);
            else
                m_error = "Cannot delete the last state.";
        }
    } else if (selected > 0) {
        ImGui::SeparatorText("Any state");
        ImGui::TextWrapped("The * node is the wildcard transition source.");
    }
    const int selectedLink = m_canvas.selectedLink;
    if (selectedLink > 0 && size_t(selectedLink) <= m_doc.transitions.size()) {
        const size_t idx = size_t(selectedLink - 1);
        ImGui::SeparatorText("Selected transition");
        auto& t = m_doc.transitions[idx];
        const char* from = t.from == UINT32_MAX ? "*" : m_doc.states[t.from].c_str();
        ImGui::Text("%s -> %s", from, m_doc.states[t.to].c_str());
        if (!m_doc.parameters.empty()) {
            if (ImGui::BeginCombo("Parameter", m_doc.parameters[t.parameter].name.c_str())) {
                for (uint32_t i = 0; i < m_doc.parameters.size(); ++i) {
                    const bool selectedParam = i == t.parameter;
                    if (ImGui::Selectable(m_doc.parameters[i].name.c_str(), selectedParam)) {
                        PushUndo();
                        t.parameter = i;
                        t.compare = DefaultCompare(m_doc.parameters[i].kind);
                        t.threshold = m_doc.parameters[i].kind == StateParamKind::Bool ? 1 : 0;
                    }
                }
                ImGui::EndCombo();
            }
            const StateParamKind kind = m_doc.parameters[t.parameter].kind;
            if (kind == StateParamKind::Trigger)
                ImGui::TextDisabled("Fires when trigger is set.");
            else {
                const bool isBool = kind == StateParamKind::Bool;
                int comp = int(t.compare);
                if (isBool) {
                    const char* choices[] = {"==", "!="};
                    if (ImGui::Combo("Compare", &comp, choices, 2)) {
                        PushUndo();
                        t.compare = StateCompare(comp);
                    }
                } else if (ImGui::Combo("Compare", &comp, kCompares, 6)) {
                    PushUndo();
                    t.compare = StateCompare(comp);
                }
                if (isBool) {
                    bool value = t.threshold != 0;
                    if (ImGui::Checkbox("Value", &value)) {
                        PushUndo();
                        t.threshold = value ? 1 : 0;
                    }
                } else if (kind == StateParamKind::Integer) {
                    int value = int(t.threshold);
                    if (ImGui::InputInt("Value", &value)) {
                        PushUndo();
                        t.threshold = value;
                    }
                } else {
                    double value = t.threshold;
                    if (ImGui::InputDouble("Value", &value) && std::isfinite(value)) {
                        PushUndo();
                        t.threshold = value;
                    }
                }
            }
        }
        if (ImGui::Button("Priority up") && idx > 0) {
            PushUndo();
            std::swap(m_doc.transitions[idx], m_doc.transitions[idx - 1]);
            m_canvas.selectedLink = int(idx);
        }
        ImGui::SameLine();
        if (ImGui::Button("Priority down") && idx + 1 < m_doc.transitions.size()) {
            PushUndo();
            std::swap(m_doc.transitions[idx], m_doc.transitions[idx + 1]);
            m_canvas.selectedLink = int(idx + 2);
        }
        if (ImGui::Button("Delete transition")) {
            PushUndo();
            m_doc.transitions.erase(m_doc.transitions.begin() + idx);
            m_canvas.selectedLink = -1;
        }
        ImGui::TextDisabled("Transition order is evaluation priority.");
    }
}

void StateMachineGraphEditor::Draw() {
    if (!show) {
        m_focused = false;
        return;
    }
    if (!ImGui::Begin("State Machine Graph", &show)) {
        m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        ImGui::End();
        return;
    }
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (ImGui::Button("New"))
        New();
    ImGui::SameLine();
    if (ImGui::Button("Reload") && !m_path.empty())
        Open(m_path);
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        Save();
    ImGui::SameLine();
    if (ImGui::Button("Undo"))
        Undo();
    ImGui::SameLine();
    if (ImGui::Button("Redo"))
        Redo();
    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(.95f, .70f, .3f, 1.0f), "Unsaved");
        ImGui::SameLine();
        if (ImGui::SmallButton("Discard & new"))
            New(true);
        ImGui::SameLine();
        if (ImGui::SmallButton("Discard & reload") && !m_path.empty())
            Open(m_path, true);
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##graph_path", m_pathField.data(), m_pathField.size());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Source path for Save / Save As (.statemachine)");
    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, .45f, .4f, 1.0f), "%s", m_error.c_str());
    }
    if (!m_hasDoc) {
        ImGui::TextWrapped("Select a .statemachine file in Assets and "
                           "click Edit graph, or press New to create one.");
        ImGui::End();
        return;
    }
    ImGui::Separator();
    tour::graph::Document topology = StateMachineTopology(m_doc);
    // Keep a wildcard source on screen even before the first wildcard
    // transition. The runtime's structural projection includes it only
    // when authored; the editor also needs to OFFER creating one.
    const uint32_t anyId = uint32_t(m_doc.states.size() + 1);
    if (!topology.nodes.contains(anyId)) {
        tour::graph::Node n;
        n.id = anyId;
        n.type = "any";
        n.ports.push_back({"out", "flow", tour::graph::PortDirection::Output});
        topology.nodes.emplace(anyId, std::move(n));
    }
    const float side = std::min(390.0f, std::max(240.0f, ImGui::GetContentRegionAvail().x * .38f));
    const auto event =
        graphui::DrawCanvas(topology, m_canvas, "##state_canvas", ImVec2(-side - 8.0f, -1.0f));
    if (event.moved)
        m_dirty = true;
    if (event.connect) {
        if (m_doc.parameters.empty()) {
            m_error = "Create a parameter before connecting two states.";
        } else if (event.to >= 1 && event.to <= m_doc.states.size() && event.from >= 1 &&
                   event.from <= m_doc.states.size() + 1) {
            PushUndo();
            StateTransition t;
            t.from = event.from == m_doc.states.size() + 1 ? UINT32_MAX : event.from - 1;
            t.to = event.to - 1;
            t.parameter = 0;
            t.compare = DefaultCompare(m_doc.parameters[0].kind);
            t.threshold = m_doc.parameters[0].kind == StateParamKind::Bool ? 1 : 0;
            m_doc.transitions.push_back(t);
            m_canvas.selectedLink = int(m_doc.transitions.size());
            m_canvas.selectedNode = -1;
            m_error.clear();
        }
    }
    ImGui::SameLine();
    ImGui::BeginChild("##state_sidebar", ImVec2(-1, -1), true);
    ImGui::TextDisabled("Drag nodes, MMB pan, wheel zoom.");
    ImGui::TextDisabled("Click out pin, then an in pin to connect.");
    ImGui::InputText("State name", m_newState.data(), m_newState.size());
    if (ImGui::Button("Add state")) {
        const std::string name(m_newState.data());
        if (!state_detail::Name(name) || m_doc.State(name) != UINT32_MAX ||
            m_doc.states.size() >= 65534) {
            m_error = "State names must be unique identifiers.";
        } else {
            PushUndo();
            m_doc.states.push_back(name);
            m_canvas.selectedNode = int(m_doc.states.size());
            m_error.clear();
        }
    }
    DrawSelection();
    ImGui::Separator();
    DrawParams();
    ImGui::EndChild();
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (m_focused && !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        if (m_canvas.selectedLink > 0 &&
            size_t(m_canvas.selectedLink) <= m_doc.transitions.size()) {
            PushUndo();
            m_doc.transitions.erase(m_doc.transitions.begin() + (m_canvas.selectedLink - 1));
            m_canvas.selectedLink = -1;
        } else if (m_canvas.selectedNode > 0 && m_canvas.selectedNode <= int(m_doc.states.size())) {
            RemoveState(uint32_t(m_canvas.selectedNode - 1));
        }
    }
    ImGui::End();
}

} // namespace editor
