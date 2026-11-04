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

} // namespace editor
