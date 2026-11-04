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

} // namespace editor
