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
// IDs: every field's ImGui id is "##<attr name>" — the ## prefix keeps
// the widget's own label invisible (the name renders in the shared
// label column) while the name-derived suffix keeps the ids UNIQUE
// (the first cut's bare "##v" collided every same-typed row in a
// window).
#include <imgui.h>
#include <cmath>
#include <string>

namespace editor::attr {

// The label column width (shared: the value fields align across rows).
inline constexpr float kLabelW = 130.0f;

// The unique-but-invisible widget label for an attribute name.
inline std::string Id(const char* name) {
	return std::string("##") + name;
}

// ---- BOOL: the flag chip (on = the selected accent; the hint carries
// the words) ---------------------------------------------------------------
inline bool Bool(const char* name, bool v, const char* hint) {
	ImGui::TextUnformatted(name);
	ImGui::SameLine(kLabelW);
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
	return clicked;
}

// ---- FLOAT: the drag (label column + the field) ---------------------------
inline bool Float(const char* name, float* v, float speed, float mn,
	float mx, const char* fmt, const char* hint) {
	ImGui::TextUnformatted(name);
	ImGui::SameLine(kLabelW);
	ImGui::SetNextItemWidth(-60.0f);
	const bool edited = ImGui::DragFloat(Id(name).c_str(), v, speed, mn,
		mx, fmt ? fmt : "%.3f");
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- SLIDER float (bounded look knobs) ------------------------------------
inline bool Slider(const char* name, float* v, float mn, float mx,
	const char* fmt, const char* hint) {
	ImGui::TextUnformatted(name);
	ImGui::SameLine(kLabelW);
	ImGui::SetNextItemWidth(-60.0f);
	const bool edited = ImGui::SliderFloat(Id(name).c_str(), v, mn, mx,
		fmt ? fmt : "%.2f");
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- INT (drag) ------------------------------------------------------------
inline bool Int(const char* name, int* v, int mn, int mx,
	const char* hint) {
	ImGui::TextUnformatted(name);
	ImGui::SameLine(kLabelW);
	ImGui::SetNextItemWidth(-60.0f);
	const bool edited = ImGui::DragInt(Id(name).c_str(), v, 1, mn, mx);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- VEC3 (the transform rows) ---------------------------------------------
inline bool Vec3(const char* name, float v[3], float speed, const char* hint) {
	ImGui::TextUnformatted(name);
	ImGui::SameLine(kLabelW);
	ImGui::SetNextItemWidth(-60.0f);
	const bool edited = ImGui::DragFloat3(Id(name).c_str(), v, speed);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- COLOR -----------------------------------------------------------------
inline bool Color(const char* name, float v[4], const char* hint) {
	ImGui::TextUnformatted(name);
	ImGui::SameLine(kLabelW);
	ImGui::SetNextItemWidth(-60.0f);
	const bool edited = ImGui::ColorEdit4(Id(name).c_str(), v);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- ENUM (the combo; items = a "A\0B\0\0" list) ----------------------------
inline bool Enum(const char* name, int* v, const char* items,
	const char* hint) {
	ImGui::TextUnformatted(name);
	ImGui::SameLine(kLabelW);
	ImGui::SetNextItemWidth(-60.0f);
	const bool edited = ImGui::Combo(Id(name).c_str(), v, items);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

// ---- TEXT (the name field) --------------------------------------------------
inline bool Text(const char* name, char* buf, size_t sz, const char* hint) {
	ImGui::TextUnformatted(name);
	ImGui::SameLine(kLabelW);
	ImGui::SetNextItemWidth(-60.0f);
	const bool edited = ImGui::InputText(Id(name).c_str(), buf, sz,
		ImGuiInputTextFlags_EnterReturnsTrue);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	return edited;
}

} // namespace editor::attr
