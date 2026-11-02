// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Scene component table: a second VIEW on the scene association tables.
// Every edit goes through the existing editor commands, so inspector edits,
// table edits, undo/redo, and .tour persistence share exactly one data model.

#include "editor_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

namespace editor {
namespace {
enum class CellKind { Number, Integer, Boolean, Seed };
struct Property {
	const char* name;
	const char* attr;
	CellKind kind;
	float low;
	float high;
};
constexpr Property kGi[] = {
	{ "Priority", "priority", CellKind::Integer, 0, 1000000 },
	{ "Probe spacing (m)", "spacing", CellKind::Number, 0.001f, 8 },
	{ "Density gate low", "spacinglow", CellKind::Number, 0, 8 },
	{ "Density gate high", "spacinghigh", CellKind::Number, 0, 8 },
	{ "Inside fade (m)", "inside", CellKind::Number, 0, 100 },
	{ "Outside fade (m)", "outside", CellKind::Number, 0, 100 },
	{ "Seed: spacing + phase XYZ", "optlayout", CellKind::Seed, 0, 8 },
};
constexpr Property kRoom[] = {
	{ "Enabled", "enabled", CellKind::Boolean, 0, 1 },
	{ "Priority", "priority", CellKind::Integer, 0, 1000000 },
	{ "Inside fade (m)", "insideFade", CellKind::Number, 0, 100 },
	{ "Outside fade (m)", "outsideFade", CellKind::Number, 0, 100 },
	{ "Wet", "wet", CellKind::Number, 0, 1 },
	{ "RT60 (s)", "rt60", CellKind::Number, 0.15f, 8 },
	{ "Damping", "damping", CellKind::Number, 0, 1 },
	{ "Transition (s)", "transitionSeconds", CellKind::Number, 0.001f, 10 },
};
constexpr Property kSource[] = {
	{ "Volume", "volume", CellKind::Number, 0, 16 },
	{ "Spatial", "spatial", CellKind::Boolean, 0, 1 },
	{ "Play on load", "playOnLoad", CellKind::Boolean, 0, 1 },
	{ "Minimum distance (m)", "minDistance", CellKind::Number, 0, 10000 },
	{ "Maximum distance (m)", "maxDistance", CellKind::Number, 0.001f, 10000 },
};
} // namespace

void EditorApp::DrawComponentTable() {
	if (!ImGui::Begin("Component table", &m_showComponentTable,
			ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	if (!m_world) {
		ImGui::TextDisabled("Open a scene to inspect its components.");
		ImGui::End();
		return;
	}
	Scene& scene = m_world->scene;
	ImGui::TextDisabled("Live scene associations; Enter commits a cell, Ctrl+Z undoes it.");
	ImGui::SetNextItemWidth(250.0f);
	ImGui::InputTextWithHint("##component_filter", "Filter node or component...",
		m_componentTableFilter, sizeof(m_componentTableFilter));
	ImGui::SameLine();
	if (ImGui::Button("Clear filter"))
		m_componentTableFilter[0] = '\0';

	const ImGuiTableFlags flags = ImGuiTableFlags_Borders |
		ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
		ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##component_rows", 4, flags, ImVec2(0.0f, 0.0f))) {
		ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		auto matches = [&](uint32_t node, const char* component) {
			if (node >= scene.hierarchy.size()
					|| m_deletedNodes.contains(node))
				return false;
			const std::string filter(m_componentTableFilter);
			if (filter.empty())
				return true;
			return getNodeName(scene, int(node)).find(filter) != std::string::npos
				|| std::string(component).find(filter) != std::string::npos;
		};
		auto drawGroup = [&](uint32_t node, const char* component,
				const Property* properties, size_t count, EditVerb verb) {
			if (!matches(node, component))
				return;
			const std::string name = getNodeName(scene, int(node));
			ImGui::PushID(int(node));
			ImGui::PushID(component);
			for (size_t index = 0; index < count; ++index) {
				const Property& p = properties[index];
				glm::vec4 oldVec(0.0f);
				double oldNum = 0.0;
				bool oldFlag = false;
				std::string oldText;
				bool found = false;
				switch (verb) {
				case EditVerb::SetGiVolumeAttr:
					found = ReadNodeGiVolumeAttr(node, p.attr, oldVec, oldNum, oldText);
					break;
				case EditVerb::SetAudioRoomAttr:
					found = ReadNodeAudioRoomAttr(node, p.attr, oldVec, oldNum,
						oldFlag, oldText);
					break;
				case EditVerb::SetAudioAttr:
					found = ReadNodeAudioSourceAttr(node, p.attr, oldVec, oldNum,
						oldFlag, oldText);
					break;
				default: break;
				}
				if (!found)
					continue;
				ImGui::PushID(int(index));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				if (ImGui::Selectable(name.c_str(), m_selected == node)) {
					m_selected = node;
					m_worldSelected = false;
					m_selection.clear();
					m_selection.push_back(node);
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(component);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(p.name);
				ImGui::TableSetColumnIndex(3);
				ImGui::SetNextItemWidth(-1.0f);
				double newNum = oldNum;
				bool newFlag = oldFlag;
				glm::vec4 newVec = oldVec;
				bool committed = false;
				if (p.kind == CellKind::Boolean) {
					committed = ImGui::Checkbox("##value", &newFlag);
				} else if (p.kind == CellKind::Integer) {
					int value = int(oldNum);
					committed = ImGui::InputInt("##value", &value, 0, 0,
						ImGuiInputTextFlags_EnterReturnsTrue);
					newNum = std::clamp(double(value), double(p.low), double(p.high));
				} else if (p.kind == CellKind::Seed) {
					float values[4] = { float(oldNum), oldVec.x, oldVec.y, oldVec.z };
					committed = ImGui::InputFloat4("##value", values, "%.3f",
						ImGuiInputTextFlags_EnterReturnsTrue);
					newNum = std::clamp(double(values[0]), double(p.low), double(p.high));
					newVec = glm::vec4(values[1], values[2], values[3], 0.0f);
				} else {
					float value = float(oldNum);
					committed = ImGui::InputFloat("##value", &value, 0, 0, "%.3f",
						ImGuiInputTextFlags_EnterReturnsTrue);
					newNum = std::clamp(double(value), double(p.low), double(p.high));
				}
				if (committed) {
					EditCmd c{};
					c.verb = verb;
					c.node = node;
					c.beforeVec4 = oldVec;
					c.beforeNum = oldNum;
					c.beforeFlag = oldFlag;
					c.beforeText = oldText;
					c.vec4 = newVec;
					c.number = newNum;
					c.afterFlag = newFlag;
					switch (verb) {
					case EditVerb::SetGiVolumeAttr: c.giVolumeAttr = p.attr; break;
					case EditVerb::SetAudioRoomAttr: c.roomAttr = p.attr; break;
					case EditVerb::SetAudioAttr: c.audioAttr = p.attr; break;
					default: break;
					}
					Do(c);
				}
				ImGui::PopID();
			}
			ImGui::PopID();
			ImGui::PopID();
		};

		// Iterate node order, not table storage order: both association
		// vectors can be reordered by cooking and world-merge.
		for (uint32_t node = 0; node < scene.hierarchy.size(); ++node) {
			if (FindGiVolume(scene, node))
				drawGroup(node, "GI Volume", kGi, sizeof(kGi)/sizeof(kGi[0]),
					EditVerb::SetGiVolumeAttr);
			if (tour::scene::FindAudioRoom(scene, node))
				drawGroup(node, "Audio Room", kRoom, sizeof(kRoom)/sizeof(kRoom[0]),
					EditVerb::SetAudioRoomAttr);
			if (FindAudioSource(scene, node))
				drawGroup(node, "Audio Source", kSource,
					sizeof(kSource)/sizeof(kSource[0]), EditVerb::SetAudioAttr);
		}
		ImGui::EndTable();
	}
	ImGui::End();
}
} // namespace editor
