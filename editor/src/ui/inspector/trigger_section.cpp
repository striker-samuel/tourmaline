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

} // namespace editor
