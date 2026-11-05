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
	void EditorApp::DrawAudioRoomSection(uint32_t node) {
		const attr::ScopedID componentId("audio_room");
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	if (node >= scene.hierarchy.size())
		return;
	const tour::scene::AudioRoomRow* row =
		tour::scene::FindAudioRoom(scene, node);
	if (!row)
		return;
	const tour::scene::SceneAudioRoom& def = row->room;
	if (attr::ComponentHeader("Audio Room")) {
		EditCmd cmd{};
		cmd.verb = EditVerb::RemoveAudioRoomVerb;
		cmd.node = node;
		Do(cmd);
		return;
	}
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

}

} // namespace editor
