// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// THE ATTRIBUTE WIDGET STUBS — one per attribute type. The contract for
// every attribute row in the editor UI:
//   [ATTR NAME] [VALUE FIELD] — a shared label column + the typed
//   control; the semantics/hints ride the MOUSE-OVER (never a text row
//   below the attribute). Booleans render as flag chips (the on-state
//   reads as the theme's selected accent — the attribute's OWN two
//   states, never a negation-named checkbox). Every stub returns true
//   on the EDIT FRAME (the caller drives the command/undo wiring).
//
// IDs: every stub PushID(name)s around its widget, so the widget id is
// the PROPERTY NAME regardless of the visible label ("on"/"off" chip
// text never leaks into the id — the old bare Button("on"/"off")
// collided every same-state boolean in one id scope, the material
// inspector's DS/Shadow/Contact chips being the live repro). The ##-
// suffixed labels stay for the drag/slider fields (invisible labels,
// name-derived ids).
//
// LAYOUT: the label column is MEASURED per window (LabelColumnWidth
// below) instead of a hardcoded 130 px — the inspector's narrow 370 px
// starting width made -60 trailing reservations leave dead space and
// long labels collide. Each helper reserves the column, then the value
// field takes the REST of the row (-1 width: no hardcoded trailing
// gap; callers that need a trailing button size it themselves).
#include <imgui.h>
#include <cmath>
#include <string>

namespace editor::attr {

// The label column width: MEASURED against the window's content width
// (42% clamped to [90, 220] px — the two-column property contract at
// any inspector width/DPI; a long label truncates with a full-text
// tooltip instead of colliding into the value field).
inline float LabelColumnWidth() {
	const float w = ImGui::GetContentRegionAvail().x;
	return std::clamp(w * 0.42f, 90.0f, 220.0f);
}

// One property row's label cell: the name CLIPPED to the column (a
// long label never collides into the value field) with the full text
// on the tooltip when it clipped; the hint tooltip wins when present.
inline void LabelCell(const char* name, const char* hint) {
	const float colW = LabelColumnWidth();
	const char* shown = name ? name : "";
	const ImVec2 ts = ImGui::CalcTextSize(shown);
	const float avail = colW - 10.0f;
	const bool clipped = ts.x > avail;
	if (clipped) {
		// Clip at the column edge (the visual cut IS the column rule);
		// the tooltip carries the full name.
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::PushClipRect(pos, ImVec2(pos.x + avail, pos.y + ts.y + 2.0f),
			true);
		ImGui::TextUnformatted(shown);
		ImGui::PopClipRect();
	}
	else {
		ImGui::TextUnformatted(shown);
	}
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	else if (clipped && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", shown);
	ImGui::SameLine(colW);
}

// The unique-but-invisible widget label for an attribute name.
inline std::string Id(const char* name) {
	return std::string("##") + name;
}

// ---- BOOL: the flag chip (on = the selected accent; the hint carries
// the words). PushID(name): the chip text NEVER forms the id.
inline bool Bool(const char* name, bool v, const char* hint) {
	LabelCell(name, hint);
	ImGui::PushID(name); // the chip's id IS the property name
	if (v) {
		ImGui::PushStyleColor(ImGuiCol_Button,
			ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
			ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,
			ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	}
	const bool clicked = ImGui::Button(v ? "on" : "off",
		ImVec2(52.0f, 0.0f));
	if (v)
		ImGui::PopStyleColor(3);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	ImGui::PopID();
	return clicked;
}

// ---- FLOAT: the drag (label column + the field) ---------------------------
inline bool Float(const char* name, float* v, float speed, float mn,
	float mx, const char* fmt, const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f); // the rest of the row - no dead gap
	const bool edited = ImGui::DragFloat(Id(name).c_str(), v, speed, mn,
		mx, fmt ? fmt : "%.3f");
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- SLIDER float (bounded look knobs) ------------------------------------
inline bool Slider(const char* name, float* v, float mn, float mx,
	const char* fmt, const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	const bool edited = ImGui::SliderFloat(Id(name).c_str(), v, mn, mx,
		fmt ? fmt : "%.2f");
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- INT (drag; speed 1 = the arrow-key step) ------------------------------
inline bool Int(const char* name, int* v, int mn, int mx,
	const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	const bool edited = ImGui::DragInt(Id(name).c_str(), v, 1, mn, mx);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- VEC3 (the transform rows): EXPANDABLE per-axis drags. A
// DragFloat3 crams three sub-fields into one row's remainder — at the
// inspector's narrow widths each axis gets a sliver. The row shows the
// compact "(x, y, z)" summary + a disclosure chip; expanded (the
// default) draws three FULL-WIDTH axis drags aligned under the value
// column. The item state the undo wiring needs (IsItemActivated /
// IsItemDeactivatedAfterEdit) is AGGREGATED across the axis rows into
// the result — a multi-item widget cannot leave "the item" on the last
// axis drawn.
struct Vec3Result {
	bool edited = false;     // any axis changed this frame
	bool activated = false;  // any axis drag started this frame
	bool finished = false;   // an edited axis released this frame
};
inline Vec3Result Vec3(const char* name, float v[3], float speed,
	const char* hint) {
	Vec3Result r;
	LabelCell(name, hint);
	ImGui::PushID(name);
	// The disclosure state: the window's storage, keyed by the property
	// id (survives frames; per-window like every ImGui open state).
	const ImGuiID openId = ImGui::GetID("vec3open");
	bool open = ImGui::GetStateStorage()->GetBool(openId, true);
	if (ImGui::Button(open ? "-##vec3" : "+##vec3", ImVec2(18.0f, 0.0f))) {
		open = !open;
		ImGui::GetStateStorage()->SetBool(openId, open);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%.2f, %.2f, %.2f", v[0], v[1], v[2]);
	if (open) {
		static const char* kAxes[3] = { "X", "Y", "Z" };
		const float colW = LabelColumnWidth();
		for (int i = 0; i < 3; ++i) {
			ImGui::Indent(colW); // align under the value column
			ImGui::SetNextItemWidth(
				ImGui::GetContentRegionAvail().x - 16.0f);
			if (ImGui::DragFloat("##axis", &v[i], speed, 0.0f, 0.0f,
					"%.3f"))
				r.edited = true;
			if (ImGui::IsItemActivated())
				r.activated = true;
			if (ImGui::IsItemDeactivatedAfterEdit())
				r.finished = true;
			ImGui::SameLine();
			ImGui::TextDisabled("%s", kAxes[i]);
			ImGui::Unindent(colW);
		}
	}
	ImGui::PopID();
	return r;
}

// ---- VEC3 compact (the single-row DragFloat3; for wide hosts where
// three sub-fields still read well) ---------------------------------------
inline bool Vec3Compact(const char* name, float v[3], float speed,
	const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	const bool edited = ImGui::DragFloat3(Id(name).c_str(), v, speed);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- COLOR -----------------------------------------------------------------
inline bool Color(const char* name, float v[4], const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	const bool edited = ImGui::ColorEdit4(Id(name).c_str(), v);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- ENUM (the combo; items = a "A\0B\0\0" list) ----------------------------
inline bool Enum(const char* name, int* v, const char* items,
	const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	const bool edited = ImGui::Combo(Id(name).c_str(), v, items);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- TEXT (the name field) --------------------------------------------------
inline bool Text(const char* name, char* buf, size_t sz, const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	const bool edited = ImGui::InputText(Id(name).c_str(), buf, sz,
		ImGuiInputTextFlags_EnterReturnsTrue);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

} // namespace editor::attr
