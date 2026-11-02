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
enum class ColumnKind { Float, UInt, Bool };
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
constexpr Column kSource[] = {
	{"Volume", "volume", ColumnKind::Float, 0, 16},
	{"Spatial", "spatial", ColumnKind::Bool, 0, 1},
	{"Play on load", "playOnLoad", ColumnKind::Bool, 0, 1},
	{"Min distance", "minDistance", ColumnKind::Float, 0, 10000},
	{"Max distance", "maxDistance", ColumnKind::Float, 0.001f, 10000},
};

std::string Lower(std::string value) {
	for (char& c : value)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return value;
}
} // namespace

void EditorApp::DrawComponentTable() {
	if (!ImGui::Begin("Component Spreadsheet", &m_showComponentTable,
			ImGuiWindowFlags_NoCollapse)) {
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
	ImGui::Combo("Component", &kind, "GI Volume\0Audio Room\0Audio Source\0");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(220);
	ImGui::InputTextWithHint("##component_filter", "Filter node name or ID",
		m_componentTableFilter, sizeof(m_componentTableFilter));
	ImGui::SameLine();
	if (ImGui::SmallButton("Clear"))
		m_componentTableFilter[0] = '\0';
	ImGui::SameLine();
	ImGui::TextDisabled("Enter: commit cell  |  Ctrl+Z: undo");

	const Column* columns = kind == 0 ? kGi : kind == 1 ? kRoom : kSource;
	const size_t count = kind == 0 ? sizeof(kGi) / sizeof(kGi[0])
		: kind == 1 ? sizeof(kRoom) / sizeof(kRoom[0])
		: sizeof(kSource) / sizeof(kSource[0]);
	const EditVerb verb = kind == 0 ? EditVerb::SetGiVolumeAttr
		: kind == 1 ? EditVerb::SetAudioRoomAttr : EditVerb::SetAudioAttr;

	auto read = [&](uint32_t node, const char* attr, glm::vec4& vec,
			double& num, bool& flag, std::string& name) {
		switch (kind) {
		case 0: return ReadNodeGiVolumeAttr(node, attr, vec, num, name);
		case 1: return ReadNodeAudioRoomAttr(node, attr, vec, num, flag, name);
		default: return ReadNodeAudioSourceAttr(node, attr, vec, num, flag, name);
		}
	};
	auto hasComponent = [&](uint32_t node) {
		switch (kind) {
		case 0: return FindGiVolume(scene, node) != nullptr;
		case 1: return tour::scene::FindAudioRoom(scene, node) != nullptr;
		default: return FindAudioSource(scene, node) != nullptr;
		}
	};
	std::vector<uint32_t> nodes;
	const std::string filter = Lower(m_componentTableFilter);
	for (uint32_t node = 0; node < scene.hierarchy.size(); ++node) {
		if (m_deletedNodes.contains(node) || !hasComponent(node))
			continue;
		const std::string name = getNodeName(scene, int(node));
		if (!filter.empty() && Lower(name).find(filter) == std::string::npos
				&& std::to_string(node).find(filter) == std::string::npos)
			continue;
		nodes.push_back(node);
	}
	ImGui::TextDisabled("%zu instances  |  click a row to select its node", nodes.size());

	const ImGuiTableFlags flags =
		ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable
		| ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable
		| ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY
		| ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders
		| ImGuiTableFlags_SizingFixedFit;
	if (ImGui::BeginTable("##scene_component_spreadsheet",
			int(count) + 2, flags, ImVec2(0, 0))) {
		ImGui::TableSetupScrollFreeze(2, 1);
		ImGui::TableSetupColumn("Node ID", ImGuiTableColumnFlags_DefaultSort
			| ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 190.0f);
		for (size_t i = 0; i < count; ++i)
			ImGui::TableSetupColumn(columns[i].title,
				ImGuiTableColumnFlags_WidthFixed, 125.0f);
		ImGui::TableHeadersRow();

		if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
			if (specs->SpecsCount > 0) {
				const ImGuiTableColumnSortSpecs& sort = specs->Specs[0];
				const int by = sort.ColumnIndex;
				const bool descending = sort.SortDirection == ImGuiSortDirection_Descending;
				std::stable_sort(nodes.begin(), nodes.end(), [&](uint32_t a, uint32_t b) {
					bool less = false;
					bool greater = false;
					if (by == 0) { less = a < b; greater = a > b; }
					else if (by == 1) {
						const std::string aa = getNodeName(scene, int(a));
						const std::string bb = getNodeName(scene, int(b));
						less = aa < bb; greater = aa > bb;
					} else if (by >= 2 && size_t(by - 2) < count) {
						glm::vec4 v(0);
						double an = 0, bn = 0;
						bool af = false, bf = false;
						std::string unused;
						read(a, columns[by - 2].attr, v, an, af, unused);
						read(b, columns[by - 2].attr, v, bn, bf, unused);
						if (columns[by - 2].kind == ColumnKind::Bool) {
							an = af ? 1 : 0; bn = bf ? 1 : 0;
						}
						less = an < bn; greater = an > bn;
					}
					return descending ? greater : less;
				});
			}
		}

		ImGuiListClipper clipper;
		clipper.Begin(int(nodes.size()));
		while (clipper.Step()) for (int row = clipper.DisplayStart;
				row < clipper.DisplayEnd; ++row) {
			const uint32_t node = nodes[size_t(row)];
			ImGui::PushID(int(node));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			char idText[32];
			std::snprintf(idText, sizeof(idText), "%u", node);
			if (ImGui::Selectable(idText, m_selected == node,
					ImGuiSelectableFlags_SpanAllColumns)) {
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
				if (col.kind == ColumnKind::Bool)
					committed = ImGui::Checkbox("##cell", &flagValue);
				else if (col.kind == ColumnKind::UInt) {
					int v = int(oldNum);
					committed = ImGui::InputInt("##cell", &v, 0, 0,
						ImGuiInputTextFlags_EnterReturnsTrue);
					value = std::clamp(double(v), double(col.minimum), double(col.maximum));
				} else {
					float v = float(oldNum);
					committed = ImGui::InputFloat("##cell", &v, 0, 0, "%.3f",
						ImGuiInputTextFlags_EnterReturnsTrue);
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
					switch (kind) {
					case 0: cmd.giVolumeAttr = col.attr; break;
					case 1: cmd.roomAttr = col.attr; break;
					default: cmd.audioAttr = col.attr; break;
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
