// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor Clarisse-style overrides: wide regex rules over node names.
//
// Layering (documented in the Overrides tab + .tour):
//   source asset < wide rules (ordered, later wins) < per-node local edits.
// A rule never touches a node that carries a local override for the same
// attribute (history scan), so locals always win in the editor and at cook
// replay. Rule writes are NOT history entries; deleting/disabling a rule
// restores captured before-values only where the scene still holds the
// rule's value (later locals are never clobbered).

#include "editor_app.h"

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <regex>

#include <imgui.h>

namespace editor {
	namespace {

		bool SameAttr(editor::EditVerb verb, smvk::tour::OverrideAttr attr) {
			using smvk::tour::OverrideAttr;
			switch (verb) {
			case editor::EditVerb::SetVisibility:
				return attr == OverrideAttr::Visibility;
			case editor::EditVerb::AssignMaterial:
				return attr == OverrideAttr::Material;
			case editor::EditVerb::SetNoLightmap:
				return attr == OverrideAttr::NoLightmap;
			case editor::EditVerb::SetLightmap:
				return attr == OverrideAttr::Lightmap;
			case editor::EditVerb::SetDitherCloseup:
				return attr == OverrideAttr::DitherCloseup;
			default:
				return false;
			}
		}

		// Photoshop-style layer eyeball drawn with raw draw-list primitives (the
		// editor ships no icon font): open eye + pupil = rule ON, droopy closed
		// lid + slash = OFF. Returns true on click.
		bool EyeToggle(const char* str_id, bool on) {
			const ImVec2 size(26.0f, 18.0f);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton(str_id, size);
			const bool clicked = ImGui::IsItemClicked();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 c(p.x + size.x * 0.5f, p.y + size.y * 0.5f);
			const float w = size.x * 0.38f;
			const float h = size.y * 0.38f;
			const ImU32 col = on ? IM_COL32(235, 190, 90, 255)
				: IM_COL32(110, 110, 110, 160);
			if (on) {
				dl->AddBezierQuadratic(ImVec2(c.x - w, c.y), ImVec2(c.x, c.y - h - 1.0f),
					ImVec2(c.x + w, c.y), col, 1.6f);
				dl->AddBezierQuadratic(ImVec2(c.x - w, c.y), ImVec2(c.x, c.y + h + 1.0f),
					ImVec2(c.x + w, c.y), col, 1.6f);
				dl->AddCircleFilled(c, 2.8f, col, 10);
			}
			else {
				dl->AddBezierQuadratic(ImVec2(c.x - w, c.y + 2.0f),
					ImVec2(c.x, c.y - h + 3.0f), ImVec2(c.x + w, c.y + 2.0f), col, 1.6f);
				dl->AddLine(ImVec2(p.x + 4.0f, p.y + size.y - 3.0f),
					ImVec2(p.x + size.x - 4.0f, p.y + 3.0f),
					IM_COL32(200, 90, 90, 190), 1.6f);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s",
					on ? "Rule ON — click to disable (matches revert)"
					: "Rule OFF — click to enable (definition kept)");
			return clicked;
		}

	} // namespace

	bool EditorApp::HasLocalOverride(uint32_t node,
		smvk::tour::OverrideAttr attr) const {
		for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i) {
			const EditCmd& c = m_history[i];
			if (c.node == node && SameAttr(c.verb, attr))
				return true;
		}
		return false;
	}

	std::vector<uint32_t> EditorApp::MatchOverrideNodes(
		const std::string& pattern, std::string& errOut) const {
		std::vector<uint32_t> out;
		errOut.clear();
		if (!m_world || pattern.empty())
			return out;
		std::regex rx;
		try {
			rx.assign(pattern, std::regex::ECMAScript | std::regex::optimize);
		}
		catch (const std::exception& e) {
			errOut = e.what();
			return out;
		}
		const Scene& scene = m_world->scene;
		for (uint32_t n = 0; n < uint32_t(scene.hierarchy.size()); ++n) {
			const std::string nm = getNodeName(scene, int(n));
			if (nm.empty())
				continue;
			try {
				if (std::regex_search(nm, rx))
					out.push_back(n);
			}
			catch (...) {
			}
		}
		return out;
	}

	int EditorApp::CountOverrideMatches(const std::string& pattern) const {
		std::string err;
		bool bad = false;
		std::vector<uint32_t> out;
		if (!m_world || pattern.empty())
			return 0;
		try {
			std::regex rx(pattern,
				std::regex::ECMAScript | std::regex::optimize);
			const Scene& scene = m_world->scene;
			int count = 0;
			for (uint32_t n = 0; n < uint32_t(scene.hierarchy.size()); ++n) {
				const std::string nm = getNodeName(scene, int(n));
				if (!nm.empty() && std::regex_search(nm, rx))
					++count;
			}
			return count;
		}
		catch (...) {
			(void)err;
			(void)bad;
			return -1;
		}
	}

	void EditorApp::ApplyOverrideRule(OverrideRule& r) {
		r.applied.clear();
		r.lastError.clear();
		r.lastMatches = 0;
		if (!m_world || !r.enabled || r.pattern.empty())
			return;
		if (r.attr == smvk::tour::OverrideAttr::Material &&
			(r.materialWorld == UINT32_MAX ||
				r.materialWorld >= m_world->materialLocations.size())) {
			r.lastError = "bad material";
			return;
		}
		std::string err;
		const std::vector<uint32_t> hits = MatchOverrideNodes(r.pattern, err);
		if (!err.empty()) {
			r.lastError = err;
			return;
		}
		Scene& scene = m_world->scene;
		int touched = 0;
		for (uint32_t n : hits) {
			if (HasLocalOverride(n, r.attr))
				continue; // local wins: never touch
			OverrideApplied a{};
			a.node = n;
			switch (r.attr) {
			case smvk::tour::OverrideAttr::Visibility: {
				if (scene.hidden.size() != scene.hierarchy.size())
					scene.hidden.assign(scene.hierarchy.size(), 0);
				a.beforeFlag = scene.hidden[n] != 0;
				if (a.beforeFlag == r.flag)
					break; // already at value: no write, no record
				if (r.flag)
					MarkHidden(scene, int(n));
				else
					ClearHidden(scene, int(n));
				r.applied.push_back(a);
				++touched;
				break;
			}
			case smvk::tour::OverrideAttr::Material: {
				auto it = scene.materialForNode.find(n);
				a.beforeMaterial = (it != scene.materialForNode.end())
					? it->second : UINT32_MAX;
				if (a.beforeMaterial == r.materialWorld)
					break;
				scene.materialForNode[n] = r.materialWorld;
				RefreshNodeDrawState(n);
				r.applied.push_back(a);
				++touched;
				break;
			}
			case smvk::tour::OverrideAttr::NoLightmap: {
				if (scene.noLightmap.size() != scene.hierarchy.size())
					scene.noLightmap.assign(scene.hierarchy.size(), 0);
				a.beforeFlag = scene.noLightmap[n] != 0;
				if (a.beforeFlag == r.flag)
					break;
				if (r.flag)
					MarkNoLightmap(scene, int(n));
				else
					ClearNoLightmap(scene, int(n));
				r.applied.push_back(a);
				++touched;
				break;
			}
			case smvk::tour::OverrideAttr::Lightmap: {
				if (scene.lightmap.size() != scene.hierarchy.size())
					scene.lightmap.assign(scene.hierarchy.size(), 0);
				a.beforeFlag = scene.lightmap[n] != 0;
				if (a.beforeFlag == r.flag)
					break;
				if (r.flag)
					MarkLightmap(scene, int(n));
				else
					ClearLightmap(scene, int(n));
				r.applied.push_back(a);
				++touched;
				break;
			}
			case smvk::tour::OverrideAttr::DitherCloseup: {
				if (scene.ditherCloseup.size() != scene.hierarchy.size())
					scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
				a.beforeFlag = scene.ditherCloseup[n] != 0;
				if (a.beforeFlag == r.flag)
					break;
				if (r.flag)
					MarkDitherCloseup(scene, int(n));
				else
					ClearDitherCloseup(scene, int(n));
				r.applied.push_back(a);
				++touched;
				break;
			}
			}
		}
		r.lastMatches = int(hits.size());
		(void)touched;
	}

	void EditorApp::UnapplyOverrideRule(OverrideRule& r) {
		if (!m_world || r.applied.empty()) {
			r.applied.clear();
			return;
		}
		Scene& scene = m_world->scene;
		for (auto it = r.applied.rbegin(); it != r.applied.rend(); ++it) {
			const uint32_t n = it->node;
			if (n >= scene.hierarchy.size())
				continue;
			if (HasLocalOverride(n, r.attr))
				continue; // a local owns this node now: never clobber
			switch (r.attr) {
			case smvk::tour::OverrideAttr::Visibility: {
				if (scene.hidden.size() != scene.hierarchy.size())
					continue;
				if ((scene.hidden[n] != 0) != r.flag)
					continue; // later rule/local changed it: keep
				if (r.flag)
					ClearHidden(scene, int(n));
				// Restore the captured before (hidden==false default); when
				// before was hidden, re-mark.
				if (it->beforeFlag)
					MarkHidden(scene, int(n));
				break;
			}
			case smvk::tour::OverrideAttr::Material: {
				auto mit = scene.materialForNode.find(n);
				const uint32_t cur = (mit != scene.materialForNode.end())
					? mit->second : UINT32_MAX;
				if (cur != r.materialWorld)
					continue; // overwritten since: keep
				if (it->beforeMaterial == UINT32_MAX)
					scene.materialForNode.erase(n);
				else
					scene.materialForNode[n] = it->beforeMaterial;
				RefreshNodeDrawState(n);
				break;
			}
			case smvk::tour::OverrideAttr::NoLightmap: {
				if (scene.noLightmap.size() != scene.hierarchy.size())
					continue;
				if ((scene.noLightmap[n] != 0) != r.flag)
					continue;
				if (it->beforeFlag)
					MarkNoLightmap(scene, int(n));
				else
					ClearNoLightmap(scene, int(n));
				break;
			}
			case smvk::tour::OverrideAttr::Lightmap: {
				if (scene.lightmap.size() != scene.hierarchy.size())
					continue;
				if ((scene.lightmap[n] != 0) != r.flag)
					continue;
				if (it->beforeFlag)
					MarkLightmap(scene, int(n));
				else
					ClearLightmap(scene, int(n));
				break;
			}
			case smvk::tour::OverrideAttr::DitherCloseup: {
				if (scene.ditherCloseup.size() != scene.hierarchy.size())
					continue;
				if ((scene.ditherCloseup[n] != 0) != r.flag)
					continue;
				if (it->beforeFlag)
					MarkDitherCloseup(scene, int(n));
				else
					ClearDitherCloseup(scene, int(n));
				break;
			}
			}
		}
		r.applied.clear();
		r.lastMatches = 0;
	}

	void EditorApp::ReapplyAllOverrides() {
		for (auto it = m_overrideRules.rbegin(); it != m_overrideRules.rend(); ++it)
			UnapplyOverrideRule(*it);
		for (auto& r : m_overrideRules)
			if (r.enabled)
				ApplyOverrideRule(r);
			else {
				r.applied.clear();
				r.lastMatches = 0;
			}
	}

	bool EditorApp::AddOverrideRule(smvk::tour::OverrideAttr attr,
		const std::string& pattern, bool flag, uint32_t materialWorld) {
		if (!m_world || pattern.empty()) {
			Log("Override: empty pattern (nothing added).");
			return false;
		}
		try {
			std::regex rx(pattern,
				std::regex::ECMAScript | std::regex::optimize);
			(void)rx;
		}
		catch (const std::exception& e) {
			Log(std::string("Override: bad regex '") + pattern + "' (" +
				e.what() + ")");
			return false;
		}
		if (attr == smvk::tour::OverrideAttr::Material &&
			(materialWorld == UINT32_MAX ||
				materialWorld >= m_world->materialLocations.size())) {
			Log("Override: bad material for rule.");
			return false;
		}
		OverrideRule r{};
		r.id = m_nextOverrideId++;
		r.enabled = true;
		r.attr = attr;
		r.pattern = pattern;
		r.flag = flag;
		r.materialWorld = materialWorld;
		m_overrideRules.push_back(std::move(r));
		ApplyOverrideRule(m_overrideRules.back());
		const OverrideRule& back = m_overrideRules.back();
		Log("Override #" + std::to_string(back.id) + " added (" +
			smvk::tour::DescribeOverride(smvk::tour::OverrideRule{
				.attr = back.attr, .pattern = back.pattern, .flag = back.flag,
				.material = 0, .enabled = back.enabled }) +
				", matched " + std::to_string(back.lastMatches) + " nodes, wrote " +
			std::to_string(back.applied.size()) + ").");
		AutosaveTour();
		return true;
	}

	bool EditorApp::DeleteOverrideRule(uint32_t id) {
		for (auto it = m_overrideRules.begin(); it != m_overrideRules.end(); ++it) {
			if (it->id != id)
				continue;
			UnapplyOverrideRule(*it);
			m_overrideRules.erase(it);
			Log("Override #" + std::to_string(id) + " deleted.");
			AutosaveTour();
			return true;
		}
		return false;
	}

	bool EditorApp::SetOverrideEnabled(uint32_t id, bool enabled) {
		for (auto& r : m_overrideRules) {
			if (r.id != id)
				continue;
			if (r.enabled == enabled)
				return true;
			if (enabled) {
				r.enabled = true;
				ApplyOverrideRule(r);
			}
			else {
				UnapplyOverrideRule(r);
				r.enabled = false;
			}
			Log("Override #" + std::to_string(id) +
				(enabled ? " enabled." : " disabled."));
			AutosaveTour();
			return true;
		}
		return false;
	}

	bool EditorApp::SetOverrideFlag(uint32_t id, bool flag) {
		for (auto& r : m_overrideRules) {
			if (r.id != id)
				continue;
			if (r.attr == smvk::tour::OverrideAttr::Material || r.flag == flag)
				return true;
			UnapplyOverrideRule(r);
			r.flag = flag;
			if (r.enabled)
				ApplyOverrideRule(r);
			Log("Override #" + std::to_string(id) + " value -> " +
				(flag ? "on" : "off") + ".");
			AutosaveTour();
			return true;
		}
		return false;
	}

	bool EditorApp::MoveOverrideRule(uint32_t id, int dir) {
		if (dir == 0 || m_overrideRules.size() < 2)
			return false;
		for (size_t i = 0; i < m_overrideRules.size(); ++i) {
			if (m_overrideRules[i].id != id)
				continue;
			const int j = int(i) + (dir < 0 ? -1 : 1);
			if (j < 0 || j >= int(m_overrideRules.size()))
				return false;
			// Order matters (later wins): reapply the whole stack so scene
			// values follow the new precedence.
			std::swap(m_overrideRules[i], m_overrideRules[size_t(j)]);
			ReapplyAllOverrides();
			Log("Override #" + std::to_string(id) + " moved.");
			AutosaveTour();
			return true;
		}
		return false;
	}

	bool EditorApp::RetargetOverrideRule(uint32_t id,
		const std::string& pattern) {
		if (pattern.empty())
			return false;
		try {
			std::regex rx(pattern,
				std::regex::ECMAScript | std::regex::optimize);
			(void)rx;
		}
		catch (...) {
			return false;
		}
		for (auto& r : m_overrideRules) {
			if (r.id != id)
				continue;
			UnapplyOverrideRule(r);
			r.pattern = pattern;
			r.lastError.clear();
			if (r.enabled)
				ApplyOverrideRule(r);
			AutosaveTour();
			return true;
		}
		return false;
	}

	void EditorApp::ClearOverrideRules() {
		for (auto it = m_overrideRules.rbegin(); it != m_overrideRules.rend(); ++it)
			UnapplyOverrideRule(*it);
		m_overrideRules.clear();
		m_nextOverrideId = 1;
		m_overridePatternBuf[0] = '\0';
	}

	void EditorApp::SaveOverridesToDoc(smvk::tour::Doc& doc) const {
		for (const OverrideRule& r : m_overrideRules) {
			smvk::tour::OverrideRule tr{};
			tr.attr = r.attr;
			tr.pattern = r.pattern;
			tr.enabled = r.enabled;
			if (r.attr == smvk::tour::OverrideAttr::Material) {
				const int pool = WorldMatToCluster(r.materialWorld);
				if (pool < 0)
					continue; // stale material: skip, keep the tour loadable
				tr.material = uint32_t(pool);
			}
			else {
				tr.flag = r.flag;
			}
			doc.overrides.push_back(std::move(tr));
		}
	}

	void EditorApp::LoadOverridesFromDoc(const smvk::tour::Doc& doc) {
		m_overrideRules.clear();
		m_nextOverrideId = 1;
		for (const smvk::tour::OverrideRule& tr : doc.overrides) {
			OverrideRule r{};
			r.id = m_nextOverrideId++;
			r.enabled = tr.enabled;
			r.attr = tr.attr;
			r.pattern = tr.pattern;
			if (tr.attr == smvk::tour::OverrideAttr::Material) {
				const int wm = ClusterMatToWorld(tr.material);
				if (wm < 0) {
					Log("tour: override material out of range: " +
						std::to_string(tr.material) + " (rule skipped)");
					--m_nextOverrideId;
					continue;
				}
				r.materialWorld = uint32_t(wm);
			}
			else {
				r.flag = tr.flag;
			}
			m_overrideRules.push_back(std::move(r));
		}
		for (auto& r : m_overrideRules)
			if (r.enabled)
				ApplyOverrideRule(r);
		if (!m_overrideRules.empty())
			Log("tour: applied " + std::to_string(m_overrideRules.size()) +
				" wide override rules.");
	}

	void EditorApp::NoteLocalOverrideWritten(uint32_t node,
		smvk::tour::OverrideAttr attr) {
		for (auto& r : m_overrideRules) {
			if (r.attr != attr || r.applied.empty())
				continue;
			r.applied.erase(std::remove_if(r.applied.begin(), r.applied.end(),
				[node](const OverrideApplied& a) { return a.node == node; }),
				r.applied.end());
		}
	}
	void EditorApp::ReassertWideOverrides(uint32_t node) {
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		const std::string nm =
			getNodeName(m_world->scene, int(node));
		if (nm.empty())
			return;
		for (auto& r : m_overrideRules) {
			if (!r.enabled || r.pattern.empty())
				continue;
			if (HasLocalOverride(node, r.attr))
				continue; // a local still owns it
			bool already = false;
			for (const auto& a : r.applied)
				if (a.node == node) {
					already = true;
					break;
				}
			if (already)
				continue;
			bool hit = false;
			try {
				std::regex rx(r.pattern,
					std::regex::ECMAScript | std::regex::optimize);
				hit = std::regex_search(nm, rx);
			}
			catch (...) {
				continue;
			}
			if (!hit)
				continue;
			// Capture before + write (same guards as ApplyOverrideRule).
			Scene& scene = m_world->scene;
			OverrideApplied a{};
			a.node = node;
			switch (r.attr) {
			case smvk::tour::OverrideAttr::Visibility: {
				if (scene.hidden.size() != scene.hierarchy.size())
					scene.hidden.assign(scene.hierarchy.size(), 0);
				a.beforeFlag = scene.hidden[node] != 0;
				if (a.beforeFlag == r.flag)
					break;
				if (r.flag)
					MarkHidden(scene, int(node));
				else
					ClearHidden(scene, int(node));
				r.applied.push_back(a);
				break;
			}
			case smvk::tour::OverrideAttr::Material: {
				if (r.materialWorld == UINT32_MAX ||
					r.materialWorld >= m_world->materialLocations.size())
					break;
				auto it = scene.materialForNode.find(node);
				a.beforeMaterial = (it != scene.materialForNode.end())
					? it->second : UINT32_MAX;
				if (a.beforeMaterial == r.materialWorld)
					break;
				scene.materialForNode[node] = r.materialWorld;
				RefreshNodeDrawState(node);
				r.applied.push_back(a);
				break;
			}
			case smvk::tour::OverrideAttr::NoLightmap: {
				if (scene.noLightmap.size() != scene.hierarchy.size())
					scene.noLightmap.assign(scene.hierarchy.size(), 0);
				a.beforeFlag = scene.noLightmap[node] != 0;
				if (a.beforeFlag == r.flag)
					break;
				if (r.flag)
					MarkNoLightmap(scene, int(node));
				else
					ClearNoLightmap(scene, int(node));
				r.applied.push_back(a);
				break;
			}
			case smvk::tour::OverrideAttr::Lightmap: {
				if (scene.lightmap.size() != scene.hierarchy.size())
					scene.lightmap.assign(scene.hierarchy.size(), 0);
				a.beforeFlag = scene.lightmap[node] != 0;
				if (a.beforeFlag == r.flag)
					break;
				if (r.flag)
					MarkLightmap(scene, int(node));
				else
					ClearLightmap(scene, int(node));
				r.applied.push_back(a);
				break;
			}
			case smvk::tour::OverrideAttr::DitherCloseup: {
				if (scene.ditherCloseup.size() != scene.hierarchy.size())
					scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
				a.beforeFlag = scene.ditherCloseup[node] != 0;
				if (a.beforeFlag == r.flag)
					break;
				if (r.flag)
					MarkDitherCloseup(scene, int(node));
				else
					ClearDitherCloseup(scene, int(node));
				r.applied.push_back(a);
				break;
			}
			}
		}
	}

	bool EditorApp::ClearLocalOverrides(uint32_t node,
		smvk::tour::OverrideAttr attr) {
		std::vector<size_t> idx;
		for (size_t i = 0; i < m_historyCursor && i < m_history.size(); ++i)
			if (m_history[i].node == node && SameAttr(m_history[i].verb, attr))
				idx.push_back(i);
		if (idx.empty()) {
			Log("Override: node " + std::to_string(node) + " has no local " +
				smvk::tour::OverrideAttrName(attr) + " override.");
			return false;
		}
		// Undo chained entries newest-first: the earliest before-state is the
		// source value, so the node lands back on inherited (or rule) state.
		for (auto it = idx.rbegin(); it != idx.rend(); ++it)
			ExecCmd(m_history[*it], false);
		std::sort(idx.begin(), idx.end(), std::greater<size_t>());
		for (size_t i : idx)
			m_history.erase(m_history.begin() + ptrdiff_t(i));
		m_historyCursor -= idx.size();
		ReassertWideOverrides(node); // a matching wide rule resumes, if any
		AutosaveTour();
		Log("Override: cleared " + std::to_string(idx.size()) + " local " +
			smvk::tour::OverrideAttrName(attr) + " command(s) on node " +
			std::to_string(node) + " (now inherited).");
		return true;
	}

	// Last enabled rule of an attr matching a node name (later wins). Null
	// when nothing wide applies — the inspector uses this to explain the
	// evaluated value (source vs wide vs local).
	const EditorApp::OverrideRule* EditorApp::LastWideRuleFor(
		smvk::tour::OverrideAttr attr, const std::string& nodeName) const {
		const OverrideRule* hit = nullptr;
		if (nodeName.empty())
			return hit;
		for (const auto& r : m_overrideRules) {
			if (!r.enabled || r.attr != attr || r.pattern.empty())
				continue;
			try {
				std::regex rx(r.pattern,
					std::regex::ECMAScript | std::regex::optimize);
				if (std::regex_search(nodeName, rx))
					hit = &r;
			}
			catch (...) {
			}
		}
		return hit;
	}

	void EditorApp::DrawInspectorOverrides(uint32_t node) {
		using smvk::tour::OverrideAttr;
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		const std::string nm = getNodeName(scene, int(node));
		ImGui::Separator();
		ImGui::Text("Overrides (Clarisse-style)");
		ImGui::TextDisabled("source < wide regex < local (more specific wins)");
		// ---- VISIBILITY: the attribute is VISIBILITY with two states
		// (on = visible, off = hidden) — a FLAG CHIP like the material's
		// DS/Shadow row, never a negation-named checkbox ("Hidden" named
		// the negation of the attribute; on/off reads cleaner). The
		// effective state + the provenance ride the text below; the
		// semantics (hierarchical effective visibility) on hover.
		{
			const bool local =
				HasLocalOverride(node, OverrideAttr::Visibility);
			const OverrideRule* wide =
				LastWideRuleFor(OverrideAttr::Visibility, nm);
			const bool visible = !IsHiddenNode(scene, int(node));
			{
				if (visible) {
					ImGui::PushStyleColor(ImGuiCol_Button,
						ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
						ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive,
						ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				}
				const bool clicked =
					ImGui::Button("Visibility", ImVec2(0.0f, 0.0f));
				if (visible)
					ImGui::PopStyleColor(3);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"VISIBILITY — on: this node renders; off: hidden.\n"
						"Effective visibility is hierarchical: a hidden\n"
						"ancestor hides this node too. Overrides stack:\n"
						"local beats wide (regex) beats inherited.");
				if (clicked) {
					// The command's flag is the HIDDEN state (the verb's
					// legacy semantics) — the chip is visibility, so the
					// write inverts: toggling OFF (hide) sets the flag.
					EditCmd c{};
					c.verb = EditVerb::SetVisibility;
					c.node = node;
					c.afterFlag = visible ? 1 : 0; // was hidden?
					c.beforeFlag = visible ? 0 : 1;
					Do(c);
				}
			}
			ImGui::SameLine();
			if (local)
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "[local]");
			else if (wide)
				ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f),
					"[wide #%u]", wide->id);
			else
				ImGui::TextDisabled("[inherited]");
			if (local) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear##vis"))
					ClearLocalOverrides(node, OverrideAttr::Visibility);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Drop the local visibility override (revert to inherited).");
			}
			// The effective state as the TEXT BELOW the attribute.
			const bool eff = IsVisibleNode(scene, int(node));
			ImGui::TextDisabled("effective: %s%s",
				eff ? "on (visible)" : "off (hidden)",
				!eff && !IsHiddenNode(scene, int(node))
					? " — ancestor hidden" : "");
		}
		// ---- per-attr status rows (edits live in their legacy controls and
		// in the Overrides tab; this section explains the evaluated value) ----
		auto statusRow = [&](const char* label, OverrideAttr attr,
			const char* effText) {
			const bool local = HasLocalOverride(node, attr);
			const OverrideRule* wide = LastWideRuleFor(attr, nm);
			ImGui::Text("%s: %s", label, effText);
			ImGui::SameLine();
			if (local)
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "[local]");
			else if (wide)
				ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f),
					"[wide #%u]", wide->id);
			else
				ImGui::TextDisabled("[inherited]");
			if (local) {
				ImGui::SameLine();
				char btn[64];
				std::snprintf(btn, sizeof(btn), "Clear##%s", label);
				if (ImGui::SmallButton(btn))
					ClearLocalOverrides(node, attr);
			}
		};
		{
			char buf[64];
			auto mit = scene.materialForNode.find(node);
			if (mit != scene.materialForNode.end())
				std::snprintf(buf, sizeof(buf), "mat.%u", mit->second);
			else
				std::snprintf(buf, sizeof(buf), "(none)");
			statusRow("Material", OverrideAttr::Material, buf);
		}
		{
			const bool eff = IsNoLightmapNode(scene, int(node));
			statusRow("Lightmap", OverrideAttr::NoLightmap,
				eff ? "excluded" : "baked");
		}
		{
			const bool eff = IsDitherCloseupNode(scene, int(node));
			statusRow("Dither", OverrideAttr::DitherCloseup,
				eff ? "on" : "off");
		}
		// ---- which wide rules touch this node (evaluated-state debugging) ----
		{
			bool any = false;
			for (const auto& r : m_overrideRules) {
				if (!r.enabled || r.pattern.empty() || nm.empty())
					continue;
				bool hit = false;
				try {
					std::regex rx(r.pattern,
						std::regex::ECMAScript | std::regex::optimize);
					hit = std::regex_search(nm, rx);
				}
				catch (...) {
				}
				if (!hit)
					continue;
				if (!any) {
					ImGui::TextDisabled("wide rules matching this node:");
					any = true;
				}
				if (r.attr == OverrideAttr::Material)
					ImGui::BulletText("#%u %s -> mat.%u", r.id,
						smvk::tour::OverrideAttrName(r.attr), r.materialWorld);
				else
					ImGui::BulletText("#%u %s = %d", r.id,
						smvk::tour::OverrideAttrName(r.attr), r.flag ? 1 : 0);
			}
		}
	}

	void EditorApp::DrawOverridesPanel() {
		using smvk::tour::OverrideAttr;
		if (!m_world) {
			return;
		}
		// ---- add form: one compact row ----
		ImGui::PushID("ovrAdd");
		ImGui::SetNextItemWidth(240.0f);
		ImGui::InputTextWithHint("##pattern", "regex (e.g. Building_.*)",
			m_overridePatternBuf, sizeof(m_overridePatternBuf));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"ECMAScript regex over node names (regex_search). One rule "
				"matches many nodes — the point of wide overrides.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(130.0f);
		const char* kAttrs = "Visibility\0Material\0NoLightmap\0DitherCloseup\0";
		ImGui::Combo("##attr", &m_overrideAttrSel, kAttrs);
		if (m_overrideAttrSel == 1) {
			// Material value picker (world slots, like the Inspector combo).
			ImGui::SameLine();
			ImGui::SetNextItemWidth(160.0f);
			std::string preview = "mat." + std::to_string(m_overrideMatSel);
			if (ImGui::BeginCombo("##mat", preview.c_str())) {
				const uint32_t count =
					uint32_t(m_world->materialLocations.size());
				for (uint32_t i = 0; i < count; ++i) {
					const auto& loc = m_world->materialLocations[i];
					std::string uri = loc.cluster
						? std::string(loc.cluster->GetMaterialUri(loc.handle).Str())
						: "";
					char item[320];
					std::snprintf(item, sizeof(item), "%u: %s", i,
						uri.empty() ? "(unnamed)" : uri.c_str());
					const bool sel = (int(i) == m_overrideMatSel);
					if (ImGui::Selectable(item, sel))
						m_overrideMatSel = int(i);
					if (sel)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}
		else {
			// The bool value as the attribute's OWN two states ("on/off"
			// — never a negation-named flag: visibility rules read
			// "on = visible", stored inverted (the rule flag is the
			// legacy hidden bit) at the Add below.
			const char* flagLabel = "On";
			ImGui::SameLine();
			ImGui::Checkbox(flagLabel, &m_overrideFlagVal);
			if (ImGui::IsItemHovered()) {
				if (m_overrideAttrSel == 0)
					ImGui::SetTooltip("VISIBILITY on: matched nodes "
						"render. Off: matched nodes hide.");
				else if (m_overrideAttrSel == 2)
					ImGui::SetTooltip("NoLightmap on: matched nodes "
						"bake probe/sky-lit (excluded from the atlas).");
				else
					ImGui::SetTooltip("DitherCloseup on: matched nodes "
						"use the closeup dither path.");
			}
		}
		ImGui::SameLine();
		{
			// Live match preview (before local-skip).
			const int n = CountOverrideMatches(m_overridePatternBuf);
			if (m_overridePatternBuf[0] == '\0')
				ImGui::TextDisabled("no pattern");
			else if (n < 0)
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "bad regex");
			else
				ImGui::TextDisabled("%d match(es)", n);
		}
		ImGui::SameLine();
		if (ImGui::Button("Add rule")) {
			const OverrideAttr attr = OverrideAttr(m_overrideAttrSel);
			const uint32_t mat =
				(m_overrideAttrSel == 1) ? uint32_t(m_overrideMatSel) : UINT32_MAX;
			// VISIBILITY inverts here: the form's checkbox is the
			// attribute's OWN state ("On" = visible), the stored rule
			// flag is the legacy hidden bit.
			const bool flag = m_overrideAttrSel == 0
				? !m_overrideFlagVal : m_overrideFlagVal;
			if (AddOverrideRule(attr, m_overridePatternBuf, flag, mat))
				m_overridePatternBuf[0] = '\0';
		}
		ImGui::PopID();

		if (m_overrideRules.empty()) {
			ImGui::Separator();
			ImGui::TextDisabled(
				"No wide rules. Example: pattern \"Tree.*\" + Visibility/Hide "
				"hides every tree in this shot without touching the source.");
			ImGui::TextDisabled(
				"Precedence: source asset < rules top-to-bottom (later wins) < "
				"per-node local edits (Inspector).");
			return;
		}

		// ---- rule table (precedence order: later rows win) ----
		const float footerH = 30.0f;
		const float tableH =
			std::max(ImGui::GetContentRegionAvail().y - footerH, 60.0f);
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
			ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH |
			ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
			ImGuiTableFlags_NoSavedSettings;
		if (ImGui::BeginTable("##ovrTable", 7, flags, ImVec2(0.0f, tableH))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed,
				36.0f);
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed,
				28.0f);
			ImGui::TableSetupColumn("Attribute",
				ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Pattern (regex)",
				ImGuiTableColumnFlags_WidthStretch, -1.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed,
				96.0f);
			ImGui::TableSetupColumn("Match/Wrote",
				ImGuiTableColumnFlags_WidthFixed, 92.0f);
			ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed,
				160.0f);
			ImGui::TableHeadersRow();
			for (size_t i = 0; i < m_overrideRules.size(); ++i) {
				OverrideRule& r = m_overrideRules[i];
				ImGui::PushID(int(r.id));
				ImGui::TableNextRow();
				// Eye: rule on/off (layer toggle).
				ImGui::TableNextColumn();
				if (EyeToggle("##eye", r.enabled))
					SetOverrideEnabled(r.id, !r.enabled);
				// Id.
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%u", r.id);
				// Attribute.
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(smvk::tour::OverrideAttrName(r.attr));
				// Pattern (in-place retarget, Enter to re-apply).
				ImGui::TableNextColumn();
				{
					const bool bad = !r.lastError.empty();
					if (bad)
						ImGui::PushStyleColor(ImGuiCol_Text,
							ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
					char patBuf[256];
					std::snprintf(patBuf, sizeof(patBuf), "%s", r.pattern.c_str());
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::InputText("##pat", patBuf, sizeof(patBuf),
						ImGuiInputTextFlags_EnterReturnsTrue))
						RetargetOverrideRule(r.id, patBuf);
					if (bad) {
						ImGui::PopStyleColor();
						ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
							r.lastError.c_str());
					}
					else if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Edit regex, Enter to re-apply.");
				}
				// Value (bool attrs flip in place; material is fixed).
				ImGui::TableNextColumn();
				if (r.attr == OverrideAttr::Material) {
					ImGui::Text("mat.%u", r.materialWorld);
					if (ImGui::IsItemHovered()) {
						const auto& loc =
							r.materialWorld < m_world->materialLocations.size()
							? m_world->materialLocations[r.materialWorld]
							: smvk::World::MaterialLocation{};
						if (loc.cluster)
							ImGui::SetTooltip("%s",
								loc.cluster->GetMaterialUri(loc.handle).Str());
					}
				}
				else {
					// The rule's VALUE cell: the attribute's own two
					// states ("on"/"off"), never the negation-named
					// pair — a visibility rule reads "on = visible"
					// (the stored flag is the legacy hidden bit, the
					// display inverts).
					const char* lbl = r.flag ? "off" : "on";
					if (ImGui::SmallButton(lbl))
						SetOverrideFlag(r.id, !r.flag);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Click to flip the rule's %s "
							"value (on = %s).",
							smvk::tour::OverrideAttrName(r.attr),
							r.attr == OverrideAttr::Visibility
							? "visible" : "active");
				}
				// Matched / wrote.
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%d / %zu", r.lastMatches, r.applied.size());
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"%d node(s) matched the regex; %zu were written (nodes "
						"with a local override are skipped).",
						r.lastMatches, r.applied.size());
				// Order + delete.
				ImGui::TableNextColumn();
				{
					if (i > 0 && ImGui::SmallButton("^ Up"))
						MoveOverrideRule(r.id, -1);
					ImGui::SameLine();
					if (i + 1 < m_overrideRules.size() && ImGui::SmallButton("v Dn"))
						MoveOverrideRule(r.id, +1);
					ImGui::SameLine();
					if (ImGui::SmallButton("Del"))
						DeleteOverrideRule(r.id);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		// Footer.
		if (ImGui::SmallButton("Reapply all"))
			ReapplyAllOverrides();
		ImGui::SameLine();
		ImGui::TextDisabled(
			"%zu rule(s) — later rows win over earlier ones; local edits "
			"(Inspector) win over rules; deleting restores captured values.",
			m_overrideRules.size());
	}

} // namespace editor
