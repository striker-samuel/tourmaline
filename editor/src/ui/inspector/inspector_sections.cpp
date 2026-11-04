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

	void EditorApp::DrawPhysicsBodySection(uint32_t node) {
		const attr::ScopedID componentId("physics_body");
		if (!m_world || node >= m_world->scene.hierarchy.size()) return;
		auto* row = tour::scene::FindPhysicsBody(m_world->scene, node);
		ImGui::Separator();
		if (!row)
			return;
		ImGui::TextUnformatted("Physics Body / Collider");
		ImGui::TextDisabled("Authored definition (simulation preview not yet available).");
		auto commit = [&](const char* key, double value, bool flag = false) {
			EditCmd cmd{};
			cmd.verb = EditVerb::SetPhysicsBodyAttr;
			cmd.node = node;
			cmd.physicsAttr = key;
			ReadNodePhysicsBodyAttr(node, key, cmd.beforeNum, cmd.beforeFlag);
			cmd.number = value;
			cmd.afterFlag = flag;
			Do(cmd);
		};
		int motion = int(row->body.motion);
		if (ImGui::Combo("Motion", &motion, "Static\0Dynamic\0Kinematic\0"))
			commit("motion", motion);
		int shape = int(row->body.shape);
		if (ImGui::Combo("Collider shape", &shape, "Box\0Sphere\0Capsule\0"))
			commit("shape", shape);
		auto scalar = [&](const char* label, const char* key, float initial,
				float lo, float hi) {
			float value = initial;
			if (ImGui::DragFloat(label, &value, 0.01f, lo, hi, "%.3f",
					ImGuiSliderFlags_AlwaysClamp))
				commit(key, std::clamp(value, lo, hi));
		};
		if (motion == 1)
			scalar("Mass (kg)", "mass", row->body.mass, 0.001f, 1000000.f);
		scalar("Friction", "friction", row->body.friction, 0.f, 1.f);
		scalar("Restitution", "restitution", row->body.restitution, 0.f, 1.f);
		if (shape == 0) {
			scalar("Half extent X (m)", "halfX", row->body.halfExtents.x, 0.001f, 10000.f);
			scalar("Half extent Y (m)", "halfY", row->body.halfExtents.y, 0.001f, 10000.f);
			scalar("Half extent Z (m)", "halfZ", row->body.halfExtents.z, 0.001f, 10000.f);
		} else {
			scalar("Radius (m)", "radius", row->body.radius, 0.001f, 10000.f);
			if (shape == 2)
				scalar("Cylinder height (m)", "height", row->body.height, 0.001f, 10000.f);
		}
		bool sensor = row->body.sensor;
		if (ImGui::Checkbox("Sensor (no contact response)", &sensor))
			commit("sensor", 0, sensor);
		if (ImGui::Button("Remove Physics Body")) {
			EditCmd cmd{};
			cmd.verb = EditVerb::RemovePhysicsBodyVerb;
			cmd.node = node;
			Do(cmd);
		}
	}

	void EditorApp::DrawTriggerVolumeSection(uint32_t node) {
		const attr::ScopedID componentId("trigger_volume");
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		const auto* row = tour::scene::FindTriggerVolume(scene, node);
		ImGui::Separator();
		if (!row)
			return;
		ImGui::TextUnformatted("Trigger component");
		ImGui::TextDisabled("Oriented box = this node's global transform.");
		auto check = [&](const char* label, const char* attr, bool current) {
			bool v = current;
			if (ImGui::Checkbox(label, &v)) {
				EditCmd cmd{};
				cmd.verb = EditVerb::SetTriggerVolumeAttr;
				cmd.node = node;
				cmd.triggerVolumeAttr = attr;
				ReadNodeTriggerVolumeAttr(node, attr, cmd.beforeNum,
					cmd.beforeFlag, cmd.beforeText);
				cmd.afterFlag = v;
				Do(cmd);
			}
		};
		check("Trigger enabled", "enabled", row->trigger.enabled);
		check("Fire only once per actor", "once", row->trigger.once);
		char tag[257];
		std::snprintf(tag, sizeof(tag), "%s", row->trigger.tag.c_str());
		if (ImGui::InputText("Event tag", tag, sizeof(tag),
				ImGuiInputTextFlags_EnterReturnsTrue)) {
			EditCmd cmd{};
			cmd.verb = EditVerb::SetTriggerVolumeAttr;
			cmd.node = node;
			cmd.triggerVolumeAttr = "tag";
			ReadNodeTriggerVolumeAttr(node, "tag", cmd.beforeNum,
				cmd.beforeFlag, cmd.beforeText);
			cmd.afterText = tag;
			Do(cmd);
		}
		if (ImGui::Button("Remove Trigger component")) {
			EditCmd cmd{};
			cmd.verb = EditVerb::RemoveTriggerVolumeVerb;
			cmd.node = node;
			Do(cmd);
		}
	}

	void EditorApp::DrawAudioRoomSection(uint32_t node) {
		const attr::ScopedID componentId("audio_room");
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	if (node >= scene.hierarchy.size())
		return;
	const tour::scene::AudioRoomRow* row =
		tour::scene::FindAudioRoom(scene, node);
	ImGui::Separator();
	if (!row)
		return;
	const tour::scene::SceneAudioRoom& def = row->room;
	ImGui::Text("Audio room: '%s'",
		getNodeName(scene, int(node)).c_str());
	ImGui::TextDisabled("shape = node translate/scale/rotation"
		" (gizmo); a GI volume on the same node keeps its own bake");
	// The drag pattern (one coalesced SetAudioRoomAttr per drag).
	auto beginRoomDrag = [&](const char* attr) {
		if (m_pending.active
			&& (m_pending.cmd.verb != EditVerb::SetAudioRoomAttr
				|| m_pending.cmd.node != node
				|| m_pending.cmd.roomAttr != attr))
			FinalizePending();
		if (!m_pending.active) {
			m_pending.active = true;
			m_pending.cmd = EditCmd{};
			m_pending.cmd.verb = EditVerb::SetAudioRoomAttr;
			m_pending.cmd.node = node;
			m_pending.cmd.roomAttr = attr;
			ReadNodeAudioRoomAttr(node, attr,
				m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum,
				m_pending.cmd.beforeFlag, m_pending.cmd.beforeText);
		}
	};
	auto endRoomDrag = [&](const char* attr) {
		if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active
			&& m_pending.cmd.verb == EditVerb::SetAudioRoomAttr
			&& m_pending.cmd.node == node
			&& m_pending.cmd.roomAttr == attr)
			FinishDrag();
	};
	{
		float v = def.wet;
		if (attr::Float("Wet", &v, 0.01f, 0.0f, 1.0f, "%.2f",
				"The environmental reverb send gain (0 = dry)")) {
			beginRoomDrag("wet");
			WriteNodeAudioRoomAttr(node, "wet", glm::vec4(0),
				double(v), false, {});
			m_pending.cmd.number = double(v);
		}
		endRoomDrag("wet");
	}
	{
		float v = def.rt60;
		if (attr::Float("RT60 (s)", &v, 0.05f, 0.15f, 8.0f, "%.2f",
				"Seconds until the tail drops 60 dB")) {
			beginRoomDrag("rt60");
			WriteNodeAudioRoomAttr(node, "rt60", glm::vec4(0),
				double(v), false, {});
			m_pending.cmd.number = double(v);
		}
		endRoomDrag("rt60");
	}
	{
		float v = def.damping;
		if (attr::Float("Damping", &v, 0.01f, 0.0f, 1.0f, "%.2f",
				"High-frequency loss in the reverb tail")) {
			beginRoomDrag("damping");
			WriteNodeAudioRoomAttr(node, "damping", glm::vec4(0),
				double(v), false, {});
			m_pending.cmd.number = double(v);
		}
		endRoomDrag("damping");
	}
	{
		int p = int(def.priority);
		if (attr::Int("Priority", &p, 0, 1000000,
				"Higher wins when rooms overlap (audio's own"
				" ordering, independent of GI priority)")) {
			beginRoomDrag("priority");
			WriteNodeAudioRoomAttr(node, "priority", glm::vec4(0),
				double(p), false, {});
			m_pending.cmd.number = double(p);
		}
		endRoomDrag("priority");
	}
	{
		float v = def.insideFade;
		if (attr::Float("Inside fade (m)", &v, 0.05f, 0.0f, 100.0f,
				"%.1f", "Per-face inner transition (metres)")) {
			beginRoomDrag("insideFade");
			WriteNodeAudioRoomAttr(node, "insideFade", glm::vec4(0),
				double(v), false, {});
			m_pending.cmd.number = double(v);
		}
		endRoomDrag("insideFade");
	}
	{
		float v = def.outsideFade;
		if (attr::Float("Outside fade (m)", &v, 0.05f, 0.0f, 100.0f,
				"%.1f", "Per-face exterior influence (metres)")) {
			beginRoomDrag("outsideFade");
			WriteNodeAudioRoomAttr(node, "outsideFade", glm::vec4(0),
				double(v), false, {});
			m_pending.cmd.number = double(v);
		}
		endRoomDrag("outsideFade");
	}
	{
		float v = def.transitionSeconds;
		if (attr::Float("Transition (s)", &v, 0.01f, 0.0f, 10.0f,
				"%.2f", "The listener's room-blend glide time")) {
			beginRoomDrag("transitionSeconds");
			WriteNodeAudioRoomAttr(node, "transitionSeconds",
				glm::vec4(0), double(v), false, {});
			m_pending.cmd.number = double(v);
		}
		endRoomDrag("transitionSeconds");
	}
	// The enabled chip (immediate — one Do per click).
	{
		bool en = def.enabled;
		if (attr::Check("Enabled", &en,
				"A disabled room covers nothing (the shape stays"
				" authored for re-enabling)")) {
			EditCmd c{};
			c.verb = EditVerb::SetAudioRoomAttr;
			c.node = node;
			c.roomAttr = "enabled";
			ReadNodeAudioRoomAttr(node, "enabled", c.beforeVec4,
				c.beforeNum, c.beforeFlag, c.beforeText);
			c.afterFlag = en;
			c.number = c.beforeNum;
			WriteNodeAudioRoomAttr(node, "enabled", glm::vec4(0),
				0.0, en, {});
			Do(c);
		}
	}
	if (ImGui::Button("Remove Audio Room component")) {
		// Drops the ASSOCIATION only — the node + any GI volume on it
		// stay (the RemoveAudioRoomVerb command; undo restores the
		// captured row exactly).
		EditCmd c{};
		c.verb = EditVerb::RemoveAudioRoomVerb;
		c.node = node;
		Do(c);
	}
}
void EditorApp::DrawExportSettingsSection(uint32_t node) {
		const attr::ScopedID componentId("export_settings");
		// The scene's exportSettings node: the WorldSettings-style EXPORT
		// CONFIG page (cook knobs). Every edit is an undoable SetExportAttr
		// command persisted as a setExportAttr tour line; Cook.exe/Pak.exe
		// read the replayed scene (CLI flags still win). outPak/contentDir
		// stay session-only in the Cook panel (never scene state).
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		auto it = FindExport(scene, node);
		if (!it)
			return;
		ImGui::Separator();
		ImGui::Text("Export settings: '%s'",
			getNodeName(scene, int(node)).c_str());
		ImGui::TextDisabled("cook/export knobs (createNode -at "
			"exportSettings + setExportAttr; CLI flags still win)");

		auto beginDrag = [&](const char* attr) {
			if (m_pending.active
				&& (m_pending.cmd.verb != EditVerb::SetExportAttr
					|| m_pending.cmd.node != node
					|| m_pending.cmd.exportAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetExportAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.exportAttr = attr;
				ReadNodeExportAttr(node, attr, m_pending.cmd.beforeNum,
					m_pending.cmd.beforeFlag, m_pending.cmd.beforeText);
			}
		};
		auto endDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active
				&& m_pending.cmd.verb == EditVerb::SetExportAttr
				&& m_pending.cmd.node == node
				&& m_pending.cmd.exportAttr == attr)
				FinishDrag();
		};
		// The row layer: every export row rides the attr stubs (name
		// left, value right - the panel-wide convention). endDrag's
		// IsItemDeactivatedAfterEdit still sees the drag as the last
		// item (the stubs add nothing after the widget).
		auto setBool = [&](const char* label, const char* attr, bool v,
			const char* tip) {
			bool val = v;
			if (attr::Check(label, &val, tip))
				PushExportAttr(node, attr, 0.0, val, "");
		};
		auto dragInt = [&](const char* label, const char* attr, int v,
			float speed, int lo, int hi) {
			(void)speed; // the stub's drag step is 1 (every caller's)
			int val = v;
			if (attr::Int(label, &val, lo, hi, nullptr)) {
				val = std::clamp(val, lo, hi);
				beginDrag(attr);
				WriteNodeExportAttr(node, attr, double(val), false, {});
				m_pending.cmd.number = double(val);
			}
			endDrag(attr);
		};
		auto dragFloat = [&](const char* label, const char* attr, float v,
			float speed, float lo, float hi, const char* fmt) {
			float val = v;
			if (attr::Float(label, &val, speed, lo, hi, fmt, nullptr)) {
				val = std::clamp(val, lo, hi);
				beginDrag(attr);
				WriteNodeExportAttr(node, attr, double(val), false, {});
				m_pending.cmd.number = double(val);
			}
			endDrag(attr);
		};

		const SceneExportSettings& es = it->settings;
		setBool("MeshOpt (meshoptimizer)", "meshopt", es.meshopt,
			"Reorder vertices/indices for GPU cache efficiency.");
		setBool("Generate LODs", "lods", es.lods,
			"Build mesh LOD chains via meshoptimizer.");
		if (es.lods) {
			ImGui::Indent();
			char ratios[64];
			std::snprintf(ratios, sizeof(ratios), "%s",
				es.lodRatios.c_str());
			if (attr::Text("LOD ratios", ratios, sizeof(ratios),
					"The LOD chain's simplify ratios (csv, e.g. "
					"\"0.5,0.25\")"))
				PushExportAttr(node, "lodRatios", 0.0, false, ratios);
			dragFloat("LOD error", "lodError", es.lodError, 0.001f,
				0.0f, 1.0f, "%.3f");
			ImGui::Unindent();
		}
		setBool("Meshlets", "meshlets", es.meshlets,
			"Per-meshlet build over the cooked topology.");
		if (es.meshlets) {
			ImGui::Indent();
			dragInt("Max verts", "meshletMaxVerts",
				int(es.meshletMaxVerts), 1.0f, 3, 255);
			dragInt("Max tris", "meshletMaxTris",
				int(es.meshletMaxTris), 1.0f, 1, 512);
			dragFloat("Cone weight", "meshletConeWeight",
				es.meshletConeWeight, 0.01f, 0.0f, 1.0f, "%.2f");
			ImGui::Unindent();
		}
		setBool("Auto-convert textures (KTX2)", "autoKtx", es.autoKtx,
			"Non-KTX sources transcode in-process at cook.");
		setBool("Skip loose VFS bake files (ignoreVfs)", "ignoreVfs",
			es.ignoreVfs,
			"Embed ONLY what the scene itself carries: no loose\n"
			"gi/<stem>.sggi, no givolumemanifest/.givolume rows, no cubemap\n"
			"manifest/KTX rows at pak assembly (minimal variant pak).");

		setBool("Lightmaps (bake)", "lightmap", es.lightmap,
			"On-demand lightmap bake (Bake GI).");
		if (es.lightmap) {
			ImGui::Indent();
			dragInt("Voxel proxy resolution", "lmProxyRes",
				es.lmProxyRes, 1.0f, 0, 256);
			dragFloat("Meters per texel", "lmMetersPerTexel",
				es.lmMetersPerTexel, 0.01f, 0.01f, 1.0f, "%.3f");
			dragInt("Max SG lobes", "lmLobes", int(es.lmLobes), 1.0f, 5, 12);
			dragInt("Atlas width (px)", "lmAtlasWidth",
				int(es.lmAtlasWidth), 64.0f, 256, 8192);
			dragInt("Rays per texel", "lmRays", int(es.lmRays), 8.0f,
				64, 8192);
			ImGui::Unindent();
		}

		setBool("Cubemap captures", "cubemap", es.cubemap,
			"On-demand preconvolved specular capture set (Update cubemaps).");
		if (es.cubemap) {
			ImGui::Indent();
			dragInt("Size", "cubemapSize", int(es.cubemapSize), 32.0f,
				64, 2048);
			dragInt("Rays per texel", "cubemapRays",
				int(es.cubemapRays), 8.0f, 1, 4096);
			setBool("Octree placement (adaptive)", "cubemapOctree",
				es.cubemapOctree,
				"Sparse adaptive placement; growth stops at the\n"
				"32-probe runtime budget.");
			if (es.cubemapOctree) {
				ImGui::Indent();
				dragFloat("Max cell (m)", "cubemapOctreeMax",
					es.cubemapOctreeMax, 0.25f, 0.5f, 64.0f, "%.1f");
				dragFloat("Min cell (m)", "cubemapOctreeMin",
					es.cubemapOctreeMin, 0.1f, 0.125f,
					std::max(es.cubemapOctreeMax, 0.25f), "%.2f");
				dragInt("Depth cap", "cubemapOctreeDepth",
					int(es.cubemapOctreeDepth), 1.0f, 0, 8);
				dragFloat("Pinned influence (m)", "cubemapExtraRadius",
					es.cubemapExtraRadius, 0.25f, 0.5f, 50.0f, "%.1f");
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}

		ImGui::Separator();
		ImGui::Text("GI bakes (on demand — Bake GI in the Cook panel)");
		setBool("GI volumes (bake authored)", "giVolumes", es.giVolumes,
			"Bake the authored GI volume nodes (gi/<stem>_*.givolume).");
		ImGui::TextDisabled("the world-field octree bake is retired — convex\n"
			"volumes + the sky IBL carry the ambient");
	}

	void EditorApp::DrawCubemapSection(uint32_t node) {
		const attr::ScopedID componentId("cubemap");
		// Authored cubemap capture on this node (editor cubemap node): a
		// PINNED specular probe. Position = the node's translate (gizmo);
		// the influence radius below. Baked by the cook's --cubemap stage
		// as a 'pin' row (wins weight ties at load).
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		auto it = FindCubemap(scene, node);
		if (!it)
			return;
		::SceneCubemap& def = it->cubemap;
		ImGui::Separator();
		ImGui::Text("Cubemap capture: '%s'",
			getNodeName(scene, int(node)).c_str());
		ImGui::TextDisabled("pinned probe — wins weight ties at load");
		auto beginCubeDrag = [&](const char* attr) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetCubemapAttr ||
				m_pending.cmd.node != node || m_pending.cmd.cubemapAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetCubemapAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.cubemapAttr = attr;
				ReadNodeCubemapAttr(node, attr, m_pending.cmd.beforeVec4,
					m_pending.cmd.beforeNum, m_pending.cmd.beforeText);
			}
		};
		auto endCubeDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.verb == EditVerb::SetCubemapAttr &&
				m_pending.cmd.node == node &&
				m_pending.cmd.cubemapAttr == attr)
				FinishDrag();
		};
		float radius = def.radius;
		if (attr::Float("Influence radius", &radius, 0.05f, 0.05f,
				100.0f, "%.2f",
				"The probe's full-weight radius (the fade ring is "
				"radius/2); position = node translate (gizmo); bake: "
				"Cook panel > Cubemap captures (pinned rows)")) {
			beginCubeDrag("radius");
			WriteNodeCubemapAttr(node, "radius", glm::vec4(0),
				double(radius), {});
			m_pending.cmd.number = double(radius);
		}
		endCubeDrag("radius");
	}

	void EditorApp::DrawCamSection(uint32_t node) {
		const attr::ScopedID componentId("camera");
		// Authored camera on this node (editor camera node): the LENS below;
		// the pose is the node's transform (gizmo), forward = node -Z in
		// world. Undoable SetCamAttr edits persisted as setCamAttr tour
		// verbs so cooks replay them.
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		if (!(FindCamera(scene, node) != nullptr))
			return;
		ImGui::Separator();
		ImGui::Text("Camera: '%s'",
			getNodeName(scene, int(node)).c_str());
		ImGui::TextDisabled("pose = node transform (gizmo), forward = -Z");
		const ::SceneCamera* C = FindNodeCamera(node);
		if (!C)
			return;
		auto beginCamDrag = [&](const char* attr) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetCamAttr ||
				m_pending.cmd.node != node || m_pending.cmd.camAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetCamAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.camAttr = attr;
				ReadNodeCameraAttr(node, attr, m_pending.cmd.beforeVec4,
					m_pending.cmd.beforeNum, m_pending.cmd.beforeText);
			}
		};
		auto endCamDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.verb == EditVerb::SetCamAttr &&
				m_pending.cmd.node == node &&
				m_pending.cmd.camAttr == attr)
				FinishDrag();
		};
		int proj = (C->projection
			== ::SceneCamera::Projection::Orthographic) ? 1 : 0;
		if (attr::Enum("Projection", &proj,
				"Perspective\0Orthographic\0", "The lens projection")) {
			EditCmd c{};
			c.verb = EditVerb::SetCamAttr;
			c.node = node;
			c.camAttr = "projection";
			c.number = double(proj);
			ReadNodeCameraAttr(node, "projection", c.beforeVec4,
				c.beforeNum, c.beforeText);
			Do(c);
		}
		if (proj == 0) {
			float fov = C->fovYDeg;
			if (attr::Float("FOV", &fov, 1.0f, 1.0f, 179.0f, "%.0f",
					"The vertical field of view (deg)")) {
				beginCamDrag("fovY");
				WriteNodeCameraAttr(node, "fovY", glm::vec4(0),
					double(fov), {});
				m_pending.cmd.number = double(fov);
			}
			endCamDrag("fovY");
		}
		else {
			float hh = C->orthoHalfH;
			if (attr::Float("Ortho half-height", &hh, 0.25f, 0.01f,
					100000.0f, "%.1f", "The orthographic vertical "
					"half-extent (m)")) {
				beginCamDrag("orthoHalfH");
				WriteNodeCameraAttr(node, "orthoHalfH", glm::vec4(0),
					double(hh), {});
				m_pending.cmd.number = double(hh);
			}
			endCamDrag("orthoHalfH");
		}
		float nc = C->nearClip;
		if (attr::Float("Near clip", &nc, 0.005f, 0.0001f, 50.0f,
				"%.4f", "The near plane (m)")) {
			beginCamDrag("near");
			WriteNodeCameraAttr(node, "near", glm::vec4(0), double(nc), {});
			m_pending.cmd.number = double(nc);
		}
		endCamDrag("near");
		float fc = C->farClip;
		if (attr::Float("Far clip", &fc, 5.0f, 0.01f, 1000000.0f,
				"%.0f", "The far plane (m)")) {
			beginCamDrag("far");
			WriteNodeCameraAttr(node, "far", glm::vec4(0), double(fc), {});
			m_pending.cmd.number = double(fc);
		}
		endCamDrag("far");
	}

	void EditorApp::DrawAudioSection(uint32_t node) {
		const attr::ScopedID componentId("audio_source");
		// Authored audio source on this node (the emitter association):
		// the payload knobs + the SOUND SLOTS below — asset references,
		// never playing voices (the runtime audio manager owns playback;
		// clips are shared by many emitters). The emitter POSE is the
		// node's transform (gizmo) — the audio manager resolves the
		// GLOBAL transform at use, so parenting applies like meshes.
		// Undoable SetAudioAttr/AddAudioSound/RemoveAudioSound edits
		// persisted as the tabulated tour verbs (cooks replay them).
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		AudioSourceRow* it = FindAudioSource(scene, node);
		if (!it)
			return;
		::SceneAudioSource& def = it->source;
		ImGui::Separator();
		ImGui::Text("Audio source: '%s'",
			getNodeName(scene, int(node)).c_str());
		ImGui::TextDisabled("emitter — pose = node transform (gizmo);"
			"\nsounds are asset references, not playing voices");
		// The drag pattern (one coalesced SetAudioAttr per drag).
		auto beginAudioDrag = [&](const char* attr) {
			if (m_pending.active
				&& (m_pending.cmd.verb != EditVerb::SetAudioAttr
					|| m_pending.cmd.node != node
					|| m_pending.cmd.audioAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetAudioAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.audioAttr = attr;
				ReadNodeAudioSourceAttr(node, attr,
					m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum,
					m_pending.cmd.beforeFlag, m_pending.cmd.beforeText);
			}
		};
		auto endAudioDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active
				&& m_pending.cmd.verb == EditVerb::SetAudioAttr
				&& m_pending.cmd.node == node
				&& m_pending.cmd.audioAttr == attr)
				FinishDrag();
		};
		float vol = def.volume;
		if (attr::Float("Volume", &vol, 0.01f, 0.0f, 16.0f, "%.2f",
				"The emitter's master gain")) {
			beginAudioDrag("volume");
			WriteNodeAudioSourceAttr(node, "volume", glm::vec4(0),
				double(vol), false, {});
			m_pending.cmd.number = double(vol);
		}
		endAudioDrag("volume");
		float mind = def.minDistance;
		if (attr::Float("Min distance (m)", &mind, 0.1f, 0.0f, 1000.0f,
				"%.1f", "Full-volume radius (the attenuation starts"
				" past this)")) {
			beginAudioDrag("minDistance");
			WriteNodeAudioSourceAttr(node, "minDistance", glm::vec4(0),
				double(mind), false, {});
			m_pending.cmd.number = double(mind);
		}
		endAudioDrag("minDistance");
		float maxd = def.maxDistance;
		if (attr::Float("Max distance (m)", &maxd, 0.5f, 0.01f, 10000.0f,
				"%.1f", "Silent radius (the attenuation ends here)")) {
			beginAudioDrag("maxDistance");
			WriteNodeAudioSourceAttr(node, "maxDistance", glm::vec4(0),
				double(maxd), false, {});
			m_pending.cmd.number = double(maxd);
		}
		endAudioDrag("maxDistance");
		// Immediate checkboxes (chips — one Do per click, exact inverse).
		{
			bool spatial = def.spatial;
			if (attr::Check("Spatial (3D)", &spatial,
					"Distance attenuation + stereo pan from the"
					" listener (off = a 2D full-mix source)")) {
				EditCmd c{};
				c.verb = EditVerb::SetAudioAttr;
				c.node = node;
				c.audioAttr = "spatial";
				ReadNodeAudioSourceAttr(node, "spatial", c.beforeVec4,
					c.beforeNum, c.beforeFlag, c.beforeText);
				c.afterFlag = spatial;
				c.number = c.beforeNum;
				WriteNodeAudioSourceAttr(node, "spatial", glm::vec4(0),
					0.0, spatial, {});
				Do(c);
			}
			bool pol = def.playOnLoad;
			if (attr::Check("Play on load", &pol,
					"Start the first slot's clip when the scene loads"
					" (the runtime autoplay)")) {
				EditCmd c{};
				c.verb = EditVerb::SetAudioAttr;
				c.node = node;
				c.audioAttr = "playOnLoad";
				ReadNodeAudioSourceAttr(node, "playOnLoad", c.beforeVec4,
					c.beforeNum, c.beforeFlag, c.beforeText);
				c.afterFlag = pol;
				c.number = c.beforeNum;
				WriteNodeAudioSourceAttr(node, "playOnLoad",
					glm::vec4(0), 0.0, pol, {});
				Do(c);
			}
		}
		// ---- the sound slots (asset references, ordered) ----
		ImGui::TextDisabled("sounds");
		for (size_t si = 0; si < def.sounds.size(); ++si) {
			ImGui::PushID(int(si));
			const AudioSoundRef& slot = def.sounds[si];
			ImGui::Text("%s", slot.name.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) {
				// Remove (the removed slot rides the command's
				// before-state — the exec captures it).
				EditCmd c{};
				c.verb = EditVerb::RemoveAudioSound;
				c.node = node;
				c.afterText = slot.name;
				Do(c);
				ImGui::PopID();
				break; // the row vector just mutated
			}
			// The asset path (an addAudioSound UPSERT on commit — the
			// same stable slot name, the new path).
			char path[256] = {};
			std::snprintf(path, sizeof(path), "%s",
				slot.assetPath.c_str());
			if (ImGui::InputText("path", path, sizeof(path),
					ImGuiInputTextFlags_EnterReturnsTrue)) {
				EditCmd c{};
				c.verb = EditVerb::AddAudioSound;
				c.node = node;
				c.afterText = slot.name;
				c.audioPath = path;
				c.number = double(slot.gain);
				c.afterFlag = slot.loop;
				Do(c);
			}
			float gain = slot.gain;
			if (attr::Float("gain", &gain, 0.01f, 0.0f, 16.0f, "%.2f",
					"Per-slot gain")) {
				EditCmd c{};
				c.verb = EditVerb::AddAudioSound;
				c.node = node;
				c.afterText = slot.name;
				c.audioPath = slot.assetPath;
				c.number = double(gain);
				c.afterFlag = slot.loop;
				Do(c);
				ImGui::PopID();
				break; // upsert re-ordering safety
			}
			bool loop = slot.loop;
			if (attr::Check("loop", &loop, "The slot's loop default")) {
				EditCmd c{};
				c.verb = EditVerb::AddAudioSound;
				c.node = node;
				c.afterText = slot.name;
				c.audioPath = slot.assetPath;
				c.number = double(slot.gain);
				c.afterFlag = loop;
				Do(c);
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
		if (ImGui::Button("+ Add slot")) {
			// A unique slot name over this source's live slots.
			std::string name = "Sound";
			for (int n = 2;; ++n) {
				bool taken = false;
				for (const AudioSoundRef& s : def.sounds)
					if (s.name == name) {
						taken = true;
						break;
					}
				if (!taken)
					break;
				name = "Sound" + std::to_string(n);
			}
			EditCmd c{};
			c.verb = EditVerb::AddAudioSound;
			c.node = node;
			c.afterText = name;
			c.audioPath = "content/audio/new.wav";
			c.number = 1.0;
			c.afterFlag = false;
			Do(c);
		}
	}

} // namespace editor
