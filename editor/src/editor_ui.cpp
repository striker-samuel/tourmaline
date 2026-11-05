// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "ui/panel_registry.h"
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
	// Dock windows own their placement and tabs. RenderFrame consumes the
	// last completed "Viewport" client rectangle for its GPU scene pass.

	// Coalesced drag helpers (member functions for world access).
	void EditorApp::DrawBrushPresetControls(bool scatter) {
	using namespace brush;
	ImGui::PushID(scatter ? "scatter_preset" : "terrain_preset");
	char* nameBuf = scatter ? m_scatterPresetName : m_terrainPresetName;
	ImGui::SeparatorText(scatter ? "Scatter brush presets" : "Terrain brush presets");
	if (ImGui::BeginCombo("Brush preset", nameBuf)) {
		for (const auto& path : Assets()) {
			Preset p;
			if (!Load(path, p) || p.kind != (scatter ? Kind::Scatter : Kind::Terrain))
				continue;
			if (ImGui::Selectable(p.name.c_str())) {
				std::snprintf(nameBuf, 64, "%s",
					p.name.c_str());
				if (scatter) {
					m_scatterBrushRadius = p.radius;
					m_scatterBrushStrength = std::clamp(p.strength, 0.0f, 1.0f);
					m_scatterBrushSpacing = p.spacing;
				} else {
					m_paintRadius = p.radius;
					m_paintStrength = p.strength;
					m_paintStrokeSpacing = p.spacing;
				}
				if (!scatter) {
					m_paintOp = p.operation;
				} else if (auto* graph = m_placement.current()) {
					m_placement.pushUndo();
					for (auto& [id, node] : graph->nodes) {
						using placement::NodeKind;
						if (node.kind == NodeKind::Density)
							placement::SetAttr(node, "value", p.density);
						else if (node.kind == NodeKind::Transform) {
							placement::SetAttr(node, "minScale", p.minScale);
							placement::SetAttr(node, "maxScale", p.maxScale);
						} else if (node.kind == NodeKind::Spacing)
							placement::SetAttr(node, "minSpacing", p.minDistance);
					}
					MarkPlacementDirty();
				}
				Log("Loaded brush preset: " + p.name);
			}
		}
		ImGui::EndCombo();
	}
	ImGui::InputText("Asset name", nameBuf, 64);
	if (scatter)
		ImGui::TextDisabled("Applies to Density, Transform and Spacing nodes; brush shape controls viewport mask painting.");
	else
		ImGui::DragFloat("Stroke spacing", &m_paintStrokeSpacing, 0.02f, 0.05f, 2.0f,
			"%.2f radius");
	if (ImGui::Button("Save brush preset")) {
		Preset p;
		p.name = nameBuf;
		p.kind = scatter ? Kind::Scatter : Kind::Terrain;
		p.radius = scatter ? m_scatterBrushRadius : m_paintRadius;
		p.strength = scatter ? m_scatterBrushStrength : m_paintStrength;
		p.spacing = scatter ? m_scatterBrushSpacing : m_paintStrokeSpacing;
		p.operation = m_paintOp;
		if (scatter) {
			if (const auto* graph = m_placement.current()) {
				for (const auto& [id, node] : graph->nodes) {
					using placement::NodeKind;
					if (node.kind == NodeKind::Density)
						p.density = float(placement::GetAttr(node, "value"));
					else if (node.kind == NodeKind::Transform) {
						p.minScale = float(placement::GetAttr(node, "minScale"));
						p.maxScale = float(placement::GetAttr(node, "maxScale"));
					} else if (node.kind == NodeKind::Spacing)
						p.minDistance = float(placement::GetAttr(node, "minSpacing"));
				}
			}
		}
		Log(Save(p) ? "Saved brush preset asset: " + p.name
			: "Brush preset save failed (check name and parameter ranges).");
	}
	ImGui::PopID();
}

void EditorApp::FinalizePending() {
		if (!m_pending.active)
			return;
		EditCmd& c = m_pending.cmd;
		switch (c.verb) {
		case EditVerb::Translate:
		case EditVerb::Rotate:
		case EditVerb::Scale: {
			glm::vec3 t, r, s;
			ReadLocalTRS(c.node, t, r, s);
			if (c.verb == EditVerb::Translate)
				c.afterVec = t;
			else if (c.verb == EditVerb::Rotate)
				c.afterVec = r;
			else
				c.afterVec = s;
			break;
		}
		case EditVerb::SetMatAttr: {
			glm::vec4 v4;
			double num = 0.0;
			ReadSharedMatAttr(c.matWorld, c.attr, v4, num);
			c.vec4 = v4;
			c.number = num;
			break;
		}
		default:
			break;
		}
		PushDone(c);
		m_pending.active = false;
		m_pending.cmd = EditCmd{};
	}

	bool EditorApp::TrackDrag(EditVerb verb, uint32_t node,
		const glm::mat4& beforeMat, bool activated) {
		if (activated) {
			if (m_pending.active && (m_pending.cmd.verb != verb ||
				m_pending.cmd.node != node))
				FinalizePending(); // switching controls mid-gesture
			m_pending.active = true;
			m_pending.cmd = EditCmd{};
			m_pending.cmd.verb = verb;
			m_pending.cmd.node = node;
			m_pending.cmd.beforeMat = beforeMat;
			return true;
		}
		return m_pending.active && m_pending.cmd.verb == verb &&
			m_pending.cmd.node == node;
	}

	void EditorApp::FinishDrag() {
		PushDone(m_pending.cmd);
		m_pending.active = false;
		m_pending.cmd = EditCmd{};
	}


	// Editor terrain-group top view: world X is right, +Z is down.
	// The heightmap remains the same authoritative grid as terrain painting.
	void EditorApp::DrawWorldMapPanel() {
		if (!m_world || m_terrainGroups.empty()) {
			ImGui::TextDisabled("Create a terrain to display its heightmap.");
			return;
		}
		std::vector<uint32_t> ids;
		for (const auto& [id, g] : m_terrainGroups)
			if (!g.deleted && g.data.heights.width > 1 &&
				g.data.heights.height > 1) ids.push_back(id);
		std::sort(ids.begin(), ids.end());
		if (ids.empty()) { ImGui::TextDisabled("No editable terrain."); return; }
		const uint32_t selected = TerrainGroupOf(m_selected);
		if (selected != UINT32_MAX && selected != m_worldMapGroup &&
			std::binary_search(ids.begin(), ids.end(), selected)) {
			m_worldMapGroup = selected;
			m_worldMapScale = 0.0f;
		}
		if (!std::binary_search(ids.begin(), ids.end(), m_worldMapGroup))
			m_worldMapGroup = ids.front();
		uint32_t group = m_worldMapGroup;
		if (ImGui::BeginCombo("Terrain group", std::to_string(group).c_str())) {
			for (uint32_t id : ids)
				if (ImGui::Selectable(std::to_string(id).c_str(), id == group))
					group = id;
			ImGui::EndCombo();
		}
		if (group != m_worldMapGroup) { m_worldMapGroup = group; m_worldMapScale = 0; }
		auto& data = m_terrainGroups.at(group).data;
		const auto& p = data.params;
		if (p.size <= 0.0f) return;
		ImGui::SameLine();
		if (ImGui::Button("Fit")) m_worldMapScale = 0.0f;
		ImGui::SameLine();
		ImGui::Checkbox("Sculpt", &m_worldMapSculpt);
		if (m_worldMapSculpt) {
			const char* names[] = {"Raise", "Lower", "Flatten", "Smooth"};
			ImGui::Combo("Brush", &m_paintOp, names, 4);
			ImGui::DragFloat("Radius (m)", &m_paintRadius, 0.25f, 0.5f, 256.f);
			ImGui::DragFloat("Strength", &m_paintStrength, 0.01f, 0.01f, 64.f);
		}
		const ImVec2 pos = ImGui::GetCursorScreenPos(), dim = ImGui::GetContentRegionAvail();
		if (dim.x < 32 || dim.y < 32) return;
		ImGui::InvisibleButton("##topMap", dim,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
			ImGuiButtonFlags_MouseButtonRight);
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const ImVec2 mid(pos.x + dim.x * 0.5f, pos.y + dim.y * 0.5f);
		if (m_worldMapScale <= 0) {
			m_worldMapScale = p.size * 1.1f / std::max(1.f, std::min(dim.x, dim.y));
			m_worldMapCenter = p.origin + glm::vec2(p.size * 0.5f);
		}
		auto toWorld = [&](ImVec2 v) {
			return m_worldMapCenter + glm::vec2(v.x-mid.x, v.y-mid.y)*m_worldMapScale;
		};
		auto toPixel = [&](glm::vec2 v) {
			return ImVec2(mid.x+(v.x-m_worldMapCenter.x)/m_worldMapScale,
				mid.y+(v.y-m_worldMapCenter.y)/m_worldMapScale);
		};
		if (hovered && ImGui::GetIO().MouseWheel != 0) {
			const glm::vec2 anchor = toWorld(mouse);
			m_worldMapScale = std::clamp(m_worldMapScale *
				std::pow(1.15f, -ImGui::GetIO().MouseWheel),
				p.size/16384.f, p.size*16.f);
			m_worldMapCenter += anchor - toWorld(mouse);
		}
		if (ImGui::IsItemActive() &&
			(ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
			 ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
			const auto d = ImGui::GetIO().MouseDelta;
			m_worldMapCenter -= glm::vec2(d.x, d.y)*m_worldMapScale;
		}
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 end(pos.x+dim.x, pos.y+dim.y);
		draw->AddRectFilled(pos, end, IM_COL32(24, 28, 33, 255));
		draw->PushClipRect(pos, end, true);
		// Bounded CPU preview; no GPU readback and no duplicated terrain data.
		const int nx = std::clamp(int(dim.x / 7), 1, 96);
		const int nz = std::clamp(int(dim.y / 7), 1, 96);
		const float dx = dim.x/nx, dz = dim.y/nz;
		for (int z=0; z<nz; ++z) for (int x=0; x<nx; ++x) {
			const glm::vec2 w = toWorld(ImVec2(pos.x+(x+.5f)*dx,pos.y+(z+.5f)*dz));
			if (w.x < p.origin.x || w.y < p.origin.y ||
				w.x > p.origin.x+p.size || w.y > p.origin.y+p.size) continue;
			const float level = std::clamp(data.heights.sampleHeight(w.x,w.y) /
				std::max(1.f,p.height),0.f,1.f);
			const int v = int(55+level*165);
			draw->AddRectFilled(ImVec2(pos.x+x*dx,pos.y+z*dz),
				ImVec2(pos.x+(x+1)*dx+.5f,pos.y+(z+1)*dz+.5f),
				IM_COL32(v,v,v,255));
		}
		// Tile outlines follow shared grid samples, not approximate equal sizes.
		const float step = p.size / float(data.heights.width-1);
		for (uint32_t tz=0; tz<data.tilesZ; ++tz)
			for (uint32_t tx=0; tx<data.tilesX; ++tx) {
				uint32_t x0,x1,z0,z1;
				smvk::placement::TerrainTileRange(data,tx,tz,x0,x1,z0,z1);
				draw->AddRect(toPixel(p.origin+glm::vec2(x0*step,z0*step)),
					toPixel(p.origin+glm::vec2(x1*step,z1*step)),
					IM_COL32(240,203,113,220));
			}
		const glm::vec2 cursor = toWorld(mouse);
		const bool inside = cursor.x>=p.origin.x && cursor.y>=p.origin.y &&
			cursor.x<=p.origin.x+p.size && cursor.y<=p.origin.y+p.size;
		if (hovered && inside) {
			if (m_worldMapSculpt)
				draw->AddCircle(mouse,m_paintRadius/m_worldMapScale,
					IM_COL32(141,239,157,255),48,1.5f);
			ImGui::SetTooltip("X %.2f  Z %.2f  Y %.2f m",cursor.x,cursor.y,
				data.heights.sampleHeight(cursor.x,cursor.y));
		}
		draw->PopClipRect();
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			m_worldMapLastStamp = glm::vec2(-1e9f);
		if (m_worldMapSculpt && inside && ImGui::IsItemActive() &&
			ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
			glm::distance(cursor,m_worldMapLastStamp) >=
				std::max(0.1f,m_paintRadius*m_paintStrokeSpacing)) {
			static const char* ops[] = {"raise","lower","flatten","smooth"};
			EditCmd cmd{};
			cmd.verb = EditVerb::TerrainPaint;
			cmd.node = group;
			cmd.terrainOp = ops[std::clamp(m_paintOp,0,3)];
			cmd.vec4 = glm::vec4(cursor.x,cursor.y,m_paintRadius,
				m_paintOp==2 ? data.heights.sampleHeight(cursor.x,cursor.y) : 0.f);
			cmd.number = double(m_paintStrength);
			Do(cmd);
			m_worldMapLastStamp = cursor;
		}
	}

	// ---- the live-view image lane (the offscreen scene RT) ----------
	// Bind the WYSIWYG inline LDR target as an ImGui user texture for BOTH
	// docked and detached Viewport windows. ImGui's own draw order and clip
	// rects keep the scene below the dock chrome, panels, and popups. The same shape
	// as TextureThumb: a cached descriptor rebuilt when the physical
	// view changes (an inline-target recreate) and released on
	// teardown/recreate - never per frame.
	ImTextureID EditorApp::WysiwygStageTexture() {
		if (!m_wysiwyg || !m_wysiwygValid || !m_context || !m_imgui)
			return ImTextureID(0);
		const smvk::ImageViewHandle view = m_wysiwyg->InlineTargetView();
		if (view.empty())
			return ImTextureID(0);
		if (m_wysiwygDetachedTex != 0
			&& m_wysiwygDetachedPhysical == view)
			return m_wysiwygDetachedTex;
		DropWysiwygStageTexture();
		const void* vkView = m_context->GetImageViewVkImageView(view);
		if (!vkView)
			return ImTextureID(0);
		// The same linear/clamp sampler the main-swapchain composite
		// samples the target with (lazy-shared with the thumbnail lane).
		if (m_cubeFaceSampler == UINT32_MAX) {
			m_cubeFaceSampler = m_context->GetOrCreateSampler(
				smvk::SamplerCreateInfo{
					.magFilter = smvk::Filter::Linear,
					.minFilter = smvk::Filter::Linear,
					.mipmapMode = smvk::MipmapMode::Nearest,
					.addressU = smvk::AddressMode::ClampToEdge,
					.addressV = smvk::AddressMode::ClampToEdge,
					.addressW = smvk::AddressMode::ClampToEdge,
					.maxAnisotropy = 1.0f });
		}
		const void* vkSampler =
			m_context->GetVkSamplerOpaque(m_cubeFaceSampler);
		if (!vkSampler)
			return ImTextureID(0);
		VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
			VkSampler(const_cast<void*>(vkSampler)),
			VkImageView(const_cast<void*>(vkView)),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if (set == VK_NULL_HANDLE)
			return ImTextureID(0);
		m_wysiwygDetachedTex = ImTextureID(reinterpret_cast<ImU64>(set));
		m_wysiwygDetachedPhysical = view;
		return m_wysiwygDetachedTex;
	}

	void EditorApp::DropWysiwygStageTexture() {
		if (m_wysiwygDetachedTex == 0) {
			m_wysiwygDetachedPhysical = smvk::ImageViewHandle{};
			return;
		}
		ImGui_ImplVulkan_RemoveTexture(
			reinterpret_cast<VkDescriptorSet>(ImU64(m_wysiwygDetachedTex)));
		m_wysiwygDetachedTex = 0;
		m_wysiwygDetachedPhysical = smvk::ImageViewHandle{};
	}

	ImTextureID EditorApp::LegacyStageTexture() {
		if (!m_imgui || !m_context || m_legacyStageView.empty())
			return ImTextureID(0);
		const smvk::ImageViewHandle view = m_legacyStageView;
		if (m_legacyStageTex != 0 && m_legacyStagePhysical == view)
			return m_legacyStageTex;
		DropLegacyStageTexture();
		const void* vkView = m_context->GetImageViewVkImageView(view);
		if (!vkView)
			return ImTextureID(0);
		if (m_cubeFaceSampler == UINT32_MAX) {
			m_cubeFaceSampler = m_context->GetOrCreateSampler(
				smvk::SamplerCreateInfo{
					.magFilter = smvk::Filter::Linear,
					.minFilter = smvk::Filter::Linear,
					.mipmapMode = smvk::MipmapMode::Nearest,
					.addressU = smvk::AddressMode::ClampToEdge,
					.addressV = smvk::AddressMode::ClampToEdge,
					.addressW = smvk::AddressMode::ClampToEdge,
					.maxAnisotropy = 1.0f });
		}
		const void* vkSampler = m_context->GetVkSamplerOpaque(m_cubeFaceSampler);
		if (!vkSampler)
			return ImTextureID(0);
		const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
			VkSampler(const_cast<void*>(vkSampler)),
			VkImageView(const_cast<void*>(vkView)),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if (set == VK_NULL_HANDLE)
			return ImTextureID(0);
		m_legacyStageTex = ImTextureID(reinterpret_cast<ImU64>(set));
		m_legacyStagePhysical = view;
		return m_legacyStageTex;
	}

	void EditorApp::DropLegacyStageTexture() {
		if (m_legacyStageTex != 0)
			ImGui_ImplVulkan_RemoveTexture(
				reinterpret_cast<VkDescriptorSet>(ImU64(m_legacyStageTex)));
		m_legacyStageTex = 0;
		m_legacyStagePhysical = {};
	}

	void EditorApp::DrawUI(smvk::Extent2D targetExtent) {
		(void)targetExtent;
		if (ImGui::BeginMainMenuBar()) {
			DrawMenuBar();
			ImGui::EndMainMenuBar();
		}
		// A normal floating project configuration window must not be
		// nested in ImGui's main menu-bar window stack.
		DrawProjectExportWindow();
		DrawImportWindow();
		DrawIntersectionToolsWindow();
		// Options is a large standalone settings window, not a dock tab.
		// Render settings retain their existing live-update/autosave behavior.
		if (m_showRenderSettingsPanel) {
			ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f),
				ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Options", &m_showRenderSettingsPanel,
				ImGuiWindowFlags_NoDocking)) {
				if (ImGui::BeginTabBar("OptionsTabs")) {
					if (ImGui::BeginTabItem("Rendering")) {
						ImGui::BeginChild("RenderingOptions", ImVec2(0, 0));
						DrawRenderSettingsPanel();
						ImGui::EndChild();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Workspace")) {
						ImGui::SeparatorText("Editor workspace");
						attr::Check("Ground grid", &m_showGrid, "Show the viewport ground grid");
						attr::Check("Selection wire overlay", &m_wireOverlay, "Show selected mesh outlines");
						attr::Check("Light gizmos", &m_showLightGizmos, "Show light icons");
						attr::Check("Camera frustums", &m_showCameraGizmos, "Show camera frustums");
						attr::Check("GI volume boxes", &m_showGiVolumeGizmos, "Show GI volume bounds");
						attr::Check("Cubemap capture markers", &m_showCubemapGizmos, "Show cubemap markers");
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
			}
			ImGui::End();
		}
		// Tool topbar: tabbed toolbar directly under the menu bar (hidden
		// with the panels via m_uiMode). Everything below keys off the
		// combined menu+toolbar offset (m_menuH), so panels, splitters and
		// the rendered viewport rects stay in lockstep. The dockspace
		// occupies the remaining space above the status strip; moving its
		// Viewport window changes the GPU render rectangle on the next frame.
		const float menuBarH = ImGui::GetFrameHeight();
		const float toolbarH = m_uiMode ? DrawToolTabs(menuBarH) : 0.0f;
		m_menuH = menuBarH + toolbarH;
		const float menuH = m_menuH; // panels/viewport math use the combined offset
		const float vh = float(ImGui::GetIO().DisplaySize.y);
		// Del-key routing resets here: DrawPlacementGraph re-sets it when its
		// window hierarchy holds the keyboard focus (ModalDelete vs the
		// graph's own node/wire delete).
		m_imguiFocusInPlacementGraph = false;
		// Window title tracks the scene + dirty star (change-detected).
		UpdateWindowTitle();

		// A real ImGui dockspace replaces the old fixed-position columns.
		// Panels can be docked, tabbed, split and floated WITHIN the editor
		// window; ImGui saves the workspace to editor.imgui.ini. Platform
		// multi-viewports ARE enabled (editor_app.cpp) so undocked panels
		// become real OS windows; the Viewport stage supports that through
		// the offscreen image lane (see the Viewport window below) - the
		// editor's rendergraph still presents only the primary swapchain.
		const ImVec2 mainPos = ImGui::GetMainViewport()->Pos;
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		ImGui::SetNextWindowPos(ImVec2(mainPos.x, mainPos.y + menuH), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x,
			std::max(1.0f, vh - menuH - kEditorStatusH)), ImGuiCond_Always);
		const ImGuiWindowFlags hostFlags =
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoNavFocus |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoBackground |
			(m_uiMode ? 0 : ImGuiWindowFlags_NoInputs);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		const bool hostVisible = ImGui::Begin("##TourmalineDockHost",
			nullptr, hostFlags);
		if (hostVisible) {
			const ImGuiID dockId = ImGui::GetID("TourmalineDockspace");
			// Build once, only for a new user or an explicit reset.
			// Existing ImGui .ini nodes win over the compiled defaults.
			if (m_uiMode && (m_dockResetRequested ||
				!ImGui::DockBuilderGetNode(dockId))) {
				ImGui::DockBuilderRemoveNode(dockId);
				ImGui::DockBuilderAddNode(dockId,
					ImGuiDockNodeFlags_DockSpace |
					ImGuiDockNodeFlags_PassthruCentralNode);
				ImGui::DockBuilderSetNodePos(dockId,
					ImVec2(mainPos.x, mainPos.y + menuH));
				ImGui::DockBuilderSetNodeSize(dockId,
					ImVec2(ImGui::GetIO().DisplaySize.x,
						std::max(1.0f, vh - menuH - kEditorStatusH)));
				ImGuiID center = dockId;
				ImGuiID left = ImGui::DockBuilderSplitNode(center,
					ImGuiDir_Left, 0.24f, nullptr, &center);
				ImGuiID right = ImGui::DockBuilderSplitNode(center,
					ImGuiDir_Right, 0.28f, nullptr, &center);
				ImGuiID bottom = ImGui::DockBuilderSplitNode(center,
					ImGuiDir_Down, 0.22f, nullptr, &center);
				ImGuiID leftUpper = ImGui::DockBuilderSplitNode(left,
					ImGuiDir_Up, 0.54f, nullptr, &left);
				ImGuiID mapNode = ImGui::DockBuilderSplitNode(center,
					ImGuiDir_Right, 0.38f, nullptr, &center);
				ImGui::DockBuilderDockWindow("Viewport", center);
				ImGui::DockBuilderDockWindow("World Map", mapNode);
				ImGui::DockBuilderDockWindow("Outliner", leftUpper);
				ImGui::DockBuilderDockWindow("Assets", left);
				ImGui::DockBuilderDockWindow("Console", bottom);
				ImGui::DockBuilderDockWindow("Overrides", left);
				ImGui::DockBuilderDockWindow("Inspector", right);
				ImGui::DockBuilderDockWindow("Material", right);
				// The Reference panel gets a dock HOME in the default tree
				// (the right column, tabbed with Inspector/Material). It
				// starts hidden now (m_showReferencePanel), but the builder
				// assignment lands in the window SETTINGS - DockBuilder-
				// DockWindow docks never-created windows through their
				// settings - so the first View > Panels open lands DOCKED
				// instead of a floating window over the viewport stage.
				ImGui::DockBuilderDockWindow("Reference", right);
				ImGui::DockBuilderFinish(dockId);
				m_dockResetRequested = false;
			}
			// Never paint an opaque dockspace background over the swapchain
			// scene. Tool windows keep their normal opaque backgrounds.
			ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg,
				ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			// Submitting KeepAliveOnly while panels are hidden preserves
			// dock assignments, even if the user toggles fullscreen with I.
			ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f),
				m_uiMode ? ImGuiDockNodeFlags_PassthruCentralNode
					: ImGuiDockNodeFlags_KeepAliveOnly);
			ImGui::PopStyleColor();
			// THE DEGENERATE-LAYOUT SELF-HEAL (the squeezed-central-node
			// bug): ImGui persists dock splits as ABSOLUTE pixel SizeRefs.
			// A layout authored in a large window (maximized on a big
			// monitor) loads in a smaller one with every side panel keeping
			// its absolute width - ImGui's sizing policy gives non-central
			// children their explicit size and hands the central node the
			// REMAINDER (imgui.cpp DockNodeTreeUpdatePosSize rule 3), so the
			// central node (the Viewport's home) absorbs the whole deficit
			// and collapses to a ~24-64 px sliver. The live view then floors
			// at its 64 px minimum target and composites into a sliver rect:
			// "the docked viewport doesn't render". Heal ONCE, early in the
			// session and never mid-drag: rebuild the compiled default
			// layout (the proven View > Reset workspace path), loudly. The
			// saved panel arrangement is deliberately NOT preserved - a
			// layout that starves the viewport below usability is broken
			// state, not a preference.
			if (m_uiMode && !m_dockResetRequested && !m_dockLayoutHealed
				&& m_frameCounter < 600
				&& !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				if (ImGuiDockNode* central =
						ImGui::DockBuilderGetCentralNode(dockId);
					central && central->Size.x >= 1.0f
					&& (central->Size.x < 240.0f
						|| central->Size.y < 160.0f)) {
					m_dockLayoutHealed = true;
					m_dockResetRequested = true;
					Log("Workspace: the saved dock layout squeezes the "
						"viewport stage below usable size (central node "
						+ std::to_string(int(central->Size.x)) + "x"
						+ std::to_string(int(central->Size.y))
						+ " px) - rebuilding the default layout (View > "
						"Reset workspace to customize)");
				}
			}
		}
		ImGui::End();
		ImGui::PopStyleVar();

		m_dockViewportHovered = false;
		// Drop mouse ownership at release, even if the pointer has left
		// the window. Mouse-down inside the canvas starts a captured orbit.
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)
			&& !ImGui::IsMouseDown(ImGuiMouseButton_Middle)
			&& !ImGui::IsMouseDown(ImGuiMouseButton_Right))
			m_dockViewportDragCaptured = false;
		// A hidden dock tab must not retain the previous live-view capture.
		// DrawLiveViewStage may set this again only for a visible viewport.
		m_wysiwygLiveRectFresh = false;
		if (m_uiMode) {
			// Render the live scene as a normal ImGui image in BOTH docked
			// and detached windows. The old main-swapchain composite ran
			// before ImGui and was subsequently covered by dock-node/window
			// draw commands (visible in RenderDoc as a healthy scene draw
			// followed by an opaque UI draw). An image in the Viewport draw
			// list obeys normal dock/tab clipping and Z-order. Non-live modes
			// retain their legacy direct-to-swapchain rendering when docked.
			m_dockViewportVisible = false;
			m_dockStageDetached = false;
			if (m_showViewportPanel) {
			ImGui::SetNextWindowBgAlpha(0.0f);
			ImGui::SetNextWindowSize(ImVec2(880, 620), ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Viewport", &m_showViewportPanel,
					ImGuiWindowFlags_NoBackground |
					ImGuiWindowFlags_NoScrollbar |
					ImGuiWindowFlags_NoScrollWithMouse |
					ImGuiWindowFlags_NoCollapse)) {
				const ImVec2 pos = ImGui::GetCursorScreenPos();
				const ImVec2 avail = ImGui::GetContentRegionAvail();
				if (avail.x >= 8.0f && avail.y >= 8.0f) {
					m_dockViewportRect = ViewportRect{ pos.x, pos.y,
						avail.x, avail.y };
					m_dockViewportVisible = true;
					// The host viewport decides the lane: a window outside
					// the main viewport presents through ImGui's own
					// swapchain (RenderPlatformWindows), which the editor's
					// rendergraph never touches.
					m_dockStageDetached = ImGui::GetWindowViewport()->ID
						!= ImGui::GetMainViewport()->ID;
					const ImVec2 mouse = ImGui::GetMousePos();
					m_dockViewportHovered = ImGui::IsWindowHovered()
						&& mouse.x >= pos.x && mouse.x < pos.x + avail.x
						&& mouse.y >= pos.y && mouse.y < pos.y + avail.y;
					if (m_dockViewportHovered &&
						(ImGui::IsMouseClicked(ImGuiMouseButton_Left)
							|| ImGui::IsMouseClicked(ImGuiMouseButton_Middle)
							|| ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
						m_dockViewportDragCaptured = true;
					// The inline target is rendered before this UI pass. Sample
					// it inside the Viewport window rather than compositing
					// behind the dockspace, where opaque ImGui draw calls
					// can hide it. Both lanes share descriptor lifetime.
					// ASPECT LOCK: the drive renders the target at the 4:3
					// stage extent (StageViewportRect's aspect-locked fit -
					// the projection derives from it), so the image draws
					// at the SAME stage rect, letterboxed inside the window.
					// A stretch over the full panel would distort the
					// projection whenever the panel is not 4:3.
					const int liveVp = LiveViewViewport();
					const bool live = liveVp >= 0;
					// The legacy preview is rendered in primary-window pixel
					// coordinates. Subrect UVs sample that exact area inside
					// the docked Viewport's normal draw list, with clipping and
					// Z-order identical to the live image lane.
					const auto drawLegacy = [&]() {
						if (m_dockStageDetached)
							return;
						const ImTextureID oldTex = LegacyStageTexture();
						if (oldTex == 0 || m_legacyStageExtent.width == 0
							|| m_legacyStageExtent.height == 0)
							return;
						const ImVec2 origin = ImGui::GetMainViewport()->Pos;
						const float tw = float(m_legacyStageExtent.width);
						const float th = float(m_legacyStageExtent.height);
						for (int vp = 0; vp < ViewportCount(); ++vp) {
							if (vp == liveVp)
								continue;
							const ViewportRect& r = m_viewports[vp].rect;
							if (r.w < 8.0f || r.h < 8.0f || r.x < 0.0f
								|| r.y < 0.0f || r.x + r.w > tw
								|| r.y + r.h > th)
								continue;
							ImGui::SetCursorScreenPos(
								ImVec2(origin.x + r.x, origin.y + r.y));
							ImGui::Image(oldTex, ImVec2(r.w, r.h),
								ImVec2(r.x / tw, r.y / th),
								ImVec2((r.x + r.w) / tw, (r.y + r.h) / th));
						}
					};
					const ImTextureID tex = live
						? WysiwygStageTexture() : ImTextureID(0);
					if (tex != 0) {
						// Reserve the FULL canvas first (the rect capture
						// above and the hover/drag math key on the whole
						// window content), then draw the image at the
						// stage sub-rect.
						ImGui::Dummy(avail);
						ViewportRect stage;
						if (m_dockStageDetached) {
							// The detached window's own content rect is
							// already in ImGui/global coords - the same
							// derivation DrawLiveViewStage's detached
							// branch feeds the drive's extent.
							stage = StageViewportRect(m_dockViewportRect,
								liveVp, m_vpLayout);
						}
						else {
							// Docked: the computed stage is SDL-window
							// local; convert to ImGui's global desktop
							// coords exactly once - the pairing of
							// DrawLiveViewStage's docked conversion, and
							// the same one-frame rect lag the old
							// composite had.
							const ViewportRect& local =
								m_viewports[liveVp].rect;
							const ImVec2 origin =
								ImGui::GetMainViewport()->Pos;
							stage = ViewportRect{ origin.x + local.x,
								origin.y + local.y, local.w, local.h };
						}
						if (stage.w >= 8.0f && stage.h >= 8.0f) {
							ImGui::SetCursorScreenPos(
								ImVec2(stage.x, stage.y));
							ImGui::Image(tex,
								ImVec2(stage.w, stage.h));
						}
						drawLegacy(); // other non-live viewports in multi-view layouts
					}
					else if (!live && !m_dockStageDetached) {
						ImGui::Dummy(avail);
						drawLegacy();
					}
					else if (m_dockStageDetached || live) {
						ImGui::Dummy(avail);
						ImGui::SetCursorPos(ImVec2(
							ImGui::GetCursorPos().x,
							ImGui::GetCursorPos().y - avail.y * 0.5f));
						ImGui::TextDisabled(
							"%s", live
							? "scene render unavailable (attach failed)"
							: "this view mode renders in the editor window -\n"
								"dock the Viewport back or switch to Lit (F1)");
					}
					else
						ImGui::Dummy(avail); // legacy view renders to the swapchain
				}
			}
			ImGui::End();
			}

			// Previously nested tabs are now first-class dock windows:
			// users can move EACH panel to a side, float it, or tab it
			// with the viewport / another tool. The Embree reference
			// render rides the same pattern (dock it beside
			// Render/Material or tab it with them).
			if (m_showWorldMapPanel) {
				if (ImGui::Begin("World Map", &m_showWorldMapPanel))
					DrawWorldMapPanel();
				ImGui::End();
			}
			// Register dockable panels in a single place. Panel lifetime and
			// ImGui Begin/End ownership are managed by PanelRegistry.
			// NOTE (#155 merge): "Render Settings" is NOT a registry panel -
			// it lives in the standalone NoDocking "Options" window above
			// (the EVSM consolidation); the branch's registry entry for it
			// is dropped here. The World Map panel (#153) keeps its own
			// Begin/End block above until it migrates to the registry.
			EditorActions uiActions{
				[this](const EditCmd& cmd) { Do(cmd); },
				[this](EditVerb verb, uint32_t node,
					const glm::mat4& before, bool activated) {
					return TrackDrag(verb, node, before, activated);
				},
				[this]() { FinishDrag(); }
			};
			EditorContext uiContext{m_selected, uiActions};
			PanelRegistry panels;
			panels.Register("Outliner", m_showOutlinerPanel,
				[this](EditorContext&) { DrawOutliner(); },
				[this]() {
					ImGui::MenuItem("Show Non-DAG", nullptr, &m_showNonDag);
					ImGui::SetItemTooltip("Show image, material and editor camera rows.");
				});
			panels.Register("Reference", m_showReferencePanel,
				[this](EditorContext&) { DrawReferencePanel(); });
			panels.Register("Assets", m_showAssetsPanel,
				[this](EditorContext&) { DrawAssetsPanel(); });
			panels.Register("Console", m_showConsolePanel,
				[this](EditorContext&) { DrawConsolePanel(); });
			panels.Register("Overrides", m_showOverridesPanel,
				[this](EditorContext&) { DrawOverridesPanel(); });
			panels.Register("Inspector", m_showInspectorPanel,
				[this](EditorContext&) { DrawInspector(); });
			panels.Register("Material", m_showMaterialPanel,
				[this](EditorContext&) { DrawMaterialEditor(); });
			panels.DrawAll(uiContext);

			DrawStatusBar(); // pinned editor chrome, never docked
		} // m_uiMode

		if (m_showComponentTable)
			DrawComponentTable();
		DrawInsetTag();
		DrawViewportChrome(); // borders, camera chips, box-select rect
		// Viewport chrome overlays the image; presentation aspect is 4:3.
		DrawLiveViewStage();
		DrawVerbToast();
		// UV editor window retired (the Live view window replaced the
		// canvas era; solid backgrounds everywhere now — no transparent
		// content regions). See editor_viewports.cpp's DrawUvEditorWindow.
		//DrawUvEditorWindow(); // floating UV editor (View menu; canvas via GPU)
		DrawCookWindow(); // floating cook/bake tools (View menu)
		DrawSelectionRadial(); // Q radial: Mesh/Face/Vertex highlight modes
		DrawReferencesPanel(); // Maya-style reference list (View menu)
		DrawPrefabWindow(); // separate .tour World + source editor
		DrawPathsPanel(); // consolidated file/path view (File menu)
		DrawGiVizWindow(); // GI-authoring legend + probe overlay controls
		// The WYSIWYG view is part of the dockable Viewport window.
		if (m_placement.showGraph)
			DrawPlacementGraph();
		DrawSurfaceBrushPanel();
		if (m_stateGraph.show) {
			m_stateGraph.Draw();
			m_imguiFocusInPlacementGraph |= m_stateGraph.Focused();
		}
		// Frame-time histogram (View menu / F12): a SEPARATE bottom strip —
		// full-width horizontal bucket graph pinned to the bottom of the
		// screen, drawn last so it sits on top of the panels/docks. Zero cost
		// while disabled: one flag check.
		if (smvk::profiler::FrameHistogramEnabled())
			smvk::profiler::DrawFrameHistogramUi();
	}

	// ---- Maya-like bottom status strip (process progress) --------------
	// Always visible in UI mode: left = what is happening (task stage,
	// sticky result, else Ready), right = determinate progress while a
	// task reports fractions, else a reopen button for backgrounded
	// tasks. Derived per frame from live state — no stored message.
	// ---- THE VERB TOAST (the command feedback over the view panels' rect)
	// The verb's friendly name — minimal (the action's word, not a dialog).
	void EditorApp::ShowVerbToast(const EditCmd& c) {
		const char* v = "?";
		switch (c.verb) {
		case EditVerb::Translate: v = "move"; break;
		case EditVerb::Rotate: v = "rotate"; break;
		case EditVerb::Scale: v = "scale"; break;
		case EditVerb::Rename: v = "rename"; break;
		case EditVerb::Parent: v = "reparent"; break;
		case EditVerb::CreateLight: v = "create light"; break;
		case EditVerb::CreateTerrain: v = "create terrain"; break;
		case EditVerb::Delete: v = "delete"; break;
		case EditVerb::AssignMaterial: v = "assign material"; break;
		case EditVerb::SetMatAttr: v = c.attr.empty() ? "material"
				: c.attr.c_str(); break;
		case EditVerb::SetVisibility: v = "visibility"; break;
		case EditVerb::SetLightAttr: v = c.lightAttr.empty() ? "light attr"
				: c.lightAttr.c_str(); break;
		default: break;
		}
		m_verbToast = v;
		m_verbToastAge = 0.0f;
	}

	void EditorApp::DrawVerbToast() {
		if (m_verbToastAge < 0.0f)
			return;
		// ~1.6s lifetime, the last 0.4s fades out.
		constexpr float kToastLife = 1.6f;
		m_verbToastAge += ImGui::GetIO().DeltaTime;
		if (m_verbToastAge >= kToastLife) {
			m_verbToastAge = -1.0f;
			return;
		}
		const float alpha = m_verbToastAge < kToastLife - 0.4f
			? 1.0f
			: (kToastLife - m_verbToastAge) / 0.4f;
		// The chip: centered over the ACTIVE viewport's rect, near the top
		// (above the content, below the chrome).
		const ViewportRect& vp = m_viewports[m_activeVp].rect;
		ImDrawList* dl = ImGui::GetBackgroundDrawList();
		const ImVec2 ts = ImGui::CalcTextSize(m_verbToast.c_str());
		// Scene rect is primary-window-local; ImGui draw lists are global.
		const ImVec2 origin = ImGui::GetMainViewport()->Pos;
		const ImVec2 c(origin.x + vp.x + vp.w * 0.5f,
			origin.y + vp.y + 26.0f);
		const ImVec2 bmin(c.x - ts.x * 0.5f - 10.0f, c.y - 4.0f);
		const ImVec2 bmax(c.x + ts.x * 0.5f + 10.0f, c.y + ts.y + 4.0f);
		dl->AddRectFilled(bmin, bmax,
			IM_COL32(18, 22, 30, int(200 * alpha)), 5.0f);
		dl->AddRect(bmin, bmax,
			IM_COL32(120, 200, 255, int(120 * alpha)), 5.0f);
		dl->AddText(ImVec2(bmin.x + 10.0f, bmin.y + 3.0f),
			IM_COL32(220, 235, 255, int(235 * alpha)),
			m_verbToast.c_str());
	}

	void EditorApp::DrawStatusBar() {
		const float vw = float(ImGui::GetIO().DisplaySize.x);
		const float vh = float(ImGui::GetIO().DisplaySize.y);
		// The bar's own padding: the theme's 10px WindowPadding plus a
		// 24px strip clipped the text into the window's bottom edge (the
		// "collides with the bottom" report) — a tight (6,3) padding and
		// one line of breathing room (kEditorStatusH 28) keeps the text
		// off the edges.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 3.0f));
		const ImVec2 mainPos = ImGui::GetMainViewport()->Pos;
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		ImGui::SetNextWindowPos(ImVec2(mainPos.x, mainPos.y + vh - kEditorStatusH),
			ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(vw, kEditorStatusH),
			ImGuiCond_Always);
		if (!ImGui::Begin("##statusbar", nullptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
				ImGuiWindowFlags_NoScrollbar)) {
			ImGui::End();
			ImGui::PopStyleVar();
			return;
		}
		const bool running = m_cookRunning || m_bakeRunning;
		// THE MODE LEGEND (color-coded, always visible): which editor
		// mode is live + the F-keys that switch — the bottom bar's
		// persistent orientation row (the toolbar's number chips show
		// it too; this is the at-a-glance color code).
		{
			static const char* kModeNames[4] = { "1 LAYOUT", "2 LIGHTING",
				"3 TEST", "4 PROFILE" };
			// The mode color code: layout blue, lighting amber, test
			// green, profile violet (the chips + the legend agree).
			static const ImVec4 kModeColors[4] = {
				ImVec4(0.45f, 0.65f, 1.00f, 1.0f),
				ImVec4(1.00f, 0.75f, 0.30f, 1.0f),
				ImVec4(0.45f, 0.95f, 0.55f, 1.0f),
				ImVec4(0.80f, 0.60f, 1.00f, 1.0f)
			};
			const int mode = std::clamp(int(m_mode), 0, 3);
			ImGui::TextColored(kModeColors[mode], "%s",
				kModeNames[mode]);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Editor mode (keys 1-4 anywhere).\n"
					"Layout: build. Lighting: the look + GI authoring.\n"
					"Test: the run preview. Profile: capture + stats.");
			ImGui::SameLine(0.0f, 12.0f);
		}
		// Reserve a right-side task/progress lane before drawing status text.
		// Long errors are clipped with a tooltip rather than pushing
		// interactive controls beyond the status bar window.
		const bool hasTask = m_taskWindow.wantOpen;
		const bool activeTask = running || m_dccPublishRunning;
		const float available = ImGui::GetContentRegionAvail().x;
		const float laneW = activeTask
			? std::min(250.0f, std::max(90.0f, available * 0.45f))
			: (hasTask ? std::min(54.0f, available * 0.25f) : 0.0f);
		std::string status = activeTask
			? (!m_taskWindow.stage.empty() ? m_taskWindow.stage
				: "Working...")
			: (m_taskWindow.done ? m_taskWindow.result : "Ready");
		if (!m_verbToast.empty())
			status += "  |  Last: " + m_verbToast;
		if (!m_consoleLines.empty())
			status += "  |  Console: " + m_consoleLines.back();
		const std::string fullStatus = status;
		const float textW = std::max(0.0f, available - laneW - 8.0f);
		if (ImGui::CalcTextSize(status.c_str()).x > textW) {
			const std::string ellipsis = "...";
			size_t lo = 0, hi = status.size();
			while (lo < hi) {
				const size_t mid = (lo + hi + 1) / 2;
				const std::string trial = status.substr(0, mid) + ellipsis;
				if (ImGui::CalcTextSize(trial.c_str()).x <= textW)
					lo = mid;
				else
					hi = mid - 1;
			}
			status = lo ? status.substr(0, lo) + ellipsis : "";
		}
		ImGui::BeginChild("##status_text", ImVec2(textW, ImGui::GetFrameHeight()),
			false, ImGuiWindowFlags_NoScrollbar
				| ImGuiWindowFlags_NoScrollWithMouse);
		if (m_taskWindow.done && !activeTask)
			ImGui::TextColored(m_taskWindow.ok
				? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
				: ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
				"%s", status.c_str());
		else
			ImGui::TextUnformatted(status.c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", fullStatus.c_str());
		ImGui::EndChild();
		if (activeTask || hasTask) {
			ImGui::SameLine();
			if (activeTask) {
				const float progressW = std::max(34.0f,
					laneW - (hasTask ? 52.0f : 0.0f));
				if (running && m_taskWindow.progress >= 0.0f)
					ImGui::ProgressBar(m_taskWindow.progress,
						ImVec2(progressW, 0), "");
				else
					ImGui::ProgressBar(-1.0f,
						ImVec2(progressW, 0), "...");
				if (hasTask)
					ImGui::SameLine();
			}
			if (hasTask && ImGui::SmallButton("Task"))
				ReopenTaskWindow();
			if (hasTask && ImGui::IsItemHovered())
				ImGui::SetTooltip("Reopen the operation log and result.");
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	void EditorApp::DrawImportWindow() {
		if (!m_showImportWindow)
			return;
		ImGui::SetNextWindowSize(ImVec2(630.0f, 300.0f),
			ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Import Asset", &m_showImportWindow)) {
			ImGui::TextUnformatted("Source file");
			ImGui::SetNextItemWidth(-100.0f);
			ImGui::InputText("##import_source", m_importSourceBuf,
				sizeof(m_importSourceBuf));
			ImGui::SameLine();
			if (ImGui::Button("Browse...")) {
				std::string picked;
				if (NativeOpenFile(picked) && !picked.empty())
					std::snprintf(m_importSourceBuf, sizeof(m_importSourceBuf),
						"%s", picked.c_str());
			}
			std::string ext = std::filesystem::path(m_importSourceBuf)
				.extension().string();
			for (char& c : ext)
				c = char(std::tolower(static_cast<unsigned char>(c)));
			const bool supported = ext == ".gltf" || ext == ".glb"
				|| ext == ".obj";
			ImGui::SeparatorText("Import settings");
			if (ext == ".gltf")
				ImGui::TextWrapped("glTF: copies external buffers, images and "
					"source dependencies into the project, then derives geometry, "
					"materials and animations with persistent asset GUIDs.");
			else if (ext == ".glb")
				ImGui::TextWrapped("GLB: imports the binary model and its "
					"embedded resources, deriving the runtime asset set.");
			else if (ext == ".obj")
				ImGui::TextWrapped("OBJ: imports geometry and linked MTL "
					"dependencies; no skeletal animation is expected.");
			else
				ImGui::TextDisabled("Choose a supported model: .gltf, .glb, .obj.");
			ImGui::Checkbox("Open imported model as active scene",
				&m_importOpenAfter);
			ImGui::TextDisabled("Unchecked: import into Assets without "
					"replacing the current scene.");
			ImGui::BeginDisabled(!supported || m_projectRoot.empty()
				|| m_cookRunning);
			if (ImGui::Button("Import")) {
				TaskWindowBegin("Import asset", "Importing source model...");
				TaskWindowStage("Copying dependencies and deriving assets...", 0.05f);
				const bool success = ImportAsset(m_importSourceBuf,
					m_importOpenAfter);
				TaskWindowFinish(success,
					success ? "Import complete."
						: "Import failed; see Console for details.");
				if (success)
					m_showImportWindow = false;
			}
			ImGui::EndDisabled();
			if (m_projectRoot.empty())
				ImGui::TextDisabled("Open a project before importing.");
		}
		ImGui::End();
	}

	void EditorApp::DrawMenuBar() {
		if (ImGui::BeginMenu("File")) {
			// Project first (Unity/Unreal style): a project folder owns
			// content/, paks/, gi/ and its own .tourmaline/ cache. Imports
			// land in it and the editor shows them
			// pipeline — no manual staging.
			if (ImGui::MenuItem("New / Open Project...", nullptr, false,
					!m_cookRunning && !m_bakeRunning)) {
				std::string picked;
				if (NativePickFolder(picked) && !picked.empty())
					NewProjectOrOpen(picked);
			}
			if (!m_projectRoot.empty()) {
                if (ImGui::MenuItem("New scene...", nullptr, false,
                    !m_cookRunning))
                    CreateSceneAsset((std::filesystem::path(m_projectRoot) /
                        "content" / "scenes").generic_string());
                if (ImGui::MenuItem("Import asset...", nullptr, false,
					!m_cookRunning))
					m_showImportWindow = true;
				ImGui::TextDisabled("project: %s", m_projectName.c_str());
				ImGui::Separator();
			}
			// Single entry point: native dialog (.gltf/.glb/.tour, in C:/),
			// dispatched by extension (tours replay into the stack; paks are
			// refused — they run in the App).
			// The source history agent is optional and entirely local. This
			// command only opens its browser UI; the editor remains usable
			// if the offline service is not installed or running.
			if (ImGui::MenuItem("Browse source asset history...")) {
				constexpr const char* kHistoryUrl = "http://127.0.0.1:8765/";
				if (!SDL_OpenURL(kHistoryUrl)) {
					ImGui::SetClipboardText(kHistoryUrl);
					Log("Asset history: could not open browser; URL copied to clipboard.");
				}
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Open file...", nullptr, false,
					!m_cookRunning && !m_bakeRunning)) {
				std::string picked;
				if (NativeOpenFile(picked) && !picked.empty())
					OpenAnySceneFile(picked);
			}
			if (ImGui::MenuItem("Add scene reference...", nullptr, false,
				m_world != nullptr && !m_cookRunning && !m_bakeRunning)) {
				// File browser pick (.gltf/.tour; the name defaults to the
				// path stem, the reference binds into its own namespace).
				std::string picked;
				if (NativeOpenFile(picked) && !picked.empty())
					AddSceneReference(picked, {});
			}
			if (!m_fullPrefabParentSnapshot.empty()) {
                ImGui::Separator();
                if (ImGui::MenuItem("Save prefab && return to scene"))
                    RequestReturnFromPrefab(true);
                if (ImGui::MenuItem("Discard prefab edits && return"))
                    RequestReturnFromPrefab(false);
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Save tour...", nullptr, false,
					m_world != nullptr && !m_cookRunning && !m_bakeRunning)) {
				// The startup document is transient; an explicit Save tour
				// should offer a permanent project asset, not its temp path.
				const std::string saveDefault =
					!m_startupTempTour.empty()
					&& TourTarget() == m_startupTempTour
					? (std::filesystem::path(m_projectRoot) / "content"
						/ "scenes" / "New Scene.tour").string()
					: TourTarget();
				std::snprintf(m_savePathBuf, sizeof(m_savePathBuf),
					"%s", saveDefault.c_str());
#ifdef _WIN32
				std::string target;
				if (NativeSaveTourFile(target, m_savePathBuf) && !target.empty()) {
					if (SaveTourTo(target))
						Log("Tour saved: " + target);
				}
#else
				m_showSaveDialog = true;
#endif
			}
			if (ImGui::MenuItem("Save tour incremental", "Ctrl+Alt+S",
					false, m_world != nullptr && !m_cookRunning && !m_bakeRunning))
				ModalSaveIncremental();
			if (ImGui::MenuItem("Export Project...", "Ctrl+Shift+C", false,
					!m_projectRoot.empty() && !m_cookRunning && !m_bakeRunning))
				ModalExportProject();
			ImGui::Separator();
			if (ImGui::MenuItem("Close scene", nullptr, false,
					m_world != nullptr && !m_cookRunning && !m_bakeRunning))
				CloseScene();
			ImGui::Separator();
			if (ImGui::MenuItem("Quit"))
				RequestClose();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Create")) {
			// Editor-authored punctual lights (point/spot): appended as world
			// nodes with pool definitions (undoable), persisted as
			// createLight/setLightAttr tour verbs so cooks replay them.
			const bool canCreate = m_world != nullptr;
			if (ImGui::MenuItem("Point light", nullptr, false, canCreate))
				DraftCreateLight(false);
			if (ImGui::MenuItem("Spot light", nullptr, false, canCreate))
				DraftCreateLight(true);
			if (ImGui::MenuItem("Spatial Volume", nullptr, false, canCreate))
				DraftCreateSpatialVolume();
			if (ImGui::MenuItem("Draw Spatial Volume Box", nullptr,
				m_volumeBrushArmed, canCreate)) {
				m_volumeBrushArmed = !m_volumeBrushArmed;
				m_volumeBrushDragging = false;
				m_volumeBrushStage = 0;
			}
			if (ImGui::MenuItem("Convex volume (GI)", nullptr, false,
				canCreate))
				DraftCreateGiVolume();
			if (ImGui::MenuItem("Cubemap capture (pinned)", nullptr, false,
				canCreate))
				DraftCreateCubemap();
			if (ImGui::MenuItem("Camera", nullptr, false, canCreate))
				DraftCreateCamera();
			if (ImGui::MenuItem("Audio source", nullptr, false, canCreate)) {
				// An emitter association (the AudioSource node type): a
				// plain transform + the node-local SceneAudioSource
				// payload (volume/spatial distances + sound slots —
				// asset refs, never playing voices). Persisted as
				// createAudioSource + tabulated setAudioAttr/
				// addAudioSound lines; the emitter POSE resolves from
				// the node's global transform at use.
				DraftCreateAudioSource();
			}
			if (ImGui::MenuItem("Audio room", nullptr, false, canCreate)) {
				// An environmental-reverb volume (docs/audio_graph.md):
				// a transform whose box is the room (the givolume
				// placement convention) + the node-local SceneAudioRoom
				// payload. Persisted as createNode -at audioRoom +
				// tabulated setAudioRoomAttr lines; a GI volume may
				// ALSO carry a room (one placement, two associations —
				// the inspector's Enable button attaches it).
				DraftCreateAudioRoom();
			}
			if (ImGui::MenuItem("Locator", nullptr, false, canCreate)) {
				// A named bare transform (no mesh/light/pool) — a game
				// code anchor: spawn points, trigger zones, patrol
				// markers. Tour: createNode -at transform; the runtime
				// finds it by NAME (findNodeByName).
				DraftCreateLocator();
			}
			if (ImGui::MenuItem("Terrain...", nullptr, false, canCreate)) {
				// Seed the dialog defaults (name, material) once per open.
				std::snprintf(m_terrainNameBuf, sizeof(m_terrainNameBuf), "%s",
					UniqueTerrainName().c_str());
				m_terrainMat = UINT32_MAX;
				m_showTerrainDialog = true;
			}
			if (ImGui::MenuItem("Surface brush (points / spline)", nullptr,
				m_surfaceBrushEnabled, canCreate)) {
				if (m_surfaceBrushEnabled) SurfaceBrushFinish();
				m_surfaceBrushEnabled = !m_surfaceBrushEnabled;
				m_surfaceDrawing = false;
				if (m_surfaceBrushEnabled) {
					m_volumeBrushArmed = false;
					m_paintTerrain = false;
				}
			}
			if (ImGui::MenuItem("Placement graph", nullptr, false,
					canCreate)) {
				// (The toolbar's old "New graph" button — creation belongs
				// here, the bar keeps the live overlays/actions.)
				m_placement.pushUndo();
				m_placement.addGraph("");
				MarkPlacementDirty();
			}
			if (ImGui::MenuItem("Image...", nullptr, false, canCreate)) {
				// Maya file node: an image definition (path + sampling
				// settings) held OUTSIDE the DAG — editor-only state until
				// a material uses it.
				std::string picked;
				if (NativeOpenFile(picked, true) && !picked.empty())
					AddImageDef(picked);
			}
			// NOTE: cook assets (.gicook/.lmcook) are intentionally NOT
			// menu-creatable: they are an internal consequence of baking
			// (RecordBakeAssetsFromCook mints/updates them per scene).
			if (!canCreate)
				ImGui::TextDisabled("open a scene first");
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit")) {
			const std::string ul = UndoLabel();
			const std::string rl = RedoLabel();
			if (ImGui::MenuItem(("Undo " + ul).c_str(), "Ctrl+Z", false,
				!ul.empty()))
				Undo();
			if (ImGui::MenuItem(("Redo " + rl).c_str(), "Ctrl+Shift+Z", false,
				!rl.empty()))
				Redo();
			ImGui::Separator();
			// Del-key twin (modal registry 'Del'): deletes the editor-created
			// content in the selection (created lights, terrain groups);
			// imports/references are refused with a log (ids must stay
			// stable — hide those instead).
			if (ImGui::MenuItem("Delete selection", "Del", false,
				m_world && (m_selected != UINT32_MAX
					|| !m_selection.empty()) && !m_cookRunning))
				ModalDelete();
			// Ctrl+D twin (modal registry 'Ctrl+D'): fresh Creates seeded
			// from the LIVE state of every selected duplicable node
			// (lights/cameras/cubemaps/GI volumes/transforms under the same
			// parent); imports/terrain are refused with a log.
			if (ImGui::MenuItem("Duplicate selection", "Ctrl+D", false,
				m_world && (m_selected != UINT32_MAX
					|| !m_selection.empty()) && !m_cookRunning))
				ModalDuplicate();
			if (ImGui::MenuItem("Hide / unhide selection", "Ctrl+H", false,
					m_world && !m_selection.empty() && !m_cookRunning))
				ModalToggleVisibility();
			ImGui::Separator();
			// Collapse the stack to the current state: superseded per-node /
			// per-attr edits drop (their LAST occurrence already carries the
			// live state), the redo tail clears, the .tour lightens. Creates
			// and terrain paint stamps stay (structure / ordered bake). NOT
			// undoable — same contract as Maya's delete-history.
			if (ImGui::MenuItem(
				("Coalesce history (" + std::to_string(UndoDepth())
					+ " cmds)").c_str(),
				nullptr, false, UndoDepth() > 1)) {
				const size_t dropped = CoalesceHistory();
				Log("Coalesced history: " + std::to_string(UndoDepth())
					+ " commands kept, " + std::to_string(dropped)
					+ " dropped (redo tail cleared).");
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Keeps only the last edit per node/attr "
					"channel — the surviving state — so the tour replays "
					"identically from fewer commands.");
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Geometry")) {
			if (ImGui::MenuItem("Intersection curves / seam bake..."))
				m_showIntersectionToolsPanel = true;
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Mode")) {
			// vi-style modal operation: the ACTIVE mode's keymap interprets
			// keys (editor_modal.cpp); 1-4 switch modes from any mode.
			const struct {
				EditorMode m; const char* key; const char* hint;
			} modes[] = {
				{ EditorMode::Layout, "1", "object editing: gizmos, selection" },
				{ EditorMode::Lighting, "2", "sun/env + lightmap views" },
				{ EditorMode::Test, "3", "cook, run in App, debug views" },
				{ EditorMode::Profile, "4", "perf (stub)" },
			};
			for (const auto& e : modes)
				if (ImGui::MenuItem(EditorModeName(e.m), e.key, m_mode == e.m))
					SetMode(e.m);
			ImGui::Separator();
			ImGui::TextDisabled("Modal (vi-style): the active mode interprets");
			ImGui::TextDisabled("keys; the viewport legend shows the mode line.");
			ImGui::Separator();
#if defined(TOURMALINE_EDITOR_GAME_LINKED)
			if (ImGui::MenuItem("Run linked game code", nullptr, false,
					m_world && !m_gameSession.Active()))
				m_gamePlayRequest = true;
			if (ImGui::MenuItem("Stop linked game code", nullptr, false,
					m_gameSession.Active()))
				m_gamePlayRequest = false;
#else
			ImGui::TextDisabled("Game entry: not statically linked");
#endif
			ImGui::EndMenu();
		}
		// DCC actions belong in the application menu, not the Assets grid.
		// A multi-selection must never silently operate on just its last item.
		if (ImGui::BeginMenu("DCC")) {
			const bool oneAsset = m_assetsSelSet.size() == 1
				&& !m_assetsSel.empty()
				&& m_assetsSelSet.front() == m_assetsSel
				&& std::filesystem::is_regular_file(m_assetsSel);
			if (oneAsset)
				ImGui::TextDisabled("%s", std::filesystem::path(m_assetsSel)
					.filename().string().c_str());
			else
				ImGui::TextDisabled("Select exactly one file in Assets");
			ImGui::Separator();
			if (ImGui::MenuItem("Open in DCC", nullptr, false, oneAsset))
				OpenAssetInDcc(m_assetsSel);
			if (ImGui::MenuItem("Link DCC source...", nullptr, false, oneAsset))
				LinkAssetToDcc(m_assetsSel);
			if (ImGui::MenuItem("Publish glTF", nullptr, false,
					oneAsset && !m_dccPublishRunning))
				PublishAssetFromDcc(m_assetsSel);
			if (ImGui::MenuItem("Unlink DCC source", nullptr, false, oneAsset))
				UnlinkAssetFromDcc(m_assetsSel);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Window")) {
			ImGui::MenuItem("Import Asset", nullptr, &m_showImportWindow);
			ImGui::MenuItem("Viewport", nullptr, &m_showViewportPanel);
			ImGui::MenuItem("World Map", nullptr, &m_showWorldMapPanel);
			ImGui::MenuItem("Outliner", nullptr, &m_showOutlinerPanel);
			ImGui::MenuItem("Reference", nullptr, &m_showReferencePanel);
			ImGui::MenuItem("Assets", nullptr, &m_showAssetsPanel);
			ImGui::MenuItem("Console", nullptr, &m_showConsolePanel);
			ImGui::MenuItem("Overrides", nullptr, &m_showOverridesPanel);
			ImGui::MenuItem("Inspector", nullptr, &m_showInspectorPanel);
			ImGui::MenuItem("Material", nullptr, &m_showMaterialPanel);
			ImGui::Separator();
			if (ImGui::MenuItem("Reset dock layout")) {
				m_dockResetRequested = true;
				m_showViewportPanel = true;
				m_showWorldMapPanel = true;
				m_showOutlinerPanel = true;
				m_showReferencePanel = true;
				m_showAssetsPanel = true;
				m_showConsolePanel = true;
				m_showOverridesPanel = true;
				m_showInspectorPanel = true;
				m_showMaterialPanel = true;
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View")) {
			ImGui::TextDisabled("Debug views: F1..F6, F8..F12. P parents"
				" the selection; Z toggles wireframe in Layout.");
			// Shading: the GI tri-view (the bake workflow's three looks),
			// ONE block — the old F1/F2 pair ("Unlit"/"Lit") duplicated
			// these (same ViewMode targets; the tri-view adds the ambient
			// split below). Unlit (albedo) = pure albedo; Fully Lit
			// renders the baked GI sources (world field + convex volumes
			// + cubemaps); Realtime Lit = the analytic look (direct sun +
			// sky IBL, no bake). The latter two both ride the Lit frame;
			// the difference is the ambient source (SetAmbientMode All vs
			// SkyIbl, driven every frame with the debug views).
			if (ImGui::MenuItem("Unlit (albedo)", "F2",
					ActiveViewMode() == ViewMode::Unlit))
				ActiveViewMode() = ViewMode::Unlit;
			if (ImGui::MenuItem("Fully Lit (with GI)", "F1",
					ActiveViewMode() == ViewMode::Lit && m_litView == 0))
				ModalViewLit();
			if (ImGui::MenuItem("Realtime Lit", nullptr,
					ActiveViewMode() == ViewMode::Lit && m_litView == 1)) {
				ActiveViewMode() = ViewMode::Lit;
				m_litView = 1;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Meshlets", "F3",
				ActiveViewMode() == ViewMode::Meshlets))
				ModalViewMeshlets();
			if (ImGui::MenuItem("Shadow cascade coverage", "F4",
				ActiveViewMode() == ViewMode::Cascades))
				ActiveViewMode() = ViewMode::Cascades;
			if (ImGui::MenuItem("LODs", "F5",
				ActiveViewMode() == ViewMode::Lods))
				ActiveViewMode() = ViewMode::Lods;
			if (ImGui::MenuItem("Vertex colors", "F6",
				ActiveViewMode() == ViewMode::VertexColors))
				ActiveViewMode() = ViewMode::VertexColors;
			if (ImGui::MenuItem("Parallax (POM)", "F11",
				ActiveViewMode() == ViewMode::Pom))
				ActiveViewMode() = ViewMode::Pom;
			if (ImGui::MenuItem("Baked irradiance", "F8",
				ActiveViewMode() == ViewMode::Baked, m_world != nullptr))
				ModalViewBaked();

			// The retained engine visualizers inspect the actual deferred-
			// shadow tile lists (which also consume Forward+ lights). Their
			// GPU reads are available only in DS_DEBUG_VIZ builds, and only
			// the Live Lit viewport runs the full renderer.
#if defined(DS_DEBUG_VIZ)
			if (ImGui::BeginMenu("Light tile diagnostics (Live Lit)")) {
				const char* modes[] = {
					"Off",
					"Approximate count (phase 2)",
					"Accurate count (phase 3)",
					"Final light count (phase 4)",
					"Overestimate (approx - accurate)",
					"Cull stage (selected light)",
					"List membership (selected light)",
					"Visibility (selected light)",
				};
				int mode = int(m_dsTileVizMode);
				if (ImGui::Combo("Tile visualization", &mode, modes, 8)) {
					m_dsTileVizMode = engine::DsDebugViz(mode);
					if (mode != 0) {
						ActiveViewMode() = ViewMode::Lit;
						m_litView = 0;
					}
				}
				if (mode != 0) {
					ImGui::Checkbox("Overlay on lit", &m_dsTileVizOverlay);
					if (mode >= 1 && mode <= 4)
						ImGui::SliderInt("Count heat ceiling",
							&m_dsTileVizCountCeil, 1,
							int(smvk::kDsMaxLightsPerTile));
					if (mode >= 5)
						ImGui::InputInt("Sorted light index", &m_dsTileVizLight);
					m_dsTileVizLight = std::max(0, m_dsTileVizLight);
					ImGui::TextDisabled("Requires deferred shadows; Live Lit viewport.");
					ImGui::TextDisabled("Light index is phase-1 sorted, not outliner order.");
					ImGui::TextDisabled("Diagnostic readout, not measured GPU time.");
				}
				ImGui::EndMenu();
			}
#else
			if (ImGui::BeginMenu("Light tile diagnostics (unavailable)")) {
				ImGui::TextDisabled("Build with -DTOURMALINE_DS_VIZ=ON");
				ImGui::TextDisabled("and compile the corresponding shaders.");
				ImGui::EndMenu();
			}
#endif
			if (ActiveViewMode() == ViewMode::Meshlets) {
				const char* meshletModes[] = { "Random per meshlet",
					"Cone-cutoff heat" };
				ImGui::Combo("Meshlet color mode", &m_debugMeshletMode,
					meshletModes, 2);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Isolate Selected", "Ctrl+1",
					m_isolateVisibility.active(), m_world != nullptr))
				ModalIsolateSelect();
			ImGui::SetItemTooltip("Temporarily show only selected nodes and "
				"their descendants. Ctrl+1 again (or with no selection) "
				"restores authored visibility; not saved to the scene.");
			if (ImGui::MenuItem("Wireframe (hidden-line)", "Z (Layout)",
					m_wireframeAll))
				ModalWireframeToggle();
			ImGui::SetItemTooltip("Line polygon mode over the Fill depth"
				"prepass — lines never draw over the objects in front.");
			ImGui::Separator();
			// Frame-time histogram (F12 — the kModalBindings GLOBAL row; this
			// menu item calls the SAME handler, so key + menu stay one
			// binding): the shared profiler state + a separate full-width
			// bottom strip drawn at the end of DrawUI. Zero cost while
			// disabled.
			if (ImGui::MenuItem("Frame-time histogram", "F12",
				smvk::profiler::FrameHistogramEnabled()))
				ModalFrameHistogramToggle();
			ImGui::SetItemTooltip("Live frame-time distribution (last 1024"
				" frames): a full-width horizontal bucket graph pinned to the"
				" bottom of the viewport, with avg/p50/p95/p99/max.");
			ImGui::Separator();
			if (ImGui::MenuItem("Single viewport", "V", m_vpLayout == 0)
					&& m_vpLayout != 0)
				ModalVpLayoutToggle();
			if (ImGui::MenuItem("Quad viewports (4 panels)", "V",
					m_vpLayout == 1) && m_vpLayout != 1)
				ModalVpLayoutToggle();
			if (ImGui::BeginMenu("Panels")) {
				ImGui::MenuItem("Viewport", nullptr, &m_showViewportPanel);
				ImGui::MenuItem("World Map", nullptr, &m_showWorldMapPanel);
				ImGui::MenuItem("Outliner", nullptr, &m_showOutlinerPanel);
				ImGui::MenuItem("Reference", nullptr, &m_showReferencePanel);
				ImGui::MenuItem("Assets", nullptr, &m_showAssetsPanel);
				ImGui::MenuItem("Console", nullptr, &m_showConsolePanel);
				ImGui::MenuItem("Overrides", nullptr, &m_showOverridesPanel);
				ImGui::MenuItem("Inspector", nullptr, &m_showInspectorPanel);
				ImGui::MenuItem("Material", nullptr, &m_showMaterialPanel);
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Reset dock layout")) {
				m_dockResetRequested = true;
				// A reset restores the default workspace, including closed tools.
				m_showViewportPanel = true;
				m_showWorldMapPanel = true;
				m_showOutlinerPanel = true;
				m_showAssetsPanel = true;
				m_showConsolePanel = true;
				m_showOverridesPanel = true;
				m_showInspectorPanel = true;
				m_showMaterialPanel = true;
			}
			ImGui::SetItemTooltip("Restore the default Viewport, Outliner, "
				"Assets, Console, Overrides, Inspector and Material positions.");
			ImGui::Separator();
			ImGui::MenuItem("Ground grid", nullptr, &m_showGrid);
			{
				float cell = m_gridSize;
				if (ImGui::DragFloat("Grid cell (m)", &cell, 0.05f, 0.05f, 50.0f))
					m_gridSize = std::max(cell, 0.05f);
				ImGui::SetItemTooltip("Also the Shift-drag translate snap step.");
				int ext = int(m_gridExtent);
				if (ImGui::DragInt("Grid half-extent (cells)", &ext, 1, 2, 512))
					m_gridExtent = uint32_t(std::clamp(ext, 2, 512));
			}
			ImGui::Separator();
			{
				float gs = m_gizmoScale;
				if (ImGui::SliderFloat("Gizmo size", &gs, 0.02f, 0.3f, "%.3f"))
					m_gizmoScale = std::clamp(gs, 0.02f, 0.3f);
				ImGui::SetItemTooltip(
					"Screen-size factor of the camera distance (default 0.07, "
					"half the original 0.14).");
				ImGui::DragFloat("Rotate snap (deg)", &m_snapRotateDeg,
					1.0f, 1.0f, 90.0f);
				ImGui::DragFloat("Scale snap", &m_snapScaleStep, 0.01f, 0.01f, 1.0f);
				ImGui::TextDisabled("Shift while dragging = snap "
					"(translate: grid cell)");
			}
			ImGui::Separator();
			if (ImGui::MenuItem("UV0", "F9", ActiveViewMode() == ViewMode::Uv0))
				ActiveViewMode() = ViewMode::Uv0;
			if (ImGui::MenuItem("UV1 (lightmap)", "F10", ActiveViewMode() == ViewMode::Uv1))
				ActiveViewMode() = ViewMode::Uv1;
			ImGui::Separator();
			if (ImGui::MenuItem(
				("UV inset: " + std::string(SecondViewName(m_secondView))).c_str(),
				"Shift+F5 (LIGHTING)"))
				ModalUvInsetCycle();
			ImGui::Separator();
			if (ImGui::MenuItem("Translate gizmo", "W (Layout/Lighting)",
				m_gizmoMode == GizmoMode::Translate))
				m_gizmoMode = GizmoMode::Translate;
			if (ImGui::MenuItem("Rotate gizmo", "E (Layout/Lighting)",
				m_gizmoMode == GizmoMode::Rotate))
				m_gizmoMode = GizmoMode::Rotate;
			if (ImGui::MenuItem("Scale gizmo", "R (Layout/Lighting)",
				m_gizmoMode == GizmoMode::Scale))
				m_gizmoMode = GizmoMode::Scale;
			ImGui::Separator();
			if (ImGui::MenuItem("Classic Transpose line (object only)", nullptr,
				m_transposeEnabled) && !m_transposeDrag) {
				m_transposeEnabled = !m_transposeEnabled;
				m_transposeOwner = UINT32_MAX;
			}
			if (m_transposeEnabled && ImGui::MenuItem("Reset Transpose line") && !m_transposeDrag)
				m_transposeOwner = UINT32_MAX;
			ImGui::Separator();
			if (ImGui::MenuItem("Selection radial...", "Q"))
				m_showSelRadial = true;
			if (ImGui::MenuItem("Select: Mesh", "Q > 1",
				m_selMode == SelMode::Mesh)) {
				m_selMode = SelMode::Mesh;
				ClearSubSelection();
			}
			if (ImGui::MenuItem("Select: Face", "Q > 2",
				m_selMode == SelMode::Face)) {
				m_selMode = SelMode::Face;
				ClearSubSelection();
			}
			if (ImGui::MenuItem("Select: Vertex", "Q > 3",
				m_selMode == SelMode::Vertex)) {
				m_selMode = SelMode::Vertex;
				ClearSubSelection();
			}
			ImGui::TextDisabled("mode: %s (highlight only)", SelModeName(m_selMode));
			ImGui::Separator();
			if (ImGui::MenuItem("Frame selection", "F (Layout/Lighting)"))
				FrameSelection();
			if (ImGui::MenuItem("Frame all", "Shift+F (Layout/Lighting)"))
				FrameAll();
			// The old floating UV editor is retired; UV0/UV1 use the
			// current viewport modes and the integrated inset.
			ImGui::Separator();
			if (ImGui::MenuItem("Placement graph", nullptr, m_placement.showGraph))
				m_placement.showGraph = !m_placement.showGraph;
			if (ImGui::MenuItem("State machine graph...", nullptr, m_stateGraph.show))
				m_stateGraph.show = !m_stateGraph.show;
			ImGui::MenuItem("Component Spreadsheet...", nullptr, &m_showComponentTable);
			if (ImGui::MenuItem("Prefab World...", nullptr, m_showPrefabWindow)) {
				if (m_showPrefabWindow && m_prefab.dirty)
					m_prefab.error = "Save or Revert unsaved prefab edits before closing.";
				else
					m_showPrefabWindow = !m_showPrefabWindow;
			}
			if (ImGui::MenuItem("Reference editor...", nullptr,
				m_showReferencesPanel))
				m_showReferencesPanel = !m_showReferencesPanel;
			if (ImGui::MenuItem("Path editor...", nullptr,
				m_showPathsPanel))
				m_showPathsPanel = !m_showPathsPanel;
		ImGui::Separator();
		ImGui::MenuItem("Selection wire overlay", nullptr, &m_wireOverlay);
		if (ImGui::MenuItem("Light gizmos", "L", m_showLightGizmos))
			m_showLightGizmos = !m_showLightGizmos;
		if (ImGui::MenuItem("GI volume boxes", nullptr, m_showGiVolumeGizmos))
			m_showGiVolumeGizmos = !m_showGiVolumeGizmos;
		if (ImGui::MenuItem("Audio room boxes", nullptr,
			m_showAudioRoomGizmos))
			m_showAudioRoomGizmos = !m_showAudioRoomGizmos;
		if (ImGui::MenuItem("Cubemap capture markers", nullptr,
			m_showCubemapGizmos))
			m_showCubemapGizmos = !m_showCubemapGizmos;
		// GI VISUALIZATION CLUSTER — ONE entry: the probe-viz window
		// owns every probe/density toggle (overlay on/off, octahedra
		// markers, rings, labels, density previews, sizes, distances).
		// The individual items used to live here too — that was the
		// duplicated-entries state; the window is the single control
		// surface for this cluster now.
		if (ImGui::MenuItem("GI probe viz...", nullptr,
			m_showGiVizWindow))
			m_showGiVizWindow = !m_showGiVizWindow;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Legend + controls: the probe overlay,\n"
				"octahedra markers, labels, density previews,\n"
				"sizes/distances, and the probe counts (in level /\n"
				"in view / per octree level) — LIGHTING only.");
		// (The WYSIWYG live view needs NO toggle anymore: it IS the Lit
		// view mode (F1) — the active viewport in Lit renders through the
		// engine's full pipeline. Every other mode (F2 Unlit, F3..) is
		// the editor's own approximation pass, exactly as before.)
		if (ImGui::MenuItem("Camera frustums", nullptr,
				m_showCameraGizmos))
			m_showCameraGizmos = !m_showCameraGizmos;
		ImGui::Separator();
			{
				// Convex GI volume preview (LIGHTING 'J'): off/wire/
				// solid/probes/lattice (the probe mode draws the dense
				// bake lattice — coverage: green = baked nonzero, red =
				// zero/invalid; the lattice mode draws the AUTHORED
				// defs' would-be grids as instanced 3D spheres —
				// location/density BEFORE any bake).
				const char* cvNames[5] = { "Off", "Wireframe", "Solid",
					"Probes", "Lattice (pre-bake)" };
				if (ImGui::BeginCombo("GI volumes (J)",
					cvNames[m_giVolumePreviewMode])) {
					for (int i = 0; i < 5; ++i)
						if (ImGui::Selectable(cvNames[i], m_giVolumePreviewMode == i))
							m_giVolumePreviewMode = i;
					ImGui::EndCombo();
				}
			}
			if (ImGui::MenuItem("Workspace panels", "I", m_uiMode))
				ModalUiToggle();
			// UV editor window retired (see the DrawUvEditorWindow note).
			//if (ImGui::MenuItem("UV editor window", nullptr, m_showUvEditor))
			//	m_showUvEditor = !m_showUvEditor;
			if (ImGui::MenuItem("Cook & bake window", nullptr,
					m_showCookWindow))
				m_showCookWindow = !m_showCookWindow;
			ImGui::EndMenu();
		}


		if (ImGui::BeginMenu("Options")) {
			if (ImGui::MenuItem("Open Options...", nullptr, m_showRenderSettingsPanel))
				m_showRenderSettingsPanel = true;
			ImGui::EndMenu();
		}

		if (m_showSaveDialog) {
			ImGui::OpenPopup("Save tour as");
			m_showSaveDialog = false;
		}
		DrawSaveDialog();
		if (m_showRecoverDialog) {
			ImGui::OpenPopup("Recover autosave");
			m_showRecoverDialog = false;
			m_recoverArmed = true;
		}
		DrawRecoverDialog();
		if (m_showTerrainDialog)
			ImGui::OpenPopup("Create Terrain");
		// m_showTerrainDialog stays true while the modal is up (the dialog
		// clears it on Create/Cancel); OpenPopup is idempotent.
		DrawCreateTerrainDialog();
		// Blocking task window (cook / bake / publish progress): drawn last
		// so its modal sits on top of every other popup.
		DrawTaskWindow();
		// (The startup project dialog is RETIRED with the editor/launcher
		// split: EditorLauncher owns project selection; the editor boots
		// straight into the --project pick.)
	}

	// Compact, icon-led authoring toolbar. Tool actions reuse the editor's
	// existing selection/gizmo/mode commands, preserving keyboard shortcuts.
	float EditorApp::DrawToolTabs(float top) {
		constexpr float h = 58.0f;
		const ImVec2 mainPos = ImGui::GetMainViewport()->Pos;
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		ImGui::SetNextWindowPos(ImVec2(mainPos.x, mainPos.y + top), ImGuiCond_Always);
		ImGui::SetNextWindowSize(
			ImVec2(float(ImGui::GetIO().DisplaySize.x), h), ImGuiCond_Always);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 4.0f));
		if (!ImGui::Begin("##toptoolbar", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav)) {
			ImGui::End();
			ImGui::PopStyleVar();
			return 0.0f;
		}
		// Each caption belongs to its own button hitbox, not a detached
		// keyboard-legend row. This also keeps tooltips on the whole tool.
		const auto tool = [&](const char* id, IconType icon,
			bool active, const char* caption, const char* tooltip) {
			ImGui::PushID(id);
			const ImVec2 size(49.0f, 46.0f);
			const bool pressed = ImGui::InvisibleButton("##tool", size);
			const bool hover = ImGui::IsItemHovered();
			const ImVec2 p = ImGui::GetItemRectMin();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			if (active || hover) {
				dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
					active ? IM_COL32(111, 71, 33, 255)
					       : IM_COL32(67, 70, 72, 255), 3.0f);
				dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y),
					active ? IM_COL32(237, 157, 73, 255)
					       : IM_COL32(100, 104, 106, 255), 3.0f);
			}
			const ImVec2 a(p.x + 12.0f, p.y + 2.0f);
			const ImVec2 b(a.x + 25.0f, a.y + 25.0f);
			const IconTex& image = m_icons[size_t(icon)];
			if (image.imgui != 0)
				dl->AddImage(image.imgui, a, b,
					ImVec2(0, 0), ImVec2(1, 1),
					active ? IM_COL32(255, 220, 171, 255)
					       : IM_COL32(214, 218, 219, 255));
			else
				DrawAssetIconFallback(dl, a, 25.0f, icon);
			const ImVec2 labelSize = ImGui::CalcTextSize(caption);
			dl->AddText(ImVec2(p.x + (size.x - labelSize.x) * 0.5f,
				p.y + 30.0f), IM_COL32(210, 213, 217, 255), caption);
			if (hover)
				ImGui::SetTooltip("%s", tooltip);
			ImGui::PopID();
			return pressed;
		};
		const auto next = []() { ImGui::SameLine(0.0f, 3.0f); };
		const auto split = []() {
			ImGui::SameLine(0.0f, 8.0f);
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(pos.x + 2.0f, pos.y + 2.0f),
				ImVec2(pos.x + 2.0f, pos.y + 44.0f),
				ImGui::GetColorU32(ImGuiCol_Separator));
			ImGui::Dummy(ImVec2(5.0f, 46.0f));
			ImGui::SameLine(0.0f, 8.0f);
		};
		ImGui::PushID("select");
		const IconType selectionIcons[3] = {
			IconType::ToolObject, IconType::ToolFace, IconType::ToolVertex };
		const char* selectionLabels[3] = { "Mesh", "Face", "Vertex" };
		for (int i = 0; i < 3; ++i) {
			if (i) next();
			if (tool(selectionLabels[i], selectionIcons[i],
				int(m_selMode) == i, selectionLabels[i], selectionLabels[i])
				&& int(m_selMode) != i) {
				m_selMode = SelMode(i);
				ClearSubSelection();
			}
		}
		next();
		if (tool("frame", IconType::ToolFrame, false, "Frame",
			"Frame selection (F)")) FrameSelection();
		next();
		if (tool("clear", IconType::ToolClear, false, "Clear",
			"Clear selection")) ClearSelection();
		ImGui::PopID();
		split();
		ImGui::PushID("transform");
		const IconType transformIcons[3] = {
			IconType::ToolMove, IconType::ToolRotate, IconType::ToolScale };
		const char* transformNames[3] = { "Move (W)", "Rotate (E)", "Scale (R)" };
		const char* transformCaptions[3] = { "Move", "Rotate", "Scale" };
		for (int i = 0; i < 3; ++i) {
			if (i) next();
			if (tool(transformNames[i], transformIcons[i],
				int(m_gizmoMode) == i, transformCaptions[i], transformNames[i]))
				m_gizmoMode = GizmoMode(i);
		}
		next();
		if (tool("transpose", IconType::ToolTranspose, m_transposeEnabled,
			"Trans", "Toggle the classic object Transpose line")) {
			if (!m_transposeDrag) {
				m_transposeEnabled = !m_transposeEnabled;
				m_transposeOwner = UINT32_MAX;
			}
		}
		ImGui::PopID();
		split();
		ImGui::PushID("workspace");
		const IconType modeIcons[4] = { IconType::ToolLayout,
			IconType::ToolLighting, IconType::ToolTest, IconType::ToolProfile };
		const char* modeNames[4] = {
			"Layout (1)", "Lighting (2)", "Test (3)", "Profile (4)" };
		const char* modeCaptions[4] = {
			"Layout", "Light", "Test", "Profile" };
		for (int i = 0; i < 4; ++i) {
			if (i) next();
			if (tool(modeNames[i], modeIcons[i], int(m_mode) == i,
				modeCaptions[i], modeNames[i]) && int(m_mode) != i)
				SetMode(EditorMode(i));
		}
		ImGui::PopID();
		split();
		const bool taskLive = m_bakeRunning || m_cookRunning;
		ImGui::BeginDisabled(!taskLive && SceneEmpty());
		if (tool("update_gi", IconType::GI, taskLive, "GI",
			taskLive ? "Reopen cook / GI bake progress"
				: "Update GI — rebake the current world and convex GI volumes")) {
			if (taskLive) ReopenTaskWindow();
			else BakeGI();
		}
		ImGui::EndDisabled();
		ImGui::End();
		ImGui::PopStyleVar();
		return h;
	}

	void EditorApp::DrawSaveDialog() {
		namespace fs = std::filesystem;
		ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
		if (!ImGui::BeginPopupModal("Save tour as", nullptr,
			ImGuiWindowFlags_NoCollapse))
			return;
		auto closeDlg = [&]() {
			m_saveBrowseDir.clear();
			m_saveBrowseSel.clear();
			ImGui::CloseCurrentPopup();
		};
		if (m_saveBrowseDir.empty()) {
			// First frame: start from the target field's parent dir.
			std::error_code ec;
			fs::path d = fs::path(m_savePathBuf).parent_path();
			if (d.empty())
				d = fs::current_path(ec);
			if (!fs::is_directory(d, ec))
				d = ".";
			m_saveBrowseDir = d.string();
		}
		ImGui::InputText("##savepath", m_savePathBuf, sizeof(m_savePathBuf));
		ImGui::SameLine();
		if (ImGui::Button("Up")) {
			std::error_code ec;
			fs::path p(m_saveBrowseDir);
			if (p.has_parent_path()) {
				fs::path up = p.parent_path();
				if (fs::is_directory(up, ec))
					m_saveBrowseDir = up.string();
			}
			m_saveBrowseSel.clear();
		}
		ImGui::TextDisabled("%s", m_saveBrowseDir.c_str());
		ImGui::BeginChild("##savebrowse", ImVec2(0, -60), true);
		{
			std::error_code ec;
			std::vector<fs::directory_entry> dirs, files;
			for (fs::directory_iterator it(m_saveBrowseDir, ec), end;
				!ec && it != end; it.increment(ec)) {
				const fs::directory_entry& e = *it;
				if (e.is_directory(ec))
					dirs.push_back(e);
				else if (e.is_regular_file(ec) &&
					e.path().extension() == ".tour")
					files.push_back(e);
			}
			auto byName = [](const fs::directory_entry& a,
				const fs::directory_entry& b) {
				return a.path().filename().string() <
					b.path().filename().string();
			};
			std::sort(dirs.begin(), dirs.end(), byName);
			std::sort(files.begin(), files.end(), byName);
			if (ImGui::Selectable("[..]", false,
				ImGuiSelectableFlags_AllowDoubleClick)) {
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					fs::path p(m_saveBrowseDir);
					if (p.has_parent_path()) {
						fs::path up = p.parent_path();
						if (fs::is_directory(up, ec))
							m_saveBrowseDir = up.string();
					}
					m_saveBrowseSel.clear();
				}
			}
			char label[512];
			for (auto& d : dirs) {
				const std::string name = d.path().filename().string();
				std::snprintf(label, sizeof(label), "[%s]", name.c_str());
				if (ImGui::Selectable(label, false,
					ImGuiSelectableFlags_AllowDoubleClick)) {
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						m_saveBrowseDir = d.path().string();
						m_saveBrowseSel.clear();
					}
				}
			}
			for (auto& f : files) {
				const std::string full = f.path().string();
				const std::string name = f.path().filename().string();
				const bool sel = (m_saveBrowseSel == full);
				if (ImGui::Selectable(name.c_str(), sel,
					ImGuiSelectableFlags_AllowDoubleClick)) {
					m_saveBrowseSel = full;
					std::snprintf(m_savePathBuf, sizeof(m_savePathBuf),
						"%s", full.c_str());
				}
				if (sel)
					ImGui::SetItemDefaultFocus();
				if (ImGui::IsItemHovered() &&
					ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					if (SaveTourTo(full))
						Log("Tour saved: " + full);
					closeDlg();
					ImGui::EndChild();
					ImGui::EndPopup();
					return;
				}
			}
		}
		ImGui::EndChild();
		ImGui::TextDisabled("%s",
			m_saveBrowseSel.empty() ? "type a name or pick a file" : m_saveBrowseSel.c_str());
		if (ImGui::Button("Save")) {
			std::string target = m_saveBrowseSel.empty()
				? std::string(m_savePathBuf) : m_saveBrowseSel;
			if (!target.empty()) {
				if (fs::path(target).extension() != ".tour")
					target += ".tour";
				if (SaveTourTo(target))
					Log("Tour saved: " + target);
			}
			closeDlg();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			closeDlg();
		ImGui::EndPopup();
	}

	std::string EditorApp::ResolveVfsRoot() const {
		// Prefer the content dir the current scene came from, else ./content.
		// The CANONICAL HOST PATH drives the marker walk (m_gltfHostPath — the
		// file that actually loaded, GUID-resolved on tour opens): the recorded
		// identity (m_gltfPath) can be a project-RELATIVE "content/models/..."
		// on tour-opened scenes, whose relative root then resolved against the
		// editor CWD (the build dir) instead of the project (same defect the
		// cook's contentDir walk had — see CookToPak).
		const std::string identity = !m_gltfHostPath.empty()
			? m_gltfHostPath : m_gltfPath;
		std::string posix = identity;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		if (const size_t at = posix.find("content/");
			at != std::string::npos && !identity.empty())
			return posix.substr(0, at + 7); // keep trailing "content/"
		std::error_code ec;
		if (std::filesystem::is_directory("content", ec))
			return "content";
		return ".";
	}

} // namespace editor
