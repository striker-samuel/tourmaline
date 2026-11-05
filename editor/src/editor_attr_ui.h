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
#include "editor_numeric.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace editor::attr {

// Scoped component/instance identity. Use stable semantic keys, not display
// labels or positions in a list. RAII keeps the ImGui ID stack balanced when
// an inspector section returns early.
class ScopedID {
public:
	explicit ScopedID(const char* key) { ImGui::PushID(key); }
	explicit ScopedID(int key) { ImGui::PushID(key); }
	~ScopedID() { ImGui::PopID(); }

	ScopedID(const ScopedID&) = delete;
	ScopedID& operator=(const ScopedID&) = delete;
};


// Reserve a stable name/value gutter in every property row. The label
// must never be left-clipped just because the dock was resized: below
// the minimum two-column width, stack the full label above the control.
inline float LabelColumnWidth() {
	const float w = ImGui::GetContentRegionAvail().x;
	return std::clamp(w * 0.48f, 156.0f, 220.0f);
}

// Attribute provenance scoped to the Inspector: 1=local, 2=wide rule.
inline const std::unordered_map<std::string, int>* overrideLabels = nullptr;
class ScopedOverrideLabels {
public:
	explicit ScopedOverrideLabels(const std::unordered_map<std::string, int>* labels)
		: previous_(overrideLabels) { overrideLabels = labels; }
	~ScopedOverrideLabels() { overrideLabels = previous_; }
private:
	const std::unordered_map<std::string, int>* previous_;
};

// Draw the full property name. At normal widths it is right-aligned in
// a reserved label column, separated from the value by a fixed gutter.
// When there is not enough room for both columns, use a stacked layout.
inline void LabelCell(const char* name, const char* hint) {
	const char* shown = name ? name : "";
	const float available = ImGui::GetContentRegionAvail().x;
	const float colW = LabelColumnWidth();
	const float startX = ImGui::GetCursorPosX();
	const bool stacked = available < colW + 112.0f;
	int overrideKind = 0;
	if (overrideLabels) {
		const auto it = overrideLabels->find(shown);
		if (it != overrideLabels->end()) overrideKind = it->second;
	}
	if (overrideKind)
		ImGui::PushStyleColor(ImGuiCol_Text, overrideKind == 1
			? ImVec4(1.0f, 0.69f, 0.23f, 1.0f)
			: ImVec4(0.98f, 0.43f, 0.70f, 1.0f));
	if (stacked) {
		ImGui::TextWrapped("%s", shown);
	} else {
		const float nameW = ImGui::CalcTextSize(shown).x;
		ImGui::SetCursorPosX(startX + std::max(0.0f, colW - 12.0f - nameW));
		ImGui::TextUnformatted(shown);
	}
	if (overrideKind)
		ImGui::PopStyleColor();
	if (ImGui::IsItemHovered() && overrideKind)
		ImGui::SetTooltip("%s (%s override)%s%s", shown,
			overrideKind == 1 ? "local" : "wide",
			hint ? "\\n" : "", hint ? hint : "");
	else if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	if (!stacked) {
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorPosX(startX + colW);
	}
}

// Component header with the removal action next to the component name.
// Returning true allows the caller to run its existing undoable Remove verb
// and return before accessing the removed component row.
inline bool ComponentHeader(const char* title, const char* hint = nullptr) {
	ImGui::Separator();
	ImGui::TextUnformatted(title);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	const float buttonW = ImGui::GetFrameHeight();
	ImGui::SameLine();
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
		ImGui::GetWindowContentRegionMax().x - buttonW));
	const bool remove = ImGui::SmallButton("X##remove_component");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Remove %s component (undoable)", title);
	return remove;
}

// The unique-but-invisible widget label for an attribute name.
inline std::string Id(const char* name) {
	return std::string("##") + name;
}

// ---- BOOLEAN: one consistent Off/On value selector for every editor flag.
// IDs are scoped to the property name, never the shared On/Off text.
inline bool Check(const char* name, bool* value, const char* hint) {
	LabelCell(name, hint);
	ImGui::PushID(name);
	int selected = *value ? 1 : 0;
	ImGui::SetNextItemWidth(-1.0f);
	const bool edited = ImGui::Combo("##value", &selected, "Off\0On\0");
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	if (edited)
		*value = selected != 0;
	ImGui::PopID();
	return edited;
}

// Existing callers drive the undo command when the selected value changes.
inline bool Bool(const char* name, bool value, const char* hint) {
	return Check(name, &value, hint);
}

// ---- READONLY: a read-only value row (name left, the value right,
// NORMAL text color — the replacement for the old dim TextDisabled
// readout/prose lines: readouts that are properties render as
// properties).
inline void Readonly(const char* name, const char* value) {
	LabelCell(name, nullptr);
	ImGui::TextUnformatted(value ? value : "");
}

// ---- FLOAT: the drag (label column + the field) ---------------------------
inline bool Float(const char* name, float* v, float speed, float mn,
	float mx, const char* fmt, const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f); // the rest of the row - no dead gap
	float candidate = numeric::DisplayFixed(*v, 3, mn < mx ? mn : 0.0f);
	const bool edited = ImGui::DragFloat(Id(name).c_str(), &candidate, speed, mn,
		mx, fmt ? fmt : "%.3f");
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	if (!edited || !numeric::Finite(candidate))
		return false;
	*v = candidate;
	return true;
}

// ---- SLIDER float (bounded look knobs) ------------------------------------
inline bool Slider(const char* name, float* v, float mn, float mx,
	const char* fmt, const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	float candidate = numeric::DisplayFixed(*v, 2, mn);
	const bool edited = ImGui::SliderFloat(Id(name).c_str(), &candidate, mn, mx,
		fmt ? fmt : "%.2f");
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	if (!edited || !numeric::Finite(candidate))
		return false;
	*v = candidate;
	return true;
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

// ---- VEC3: three compact, aligned property rows. Every axis uses the
// same label/value columns as Name, floats, and inspector flags.
// Aggregate per-axis drag lifecycle events for the existing undo contract.
struct Vec3Result {
	bool edited = false;
	bool activated = false;
	bool finished = false;
};
inline Vec3Result Vec3(const char* name, float v[3], float speed,
	const char* hint) {
	Vec3Result r;
	ImGui::PushID(name);
	static const char* kAxes[3] = { "X", "Y", "Z" };
	for (int i = 0; i < 3; ++i) {
		ImGui::PushID(i);
		const std::string label = std::string(name) + " " + kAxes[i];
		LabelCell(label.c_str(), hint);
		ImGui::SetNextItemWidth(-1.0f);
		float candidate = numeric::DisplayFixed(v[i]);
		if (ImGui::DragFloat("##axis", &candidate, speed, 0.0f, 0.0f,
				"%.3f") && numeric::Finite(candidate)) {
			v[i] = candidate;
			r.edited = true;
		}
		if (hint && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", hint);
		if (ImGui::IsItemActivated())
			r.activated = true;
		if (ImGui::IsItemDeactivatedAfterEdit())
			r.finished = true;
		ImGui::PopID();
	}
	ImGui::PopID();
	return r;
}

// ---- VEC2 compact (the two-component row) ----------------------------------
inline bool Vec2(const char* name, float v[2], float speed, float mn,
	float mx, const char* fmt, const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	float candidate[2] = { numeric::Display(v[0]), numeric::Display(v[1]) };
	const bool edited = ImGui::DragFloat2(Id(name).c_str(), candidate, speed, mn,
		mx, fmt ? fmt : "%.3f");
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	if (!edited || !numeric::AllFinite(candidate, 2))
		return false;
	v[0] = candidate[0];
	v[1] = candidate[1];
	return true;
}

// ---- INT BOX (typed integer input with steppers — ids/indices where a
// drag's clamps and step would fight typing) -------------------------------
inline bool IntBox(const char* name, int* v, const char* hint) {
	LabelCell(name, hint);
	ImGui::PushID(name);
	ImGui::SetNextItemWidth(-1.0f);
	const bool edited = ImGui::InputInt("##v", v);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	ImGui::PopID();
	return edited;
}

// ---- VEC3 compact (the single-row DragFloat3; for wide hosts where
// three sub-fields still read well) ---------------------------------------
inline bool Vec3Compact(const char* name, float v[3], float speed,
	const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	float candidate[3] = { numeric::Display(v[0]), numeric::Display(v[1]),
		numeric::Display(v[2]) };
	const bool edited = ImGui::DragFloat3(Id(name).c_str(), candidate, speed);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	if (!edited || !numeric::AllFinite(candidate, 3))
		return false;
	for (int i = 0; i < 3; ++i)
		v[i] = candidate[i];
	return true;
}

// ---- COLOR -----------------------------------------------------------------
inline bool Color(const char* name, float v[4], const char* hint) {
	LabelCell(name, hint);
	ImGui::SetNextItemWidth(-1.0f);
	float candidate[4] = { numeric::Display(v[0]), numeric::Display(v[1]),
		numeric::Display(v[2]), numeric::Display(v[3], 1.0f) };
	const bool edited = ImGui::ColorEdit4(Id(name).c_str(), candidate);
	if (hint && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
	if (!edited || !numeric::AllFinite(candidate, 4))
		return false;
	for (int i = 0; i < 4; ++i)
		v[i] = candidate[i];
	return true;
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
