// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// PlacementGraph editor UI: node canvas, inspector panels, viewport overlay.
// Model/compiler/persistence live in editor_placement.cpp; this file is
// presentation only (ImGui). All functions are EditorApp methods.

#include "editor_app.h"
#include "editor_placement.h"
#include "editor_attr_ui.h" // the attribute-widget stubs (attr::Float/...)

#include <cmath>
#include <cstdio>

#include <imgui.h>

namespace editor {
namespace {

using namespace editor::placement;
using namespace smvk::placement;

constexpr float kNodeW = 190.0f;
constexpr float kHeaderH = 26.0f;
constexpr float kRowH = 20.0f;

ImU32 PinColor(PinType t) {
	switch (t) {
	case PinType::Domain: return IM_COL32(220, 220, 230, 255);
	case PinType::Field: return IM_COL32(120, 220, 130, 255);
	case PinType::Protos: return IM_COL32(240, 170, 80, 255);
	case PinType::Xform: return IM_COL32(110, 200, 240, 255);
	case PinType::Spacing: return IM_COL32(200, 140, 240, 255);
	default: return IM_COL32(255, 255, 255, 255);
	}
}
ImU32 KindHeaderColor(NodeKind k) {
	const char* cat = NodeKindCategory(k);
	if (cat[0] == 'D') return IM_COL32(52, 86, 140, 255);   // Domain
	if (cat[0] == 'F' && cat[1] == 'i') return IM_COL32(46, 110, 58, 255); // Field
	if (cat[0] == 'F') return IM_COL32(150, 110, 40, 255);  // Flow
	return IM_COL32(150, 52, 52, 255);                      // Output
}

float NodeHeight(const PNode& n) {
	const size_t rows = std::max(NodeInputs(n.kind).size(),
		NodeOutputs(n.kind).size());
	return kHeaderH + (float)rows * kRowH + 10.0f;
}

// Pin screen position (canvas space -> caller adds origin+pan).
ImVec2 PinPos(const PNode& n, bool input, size_t idx) {
	const size_t rows = std::max(NodeInputs(n.kind).size(),
		NodeOutputs(n.kind).size());
	(void)rows;
	const float y = n.pos[1] + kHeaderH + 5.0f + float(idx) * kRowH + kRowH * 0.5f;
	return input ? ImVec2(n.pos[0], y) : ImVec2(n.pos[0] + kNodeW, y);
}

float BezierDist(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, ImVec2 m) {
	float best = 1e9f;
	for (int i = 1; i <= 20; ++i) {
		const float t = float(i) / 20.0f;
		const float u = 1.0f - t;
		ImVec2 q{
			u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
			u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y,
		};
		const float dx = q.x - m.x, dy = q.y - m.y;
		best = std::min(best, dx * dx + dy * dy);
	}
	return std::sqrt(best);
}

void WireCurve(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick) {
	const float dx = std::max(40.0f, std::abs(b.x - a.x) * 0.5f);
	dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b,
		col, thick);
}

} // namespace

// ---- selected-node params -------------------------------------------------------
void EditorApp::PushPlacementUndo() {
	m_placement.pushUndo();
	AutosaveTour(); // graphs ride the tour doc (img.*/place.* settings)
}
void EditorApp::DirtyPlacementOnEdit() {
	if (ImGui::IsItemEdited() || ImGui::IsItemDeactivatedAfterEdit()) {
		MarkPlacementDirty();
		if (ImGui::IsItemDeactivatedAfterEdit())
			AutosaveTour(); // once per finished edit, not per tick
	}
}

void EditorApp::DrawPlacementParams(GraphDoc& doc, PNode& n) {
	ImGui::Text("%s  (#%u)", NodeKindName(n.kind), n.id);
	ImGui::Separator();
	auto& F = n.f;
	switch (n.kind) {
	case NodeKind::Grid: {
		if (F.size() < 7) F.resize(7, 0);
		float o[3] = { F[0], F[1], F[2] };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::Vec3Compact("Origin", o, 0.5f,
				"The grid's world origin (m)")) {
			F[0] = o[0]; F[1] = o[1]; F[2] = o[2];
		}
		DirtyPlacementOnEdit();
		float e[2] = { F[3], F[4] };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::Vec2("Extent XZ", e, 0.5f, 1.0f, 512.0f, "%.1f",
				"The grid's X/Z extents (m)")) {
			F[3] = e[0]; F[4] = e[1];
		}
		DirtyPlacementOnEdit();
		float c[2] = { F[5], F[6] };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::Vec2("Cells XZ", c, 1.0f, 1.0f, 64.0f, "%.0f",
				"The cell counts per axis")) {
			F[5] = std::floor(c[0]); F[6] = std::floor(c[1]);
		}
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Mesh: {
		if (n.i.empty()) n.i = { -1 };
		int node = n.i[0];
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::IntBox("Scene node", &node,
				"The scene node whose glTF primitive is extracted at "
				"compile"))
			n.i[0] = node;
		DirtyPlacementOnEdit();
		if (ImGui::Button("Use selected")) {
			m_placement.pushUndo();
			n.i[0] = (m_selected == UINT32_MAX) ? -1 : (int)m_selected;
			MarkPlacementDirty();
		}
		{
			char selb[32];
			std::snprintf(selb, sizeof(selb), "%s",
				m_selected == UINT32_MAX
				? "-" : std::to_string(m_selected).c_str());
			attr::Readonly("Selected", selb);
		}
		if (n.i[0] < 0)
			ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "No scene node bound.");
		break;
	}
	case NodeKind::Terrain: {
		if (F.size() < 6) F = { 0, 0, 64, 64, 8, 0.02f };
		if (n.i.empty()) n.i = { 1 };
		float o[2] = { F[0], F[1] };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::Vec2("Origin XZ", o, 0.5f, 0.0f, 0.0f, "%.1f",
				"The terrain's world X/Z origin (m)"))
			{ F[0] = o[0]; F[1] = o[1]; }
		DirtyPlacementOnEdit();
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Size", &F[2], 0.5f, 1.0f, 1024.0f, "%.1f",
			"The terrain extent (m)");
		DirtyPlacementOnEdit();
		float res = F[3];
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::Float("Resolution", &res, 1.0f, 8.0f, 256.0f, "%.0f",
				"Samples per axis"))
			F[3] = std::floor(res);
		DirtyPlacementOnEdit();
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Hill height", &F[4], 0.1f, 0.0f, 128.0f, "%.1f",
			"The analytic hill amplitude (m)");
		DirtyPlacementOnEdit();
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Hill frequency", &F[5], 0.002f, 0.0005f, 1.0f, "%.4f",
			"The analytic hill frequency");
		DirtyPlacementOnEdit();
		int seed = n.i[0];
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::IntBox("Noise seed", &seed,
				"The value-noise seed (analytic hills)"))
			n.i[0] = seed;
		DirtyPlacementOnEdit();
		// Live terrain binding: scatter over a CREATED (painted) terrain
		// group instead of the analytic hills.
		if (n.i.size() < 2) n.i.resize(2, -1);
		ImGui::Separator();
		if (n.i[1] >= 0 && m_terrainGroups.contains((uint32_t)n.i[1])) {
			ImGui::Text("Bound to live terrain group #%u", (uint32_t)n.i[1]);
			ImGui::Text("Previews and scatter sample the PAINTED heights; "
				"painting marks this preview dirty.");
			if (ImGui::Button("Unbind (analytic)")) {
				PushPlacementUndo();
				n.i[1] = -1;
				MarkPlacementDirty();
			}
		}
		else {
			ImGui::Text("Unbound: analytic hills (two value-noise "
				"octaves, no scene geometry).");
			if (ImGui::Button("Use selected (bind live terrain)")) {
				const uint32_t group = TerrainGroupOf(m_selected);
				if (group != UINT32_MAX) {
					PushPlacementUndo();
					n.i[1] = int(group);
					MarkPlacementDirty();
				}
				else {}
			}
		}
		break;
	}
	case NodeKind::Density: {
		if (F.empty()) F = { 0.5f };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Slider("Density", &F[0], 0.0f, 1.0f, "%.2f",
			"The per-cell spawn probability");
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Noise: {
		if (F.size() < 3) F = { 0.02f, 0, 0 };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Frequency", &F[0], 0.002f, 0.0005f, 1.0f, "%.4f",
			"The value-noise frequency");
		DirtyPlacementOnEdit();
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Vec2("Offset", &F[1], 0.5f, 0.0f, 0.0f, "%.1f",
			"The noise coordinate offset");
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Slope: {
		if (F.size() < 2) F = { 0, 40 };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Flat deg", &F[0], 0.5f, 0.0f, 90.0f, "%.0f",
			"Slope below this passes fully");
		DirtyPlacementOnEdit();
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Steep deg", &F[1], 0.5f, 0.0f, 90.0f, "%.0f",
			"Slope above this rejects fully");
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Altitude: {
		if (F.size() < 4) F = { 0, 5, 15, 25 };
		const char* labels[4] = { "Lo in", "Lo full", "Hi full", "Hi out" };
		for (int k = 0; k < 4; ++k) {
			if (ImGui::IsItemActivated()) PushPlacementUndo();
			attr::Float(labels[k], &F[k], 0.5f, 0.0f, 0.0f, "%.1f",
				"The altitude band edge (m)");
			DirtyPlacementOnEdit();
		}
		break;
	}
	case NodeKind::Remap: {
		if (F.size() < 4) F = { 0, 1, 0, 1 };
		const char* labels[4] = { "In min", "In max", "Out min", "Out max" };
		for (int k = 0; k < 4; ++k) {
			if (ImGui::IsItemActivated()) PushPlacementUndo();
			attr::Float(labels[k], &F[k], 0.05f, 0.0f, 0.0f, "%.3f",
				"The remap range edge");
			DirtyPlacementOnEdit();
		}
		break;
	}
	case NodeKind::Smoothstep: {
		if (F.size() < 2) F = { 0, 1 };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Vec2("Edges", &F[0], 0.05f, 0.0f, 0.0f, "%.3f",
			"The smoothstep edge0/edge1 pair");
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Power: {
		if (F.empty()) F = { 2 };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Exponent", &F[0], 0.05f, 0.01f, 8.0f, "%.2f",
			"The power curve exponent");
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Accept: {
		if (F.empty()) F = { 1.0f };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Slider("Scale", &F[0], 0.0f, 2.0f, "%.2f",
			"Stochastic accept: raising density reveals new stable "
			"instances");
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Prototypes: {
		if (F.size() < 2) F = { 0, 35 };
		bool rock = F[0] != 0.0f;
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::Check("Slope to rock", &rock,
				"Flip to the rock prototype above this slope"))
			F[0] = rock ? 1.0f : 0.0f;
		DirtyPlacementOnEdit();
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Rock over deg", &F[1], 0.5f, 0.0f, 90.0f, "%.0f",
			"The slope threshold (deg)");
		DirtyPlacementOnEdit();
		const size_t rows = n.s.size();
		for (size_t r = 0; r < rows; ++r) {
			ImGui::PushID((int)r);
			char uri[256];
			std::snprintf(uri, sizeof(uri), "%s", n.s[r].c_str());
			if (ImGui::IsItemActivated()) PushPlacementUndo();
			if (attr::Text("URI", uri, sizeof(uri),
					"Resolves to a scene mesh at runtime (first cluster "
					"match wins)"))
				n.s[r] = uri;
			DirtyPlacementOnEdit();
			if (F.size() < 2 + 2 * (r + 1)) F.resize(2 + 2 * (r + 1), 0.5f);
			if (ImGui::IsItemActivated()) PushPlacementUndo();
			attr::Float("Weight", &F[2 + 2 * r], 0.05f, 0.0f, 100.0f,
				"%.2f", "The prototype's pick share");
			DirtyPlacementOnEdit();
			if (ImGui::IsItemActivated()) PushPlacementUndo();
			attr::Float("Radius", &F[2 + 2 * r + 1], 0.05f, 0.05f, 32.0f,
				"%.2f", "The prototype's spacing radius (m)");
			DirtyPlacementOnEdit();
			if (ImGui::Button("Remove row") && rows > 1) {
				m_placement.pushUndo();
				n.s.erase(n.s.begin() + r);
				F.erase(F.begin() + 2 + 2 * r, F.begin() + 2 + 2 * r + 2);
				MarkPlacementDirty();
				ImGui::PopID();
				break;
			}
			ImGui::Separator();
			ImGui::PopID();
		}
		if (rows < 8 && ImGui::Button("Add prototype")) {
			m_placement.pushUndo();
			n.s.push_back("Proto" + std::to_string(rows));
			n.f.push_back(1.0f);
			n.f.push_back(0.5f);
			MarkPlacementDirty();
		}
		{
			char rowsb[48];
			std::snprintf(rowsb, sizeof(rowsb), "%u (GPU kernel limit: 8)",
				(unsigned)n.s.size());
			attr::Readonly("Rows", rowsb);
		}
		break;
	}
	case NodeKind::Transform: {
		if (F.size() < 2) F = { 0.8f, 1.2f };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Min scale", &F[0], 0.02f, 0.01f, 32.0f, "%.2f",
			"The instance scale range floor");
		DirtyPlacementOnEdit();
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Max scale", &F[1], 0.02f, 0.01f, 32.0f, "%.2f",
			"The instance scale range ceiling");
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Spacing: {
		if (F.empty()) F = { 0.0f };
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		attr::Float("Min spacing", &F[0], 0.1f, 0.0f, 64.0f, "%.1f",
			"The spacing solver radius (m; 0 = off)");
		DirtyPlacementOnEdit();
		break;
	}
	case NodeKind::Layer: {
		if (F.empty()) F = { 4096 };
		int cands = (int)F[0];
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::Int("Candidates", &cands, 64, 65536,
				"The per-tile candidate cap"))
			F[0] = (float)cands;
		DirtyPlacementOnEdit();
		if (n.s.empty()) n.s = { "1234" };
		char seed[64];
		std::snprintf(seed, sizeof(seed), "%s", n.s[0].c_str());
		if (ImGui::IsItemActivated()) PushPlacementUndo();
		if (attr::Text("Seed", seed, sizeof(seed),
				"Stable IDs derive from seed+tile+index: streaming order "
				"never reshuffles"))
			n.s[0] = seed;
		DirtyPlacementOnEdit();
		break;
	}
	default:
		ImGui::Text("No parameters (wire inputs).");
		break;
	}
}

// ---- canvas ------------------------------------------------------------------
void EditorApp::DrawPlacementGraph() {
	GraphDoc* doc = m_placement.current();
	ImGui::SetNextWindowSize(ImVec2(1100, 720), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Placement Graph", &m_placement.showGraph)) {
		ImGui::End();
		return;
	}
	// Keyboard-focus ownership of the Del key: while any window of this
	// hierarchy is focused, the canvas's own Del (node/wire removal)
	// wins and ModalDelete stands down. Reset by DrawUI each frame.
	m_imguiFocusInPlacementGraph =
		ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	// Toolbar: graph select/add/delete, undo/redo, badges, preview controls.
	{
		if (ImGui::BeginCombo("Graph", doc ? doc->name.c_str() : "-")) {
			for (const auto& [gid, g] : m_placement.graphs) {
				const bool sel = gid == m_placement.active;
				if (ImGui::Selectable((g.name + "##" + std::to_string(gid)).c_str(), sel)) {
					m_placement.active = gid;
					m_placement.selected = -1;
					m_placement.selectedLink = -1;
					m_placement.linkFrom = -1;
					MarkPlacementDirty();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("New")) {
			m_placement.pushUndo();
			m_placement.addGraph("");
			MarkPlacementDirty();
		}
		ImGui::SameLine();
		if (ImGui::Button("Undo") && m_placement.undo()) MarkPlacementDirty();
		ImGui::SameLine();
		if (ImGui::Button("Redo") && m_placement.redo()) MarkPlacementDirty();
		ImGui::SameLine();
		if (ImGui::Button("Refresh")) MarkPlacementDirty();
		ImGui::SameLine();
		ImGui::Checkbox("Overlay", &m_placement.showOverlay);
		ImGui::SameLine();
		ImGui::Checkbox("Rejected", &m_placement.showRejected);
		if (!doc) {
			ImGui::TextDisabled("No graph: create one to start scattering.");
			ImGui::End();
			return;
		}
	}
	if (m_placement.preview.dirty)
		RefreshPlacementPreview();
	PreviewCache& pv = m_placement.preview;
	// Status line: compile + GPU badge + stats.
	{
		if (pv.ok) {
			ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.55f, 1), "Compiled OK");
			ImGui::SameLine();
			ImGui::TextDisabled("| GPU runtime: OK");
			ImGui::SameLine();
			ImGui::TextDisabled("| %s", pv.stats.c_str());
		}
		else {
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "Error: %s",
				pv.error.c_str());
		}
		if (!pv.compiled.warning.empty())
			ImGui::TextDisabled("Note: %s", pv.compiled.warning.c_str());
	}
	// Canvas + side panel split.
	const float panelW = 340.0f;
	ImVec2 canvasSz(-panelW - 8.0f, -230.0f);
	ImGui::BeginChild("##pcanvas", canvasSz, true,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		auto toScreen = [&](ImVec2 p) {
			return ImVec2(origin.x + p.x + m_placement.pan[0],
				origin.y + p.y + m_placement.pan[1]);
		};
		// Pan: MMB drag anywhere; wheel = vertical pan.
		if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
			m_placement.pan[0] += ImGui::GetIO().MouseDelta.x;
			m_placement.pan[1] += ImGui::GetIO().MouseDelta.y;
		}
		// Grid dots.
		{
			const ImVec2 sz = ImGui::GetWindowSize();
			for (float gx = fmodf(m_placement.pan[0], 28.0f); gx < sz.x; gx += 28.0f)
				for (float gy = fmodf(m_placement.pan[1], 28.0f); gy < sz.y; gy += 28.0f)
					dl->AddCircleFilled(ImVec2(origin.x + gx, origin.y + gy), 1.0f,
						IM_COL32(255, 255, 255, 22));
		}
		// Wires (under nodes).
		for (const auto& [lid, l] : doc->links) {
			const PNode* a = doc->find(l.from);
			const PNode* b = doc->find(l.to);
			if (!a || !b) continue;
			int ai = -1, bi = -1;
			const auto outs = NodeOutputs(a->kind);
			for (size_t k = 0; k < outs.size(); ++k)
				if (outs[k].name == l.fromPin) ai = (int)k;
			const auto ins = NodeInputs(b->kind);
			for (size_t k = 0; k < ins.size(); ++k)
				if (ins[k].name == l.toPin) bi = (int)k;
			if (ai < 0 || bi < 0) continue;
			ImVec2 sa = toScreen(PinPos(*a, false, (size_t)ai));
			ImVec2 sb = toScreen(PinPos(*b, true, (size_t)bi));
			PinType pt = PinType::Field;
			for (const PinDef& p : outs)
				if (p.name == l.fromPin) pt = p.type;
			const bool sel = m_placement.selectedLink == (int)lid;
			WireCurve(dl, sa, sb, sel ? IM_COL32(255, 255, 120, 255) : PinColor(pt),
				sel ? 3.0f : 2.0f);
			// Click-select the wire.
			if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
				&& m_placement.linkFrom < 0) {
				const float dx = std::max(40.0f, std::abs(sb.x - sa.x) * 0.5f);
				if (BezierDist(sa, ImVec2(sa.x + dx, sa.y),
						ImVec2(sb.x - dx, sb.y), sb, mouse) < 7.0f) {
					m_placement.selectedLink = (int)lid;
					m_placement.selected = -1;
				}
			}
		}
		// Link-drag temp wire.
		if (m_placement.linkFrom >= 0) {
			const PNode* a = doc->find((uint32_t)m_placement.linkFrom);
			if (a) {
				const auto outs = NodeOutputs(a->kind);
				for (size_t k = 0; k < outs.size(); ++k)
					if (outs[k].name == m_placement.linkFromPin)
						WireCurve(dl, toScreen(PinPos(*a, false, k)), mouse,
							PinColor(outs[k].type), 2.0f);
			}
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				m_placement.linkFrom = -1; // dropped on empty space
		}
		// Nodes.
		for (auto& [nid, n] : doc->nodes) {
			const float h = NodeHeight(n);
			ImVec2 ns = toScreen(ImVec2(n.pos[0], n.pos[1]));
			ImGui::SetCursorScreenPos(ns);
			ImGui::InvisibleButton(("##node" + std::to_string(nid)).c_str(),
				ImVec2(kNodeW, h));
			const bool hovered = ImGui::IsItemHovered();
			const bool active = ImGui::IsItemActive();
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
				m_placement.selected = (int)nid;
				m_placement.selectedLink = -1;
			}
			if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)
				&& m_placement.linkFrom < 0) {
				if (ImGui::IsItemActivated()) m_placement.pushUndo();
				n.pos[0] += ImGui::GetIO().MouseDelta.x;
				n.pos[1] += ImGui::GetIO().MouseDelta.y;
				MarkPlacementDirty(); // positions only: cheap, keeps overlay live
			}
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
				m_placement.selected = (int)nid;
				ImGui::OpenPopup(("nodectx" + std::to_string(nid)).c_str());
			}
			const bool sel = m_placement.selected == (int)nid;
			// Body.
			dl->AddRectFilled(ns, ImVec2(ns.x + kNodeW, ns.y + h),
				IM_COL32(30, 32, 38, 245), 6.0f);
			dl->AddRectFilled(ns, ImVec2(ns.x + kNodeW, ns.y + kHeaderH),
				KindHeaderColor(n.kind), 6.0f);
			dl->AddRectFilled(ImVec2(ns.x, ns.y + kHeaderH - 6.0f),
				ImVec2(ns.x + kNodeW, ns.y + kHeaderH), KindHeaderColor(n.kind));
			dl->AddRect(ns, ImVec2(ns.x + kNodeW, ns.y + h),
				sel ? IM_COL32(255, 230, 120, 255) : IM_COL32(90, 95, 105, 255),
				6.0f, 0, sel ? 2.0f : 1.0f);
			dl->AddText(ImVec2(ns.x + 8.0f, ns.y + 5.0f),
				IM_COL32(255, 255, 255, 255), NodeKindName(n.kind));
			char idbuf[16];
			std::snprintf(idbuf, sizeof(idbuf), "#%u", nid);
			dl->AddText(ImVec2(ns.x + kNodeW - 8.0f -
				ImGui::CalcTextSize(idbuf).x, ns.y + 5.0f),
				IM_COL32(255, 255, 255, 160), idbuf);
			// Pins (buttons after body so they win hits).
			const auto ins = NodeInputs(n.kind);
			const auto outs = NodeOutputs(n.kind);
			for (size_t k = 0; k < ins.size(); ++k) {
				ImVec2 pc = toScreen(PinPos(n, true, k));
				dl->AddCircleFilled(pc, 5.0f, PinColor(ins[k].type));
				dl->AddText(ImVec2(pc.x + 9.0f, pc.y - 7.0f),
					IM_COL32(200, 200, 205, 255), ins[k].name);
				ImGui::SetCursorScreenPos(ImVec2(pc.x - 8.0f, pc.y - 8.0f));
				ImGui::InvisibleButton(
					("##pin" + std::to_string(nid) + "i" + std::to_string(k)).c_str(),
					ImVec2(16, 16));
				if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
					&& m_placement.linkFrom >= 0) {
					// Complete the link drag.
					const PNode* src = doc->find((uint32_t)m_placement.linkFrom);
					uint32_t lid = 0;
					if (src) {
						// Replace existing wire on this input (single-wire).
						if (const PLink* old = doc->inputLink(nid, ins[k].name)) {
							m_placement.pushUndo();
							doc->removeLink(old->id);
						}
						m_placement.pushUndo();
						lid = doc->addLink((uint32_t)m_placement.linkFrom,
							m_placement.linkFromPin, nid, ins[k].name);
						MarkPlacementDirty();
					}
					if (!lid) Log("placement: pins are incompatible (types must match).");
					m_placement.linkFrom = -1;
				}
			}
			for (size_t k = 0; k < outs.size(); ++k) {
				ImVec2 pc = toScreen(PinPos(n, false, k));
				dl->AddCircleFilled(pc, 5.0f, PinColor(outs[k].type));
				const float tw = ImGui::CalcTextSize(outs[k].name).x;
				dl->AddText(ImVec2(pc.x - 9.0f - tw, pc.y - 7.0f),
					IM_COL32(200, 200, 205, 255), outs[k].name);
				ImGui::SetCursorScreenPos(ImVec2(pc.x - 8.0f, pc.y - 8.0f));
				ImGui::InvisibleButton(
					("##pin" + std::to_string(nid) + "o" + std::to_string(k)).c_str(),
					ImVec2(16, 16));
				if (ImGui::IsItemActivated()
					&& ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
					m_placement.linkFrom = (int)nid;
					m_placement.linkFromPin = outs[k].name;
				}
			}
			// Node context menu.
			if (ImGui::BeginPopup(("nodectx" + std::to_string(nid)).c_str())) {
				if (ImGui::MenuItem("Duplicate")) {
					m_placement.pushUndo();
					PNode copy = n;
					copy.id = doc->allocId();
					copy.pos[0] += 28.0f;
					copy.pos[1] += 28.0f;
					doc->nodes[copy.id] = copy;
					m_placement.selected = (int)copy.id;
					MarkPlacementDirty();
				}
				if (ImGui::MenuItem("Delete")) {
					m_placement.pushUndo();
					doc->removeNode(nid);
					m_placement.selected = -1;
					MarkPlacementDirty();
				}
				ImGui::EndPopup();
			}
		}
		// Background context menu (add nodes).
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Right)
			&& m_placement.linkFrom < 0) {
			// Only when not on a node: node ctx takes precedence via item
			// hover; check no node hovered.
			bool onNode = false;
			for (const auto& [nid, n] : doc->nodes) {
				ImVec2 ns = toScreen(ImVec2(n.pos[0], n.pos[1]));
				if (mouse.x >= ns.x && mouse.x <= ns.x + kNodeW
					&& mouse.y >= ns.y && mouse.y <= ns.y + NodeHeight(n)) {
					onNode = true;
					break;
				}
			}
			if (!onNode) ImGui::OpenPopup("##pcanvasctx");
		}
		if (ImGui::BeginPopup("##pcanvasctx")) {
			const char* lastCat = "";
			for (int k = 0; k < (int)NodeKind::Count; ++k) {
				const NodeKind kind = (NodeKind)k;
				const char* cat = NodeKindCategory(kind);
				if (cat != lastCat) {
					if (k > 0) ImGui::Separator();
					ImGui::TextDisabled("%s", cat);
					lastCat = cat;
				}
				char addLabel[64];
				// ## suffix: the visible name stays clean while the ID can
				// never collide even if two kinds ever share a name.
				std::snprintf(addLabel, sizeof(addLabel), "%s##addnode%d",
					NodeKindName(kind), k);
				if (ImGui::MenuItem(addLabel)) {
					m_placement.pushUndo();
					const uint32_t nid = doc->addNode(kind,
						mouse.x - origin.x - m_placement.pan[0] - kNodeW * 0.5f,
						mouse.y - origin.y - m_placement.pan[1] - 20.0f);
					m_placement.selected = (int)nid;
					MarkPlacementDirty();
				}
			}
			ImGui::EndPopup();
		}
		// Delete key. Focus may sit anywhere in this window hierarchy:
		// toolbar controls (Graph combo, New/Undo/...) live in the PARENT
		// window, so a canvas-only focus check made Del dead after using
		// them. RootAndChild covers parent + canvas + panels; the
		// !IsAnyItemActive() guard still blocks text fields and drags.
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			&& !ImGui::IsAnyItemActive()
			&& (ImGui::IsKeyPressed(ImGuiKey_Delete)
				|| ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
			if (m_placement.selectedLink >= 0) {
				m_placement.pushUndo();
				doc->removeLink((uint32_t)m_placement.selectedLink);
				m_placement.selectedLink = -1;
				MarkPlacementDirty();
			}
			else if (m_placement.selected >= 0) {
				m_placement.pushUndo();
				doc->removeNode((uint32_t)m_placement.selected);
				m_placement.selected = -1;
				MarkPlacementDirty();
			}
		}
		ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 8.0f));
		ImGui::TextDisabled("drag pins to link - MMB/wheel: pan - RMB: add - Del: remove");
	}
	ImGui::EndChild();
	ImGui::SameLine();
	// Side panel: params + trace.
	ImGui::BeginChild("##ppanel", ImVec2(-1, -230.0f), true);
	{
		if (m_placement.selected >= 0) {
			if (PNode* n = doc->find((uint32_t)m_placement.selected))
				DrawPlacementParams(*doc, *n);
			else
				m_placement.selected = -1;
		}
		else {
			ImGui::TextDisabled("Select a node to edit its parameters.");
			ImGui::TextDisabled("Select a wire to delete it.");
		}
	}
	ImGui::EndChild();
	// Bottom: trace + preview list.
	ImGui::BeginChild("##ptrace", ImVec2(-1, -1), true);
	{
		if (!pv.ok) {
			ImGui::TextDisabled("Fix the graph error to preview and trace instances.");
		}
		else {
			ImGui::Text("Accepted: %u   Rejected shown: %u",
				(unsigned)pv.accepted.size(), (unsigned)pv.rejected.size());
			ImGui::SameLine();
			static int traceIdx = 0;
			ImGui::SetNextItemWidth(120.0f);
			ImGui::InputInt("Trace #", &traceIdx, 1, 10);
			if (traceIdx < 0) traceIdx = 0;
			if (traceIdx >= 0 && (size_t)traceIdx < pv.accepted.size())
				pv.picked = traceIdx;
			else if ((size_t)traceIdx >= pv.accepted.size())
				pv.picked = -1;
			if (pv.picked >= 0 && (size_t)pv.picked < pv.accepted.size()) {
				const PreviewPoint& pp = pv.accepted[(size_t)pv.picked];
				char idb[96];
				std::snprintf(idb, sizeof(idb), "%08x %08x %08x %08x",
					pp.id.w[0], pp.id.w[1], pp.id.w[2], pp.id.w[3]);
				ImGui::Text("pos (%.2f, %.2f, %.2f)  proto %u  stable %s",
					pp.p.x, pp.p.y, pp.p.z, pp.proto, idb);
				// Re-evaluate the field program at this point: per-node values.
				DomainSample s = (pv.domainKind == DomainKind::Grid)
					? pv.grid.sample(pp.id, pp.candidate)
					: (pv.domainKind == DomainKind::Heightfield
						? pv.terrainDom.sample(pp.id)
						: SampleMeshSurface(pv.mesh, pp.id));
				std::vector<float> values = EvaluateProgram(pv.compiled.program, s);
				if (ImGui::BeginTable("##ptracevals", 2,
					ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
					ImGui::TableSetupColumn("Node");
					ImGui::TableSetupColumn("Value");
					ImGui::TableHeadersRow();
					for (size_t vi = 0; vi < values.size(); ++vi) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						int enid = vi < pv.compiled.instrToNode.size()
							? pv.compiled.instrToNode[vi] : -1;
						const PNode* en = enid >= 0 ? doc->find((uint32_t)enid) : nullptr;
						char nb[80];
						// vi in the visible label AND the ID: one editor node
						// can own several instructions (Accept scale-folding
						// emits two), and identical labels would share an ID.
						std::snprintf(nb, sizeof(nb), "v%u: %s #%d##trace%u",
							(unsigned)vi, en ? NodeKindName(en->kind) : "?",
							enid, (unsigned)vi);
						if (ImGui::Selectable(nb, false,
							ImGuiSelectableFlags_SpanAllColumns))
							if (en) m_placement.selected = enid;
						ImGui::TableNextColumn();
						ImGui::Text("%.4g", (double)values[vi]);
					}
					ImGui::EndTable();
				}
			}
			else {
				ImGui::TextDisabled("Enter an accepted-point index to trace it through the graph.");
			}
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

// ---- viewport overlay -----------------------------------------------------------
void EditorApp::DrawPlacementOverlay() {
	PreviewCache& pv = m_placement.preview;
	if (!m_placement.showOverlay)
		return;
	if (pv.dirty)
		RefreshPlacementPreview();
	if (!pv.ok)
		return;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const auto protoColor = [](uint32_t proto, int alpha) {
		const float hue = std::fmod(float(proto) * 0.23f + 0.33f, 1.0f);
		ImColor c = ImColor::HSV(hue, 0.75f, 1.0f);
		c.Value.w = float(alpha) / 255.0f;
		return (ImU32)c;
	};
	for (int i = 0; i < ViewportCount(); ++i) {
		const EditorViewport& vp = m_viewports[i];
		if (vp.rect.w < 40 || vp.rect.h < 40)
			continue;
		const glm::mat4 vpm = vp.viewProj;
		const auto project = [&](const glm::vec3& w, ImVec2& out) {
			const glm::vec4 c = vpm * glm::vec4(w, 1.0f);
			if (c.w <= 1e-6f)
				return false;
			// The projection's near plane (reversed-Z ZO: ndc z = 1) and
			// the far side (z = 0): markers between the eye and near used
			// to draw over the view through the eye-plane-only test.
			if (c.z < 0.0f || c.z > c.w)
				return false;
			out.x = vp.rect.x + (c.x / c.w * 0.5f + 0.5f) * float(vp.rect.w);
			out.y = vp.rect.y + (c.y / c.w * 0.5f + 0.5f) * float(vp.rect.h);
			return true;
		};
		dl->PushClipRect(ImVec2(vp.rect.x, vp.rect.y),
			ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h), true);
		// Rejected first (under), then accepted.
		if (m_placement.showRejected) {
			size_t step = pv.rejected.size() > 1000 ? pv.rejected.size() / 1000 : 1;
			for (size_t k = 0; k < pv.rejected.size(); k += step) {
				ImVec2 s;
				if (!project(pv.rejected[k].p, s)) continue;
				dl->AddRectFilled(ImVec2(s.x - 1, s.y - 1), ImVec2(s.x + 1, s.y + 1),
					IM_COL32(255, 70, 60, 140));
			}
		}
		{
			size_t step = pv.accepted.size() > 4000 ? pv.accepted.size() / 4000 : 1;
			for (size_t k = 0; k < pv.accepted.size(); k += step) {
				ImVec2 s;
				if (!project(pv.accepted[k].p, s)) continue;
				const ImU32 col = protoColor(pv.accepted[k].proto, 235);
				dl->AddRectFilled(ImVec2(s.x - 1.5f, s.y - 1.5f),
					ImVec2(s.x + 1.5f, s.y + 1.5f), col);
			}
		}
		// Scatter-rect bounds (ground rect + label): grid tile or terrain field.
		if (pv.domainKind == DomainKind::Grid
			|| pv.domainKind == DomainKind::Heightfield) {
			float x0, z0, x1, z1, y;
			if (pv.domainKind == DomainKind::Grid) {
				const GridDomain& g = pv.grid;
				x0 = g.origin.x; z0 = g.origin.z;
				x1 = g.origin.x + g.extent.x; z1 = g.origin.z + g.extent.y;
				y = g.origin.y;
			}
			else {
				const HeightfieldDomain& h = pv.terrainDom;
				x0 = h.extentMin.x; z0 = h.extentMin.y;
				x1 = h.extentMax.x; z1 = h.extentMax.y;
				y = 0.5f * (pv.terrainField.sampleHeight(x0, z0)
					+ pv.terrainField.sampleHeight(x1, z1));
			}
			glm::vec3 corners[4] = {
				{ x0, y, z0 }, { x1, y, z0 }, { x1, y, z1 }, { x0, y, z1 },
			};
			ImVec2 s[4];
			bool ok = true;
			for (int k = 0; k < 4; ++k) ok &= project(corners[k], s[k]);
			if (ok) {
				for (int k = 0; k < 4; ++k)
					dl->AddLine(s[k], s[(k + 1) % 4], IM_COL32(240, 220, 90, 220), 1.5f);
				char label[64];
				std::snprintf(label, sizeof(label), "placement: %u pts",
					(unsigned)pv.accepted.size());
				dl->AddText(ImVec2(s[0].x + 4.0f, s[0].y - 10.0f),
					IM_COL32(240, 220, 90, 255), label);
			}
		}
		// Picked point cross.
		if (pv.picked >= 0 && (size_t)pv.picked < pv.accepted.size()) {
			ImVec2 s;
			if (project(pv.accepted[(size_t)pv.picked].p, s)) {
				dl->AddLine(ImVec2(s.x - 7, s.y), ImVec2(s.x + 7, s.y),
					IM_COL32(255, 255, 255, 255), 2.0f);
				dl->AddLine(ImVec2(s.x, s.y - 7), ImVec2(s.x, s.y + 7),
					IM_COL32(255, 255, 255, 255), 2.0f);
			}
		}
		dl->PopClipRect();
	}
}

} // namespace editor
