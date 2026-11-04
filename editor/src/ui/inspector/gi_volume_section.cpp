// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "editor_brush_presets.h"
#include "modal_bindings.h"
#include "editor_attr_ui.h" // the attribute-widget stubs (attr::Bool/...)
#include "editor_asset_browser_nav.h" // collection folders belong at root
#include "editor_asset_inspect.h" // generic file/directory inspector
#include "editor_asset_creation.h"
#include "editor_asset_dcc.h" // DCC source link + export build stage

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Asset/shape_asset.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Profile/profile.h>

#include <Engine/Asset/spriteset.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder: initial workspace graph + user reset
#include <SDL3/SDL_misc.h> // SDL_OpenURL: launch the local asset history UI
#include <SDL3/SDL_filesystem.h> // SDL_GetBasePath (staged export scripts)
#include <imgui_impl_vulkan.h> // ImGui_ImplVulkan_AddTexture (TextureThumb)
#include <stb_image.h>         // stbi_load (AssetSidecarThumb; impl in editor_branding)

#include <ktx.h>

#include <cmath>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h> // ShellExecuteA (reveal-in-explorer)
#endif



namespace editor {
	void EditorApp::DrawGiVolumeSection(uint32_t node) {
		const attr::ScopedID componentId("gi_volume");
		// Authored GI volume on this node (editor GI volume node):
		// box shape from the node's TRS (translate = center, scale = full
		// extents), bake params below. Undoable SetGiVolumeAttr edits persisted
		// as setGiVolumeAttr tour verbs so cooks replay them.
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		auto it = FindGiVolume(scene, node);
		if (!it)
			return;
		::SceneGiVolume& def = it->volume;
		ImGui::Separator();
		ImGui::Text("Convex volume: '%s'",
			getNodeName(scene, int(node)).c_str());
		glm::vec3 mn, mx;
		if (GiVolumeBox(node, mn, mx))
			ImGui::TextDisabled("aabb (%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f) m",
				mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
		ImGui::TextDisabled("shape = node translate/scale/rotation (gizmo "
			"or transform; a rotated box — the bake rotates the planes)");
		auto beginGiVolumeDrag = [&](const char* attr) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetGiVolumeAttr ||
				m_pending.cmd.node != node || m_pending.cmd.giVolumeAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetGiVolumeAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.giVolumeAttr = attr;
				ReadNodeGiVolumeAttr(node, attr, m_pending.cmd.beforeVec4,
					m_pending.cmd.beforeNum, m_pending.cmd.beforeText);
			}
		};
		auto endGiVolumeDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.verb == EditVerb::SetGiVolumeAttr &&
				m_pending.cmd.node == node &&
				m_pending.cmd.giVolumeAttr == attr)
				FinishDrag();
		};
		int prio = int(def.priority);
		if (attr::Int("Priority", &prio, 0, 255,
				"Higher wins (the FOX override rule: later entries "
				"composite over earlier)")) {
			beginGiVolumeDrag("priority");
			WriteNodeGiVolumeAttr(node, "priority", glm::vec4(0), double(prio), {});
			m_pending.cmd.number = double(prio);
		}
		endGiVolumeDrag("priority");
		float spacing = def.spacing;
		if (attr::Float("Probe spacing", &spacing, 0.01f, 0.05f, 4.0f, "%.2f",
				"The PREFERRED density (m): the bake uses the seeded "
				"layout below, else the gate search, else this fixed "
				"spacing")) {
			beginGiVolumeDrag("spacing");
			WriteNodeGiVolumeAttr(node, "spacing", glm::vec4(0), double(spacing),
				{});
			m_pending.cmd.number = double(spacing);
		}
		endGiVolumeDrag("spacing");
		// ---- Density gates (the optimizer's search bounds; 0 = off,
		// the search then defaults to +-20%% around the spacing above).
		float low = def.spacingLow;
		if (attr::Float("Density gate low", &low, 0.01f, 0.0f, 4.0f, "%.2f",
				"The optimizer's lower spacing bound (m; 0 = off)")) {
			low = std::clamp(low, 0.0f, def.spacingHigh > 0.0f
				? def.spacingHigh : 4.0f);
			beginGiVolumeDrag("spacinglow");
			WriteNodeGiVolumeAttr(node, "spacinglow", glm::vec4(0),
				double(low), {});
			m_pending.cmd.number = double(low);
		}
		endGiVolumeDrag("spacinglow");
		float high = def.spacingHigh;
		if (attr::Float("Density gate high", &high, 0.01f, 0.0f, 4.0f, "%.2f",
				"The optimizer's upper spacing bound (m; 0 = off)")) {
			high = std::clamp(high, std::max(def.spacingLow, 0.01f), 4.0f);
			beginGiVolumeDrag("spacinghigh");
			WriteNodeGiVolumeAttr(node, "spacinghigh", glm::vec4(0),
				double(high), {});
			m_pending.cmd.number = double(high);
		}
		endGiVolumeDrag("spacinghigh");
		float inside = def.insideFade;
		if (attr::Float("Inside fade", &inside, 0.01f, 0.0f, 4.0f, "%.2f",
				"The interior weight fade band (m)")) {
			beginGiVolumeDrag("inside");
			WriteNodeGiVolumeAttr(node, "inside", glm::vec4(0), double(inside),
				{});
			m_pending.cmd.number = double(inside);
		}
		endGiVolumeDrag("inside");
		float outside = def.outsideFade;
		if (attr::Float("Outside fade", &outside, 0.01f, 0.0f, 4.0f, "%.2f",
				"The exterior falloff band beyond the volume (m)")) {
			beginGiVolumeDrag("outside");
			WriteNodeGiVolumeAttr(node, "outside", glm::vec4(0), double(outside),
				{});
			m_pending.cmd.number = double(outside);
		}
		endGiVolumeDrag("outside");
		// ---- Automatic grid layout: the optimizer's seeded result
		// (what the bake will build) + the inspector button.
		ImGui::SeparatorText("Grid layout");
		if (def.optSpacing > 0.0f) {
			char buf[160];
			std::snprintf(buf, sizeof(buf), "spacing %.3f m, phase "
				"(%.2f, %.2f, %.2f) - rebakes use this verbatim",
				def.optSpacing, def.optPhase[0], def.optPhase[1],
				def.optPhase[2]);
			attr::Readonly("Seeded layout", buf);
			if (m_giLayoutLast.valid && m_giLayoutLast.node == node) {
				const auto& r = m_giLayoutLast.result;
				std::snprintf(buf, sizeof(buf), "probes %llu, invalid %llu "
					"(inside-solid %llu), coverage %.1f%%, score %.3f",
					(unsigned long long)r.probes,
					(unsigned long long)r.invalid,
					(unsigned long long)r.insideSolid,
					100.0 * r.coverage, r.score);
				attr::Readonly("Last search", buf);
			}
			if (ImGui::Button("Clear seeded layout")) {
				// Undoable clear (spacing 0 = search/legacy takes over).
				beginGiVolumeDrag("optlayout");
				WriteNodeGiVolumeAttr(node, "optlayout", glm::vec4(0), 0.0,
					{});
				m_pending.cmd.number = 0.0;
				FinalizePending();
			}
		}
		else {
			attr::Readonly("Seeded layout", "none - the bake runs the "
				"gate search (or the fixed spacing when the gates are "
				"off)");
		}
		if (m_giLayoutRunning) {
			ImGui::BeginDisabled();
			ImGui::Button("Optimizing...");
			ImGui::EndDisabled();
		}
		else if (ImGui::Button("Optimize grid layout")) {
			RunGiLayoutOptimizer(node);
		}
		if (ImGui::Button("Remove GI Volume component")) {
			EditCmd c{};
			c.verb = EditVerb::RemoveGiVolumeVerb;
			c.node = node;
			Do(c);
		}
	}

} // namespace editor
