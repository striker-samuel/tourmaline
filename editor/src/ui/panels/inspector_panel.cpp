// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "ui/inspector/section_registry.h"
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
	void EditorApp::DrawInspector() {
		SetupDefaultCameras();
		// Material focus page: the Inspector is "traveling" in a material.
		if (m_world && m_materialFocus != UINT32_MAX) {
			DrawMaterialFocusPage();
			return;
		}
		// Browser-selected ASSET page: a .material/.mesh click in the asset
		// browser inspects here (view parameters only; edits happen on
		// scenes — Unity-style). A .texture click opens the TEXTURE-ASSET
		// page: its import settings ARE the authoring (editable for LDR
		// sources; a KTX2 source pre-decided them — read-only).
		if (!m_assetsSel.empty()) {
			const auto kind = std::filesystem::is_directory(m_assetsSel)
				? BrowserInspectorKind::Generic
				: ClassifyBrowserInspector(m_assetsSel);
			if (kind == BrowserInspectorKind::TextureSource)
				DrawImportableTexturePage();
			else if (kind == BrowserInspectorKind::Generic)
				DrawAssetGenericPage();
			else
				DrawAssetReadonlyPage();
			return;
		}
		// World pseudo-node page (global render settings, not a scene node).
		if (m_worldSelected) {
			DrawWorldPanel();
			return;
		}
		if (!m_world || m_selected == UINT32_MAX ||
			m_selected >= m_world->scene.hierarchy.size()) {
			ImGui::TextDisabled("Nothing selected.");
			ImGui::TextWrapped("Click a mesh in the viewport or pick a node in the outliner. "
				"Drag nodes onto each other to reparent. All edits are undoable (Ctrl+Z). "
				"Global settings (sun, IBL, exposure) live on the World node at the "
				"top of the outliner.");
			return;
		}
		Scene& scene = m_world->scene;
		const uint32_t node = m_selected;
		const attr::ScopedID nodeId{int(node)};
		if (m_selection.size() > 1)
			ImGui::TextDisabled("multi-select: %zu nodes (primary #%u)",
				m_selection.size(), node);
		// Selection cycling (Up/Down arrows + Prev/Next option).
		{
			if (ImGui::Button("< Prev")) {
				SelectPrevNode();
				return;
			}
			ImGui::SameLine();
			if (ImGui::Button("Next >")) {
				SelectNextNode();
				return;
			}
			ImGui::SameLine();
			ImGui::Checkbox("Up/Down", &m_inspectorCycle);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"When on, the Up/Down arrow keys cycle the node selection.");
		}
		ImGui::Text("Node %u", node);
		// Sub-object highlight state (selection only, never edited).
		if (m_selMode == SelMode::Face) {
			if (m_selFaceNode != UINT32_MAX && m_selFaceTri != UINT32_MAX)
				ImGui::Text("face: node %u tri %u (%.2f,%.2f,%.2f)",
					m_selFaceNode, m_selFaceTri,
					m_selFacePos.x, m_selFacePos.y, m_selFacePos.z);
		}
		else if (m_selMode == SelMode::Vertex) {
			if (m_selVertNode != UINT32_MAX && m_selVertIdx != UINT32_MAX)
				ImGui::Text("vertex: node %u vert %u (%.2f,%.2f,%.2f)",
					m_selVertNode, m_selVertIdx,
					m_selVertPos.x, m_selVertPos.y, m_selVertPos.z);
		}

		// Name.
		{
			char buf[256];
			const std::string cur = getNodeName(scene, int(node));
			std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
			if (attr::Text("Name", buf, sizeof(buf),
					"The node's display name (Enter commits the "
					"undoable rename)")) {
				EditCmd c{};
				c.verb = EditVerb::Rename;
				c.node = node;
				c.afterText = buf;
				c.beforeText = cur;
				Do(c);
			}
		}

		// Transform (coalesced drags: one command per gesture). The
		// per-axis rows (attr::Vec3, expandable) AGGREGATE the item
		// state; the activation feeds TrackDrag, the finish edge closes
		// the gesture.
		{
			glm::vec3 t, r, s;
			ReadLocalTRS(node, t, r, s);
			float ts[3] = { t.x, t.y, t.z };
			float rs[3] = { r.x, r.y, r.z };
			float ss[3] = { s.x, s.y, s.z };
			const attr::Vec3Result tv = attr::Vec3("Translate", ts, 0.05f,
				"The node's local translation (m)");
			const bool tOwn = TrackDrag(EditVerb::Translate, node,
				scene.localTransform[node], tv.activated);
			if (tOwn && tv.edited)
				WriteLocalTRS(node, glm::vec3(ts[0], ts[1], ts[2]), r, s);
			if (tOwn && tv.finished) {
				m_pending.cmd.afterVec = glm::vec3(ts[0], ts[1], ts[2]);
				FinishDrag();
			}
			const attr::Vec3Result rv = attr::Vec3("Rotate", rs, 0.5f,
				"The node's local Euler rotation (degrees)");
			const bool rOwn = TrackDrag(EditVerb::Rotate, node,
				scene.localTransform[node], rv.activated);
			if (rOwn && rv.edited)
				WriteLocalTRS(node, t, glm::vec3(rs[0], rs[1], rs[2]), s);
			if (rOwn && rv.finished) {
				m_pending.cmd.afterVec = glm::vec3(rs[0], rs[1], rs[2]);
				FinishDrag();
			}
			const attr::Vec3Result sv = attr::Vec3("Scale", ss, 0.02f,
				"The node's local scale (uniform per axis)");
			const bool sOwn = TrackDrag(EditVerb::Scale, node,
				scene.localTransform[node], sv.activated);
			if (sOwn && sv.edited)
				WriteLocalTRS(node, t, r, glm::vec3(ss[0], ss[1], ss[2]));
			if (sOwn && sv.finished) {
				m_pending.cmd.afterVec = glm::vec3(ss[0], ss[1], ss[2]);
				FinishDrag();
			}
		}

		// Parent.
		{
			const int p = scene.hierarchy[node].parent;
			ImGui::Text("Parent: %d (%s)", p,
				p >= 0 ? getNodeName(scene, p).c_str() : "-");
			ImGui::SameLine();
			if (ImGui::Button("Detach to root") && p != int(m_clusterBase)) {
				if (m_clusterBase == UINT32_MAX) {
					Log("Detach: no cluster mapping (multi-cluster scene?)");
				}
				else {
					EditCmd c{};
					c.verb = EditVerb::Parent;
					c.node = node;
					c.node2 = m_clusterBase;
					c.beforeParent = uint32_t(p);
					Do(c);
				}
			}
		}

		// Mesh info.
		if (const MeshRow* meshRow = FindMesh(scene, node)) {
			if (smvk::StaticMesh* mesh = m_world->GetMesh(meshRow->mesh)) {
				uint32_t tris = 0;
				for (auto& s : mesh->geometry.surfaces)
					tris += s.indexCount / 3;
				ImGui::Text("Mesh: %u verts, %u tris", mesh->geometry.vertexCount, tris);
				if (mesh->lmChunk == 0xFFFFFFFFu) {
					ImGui::Text("Lightmap: none (probe/sky lit)");
				}
				else {
					ImGui::Text("Lightmap: baked, %u lobes", mesh->lmChunk);
					if (m_hasSglm)
						ImGui::TextDisabled("atlas: %s (%ux%u)",
							m_sglmPath.c_str(), m_sglm.width, m_sglm.height);
					else
						ImGui::TextDisabled("atlas: not loaded (see panel below)");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("UV1 (F10) shows baked texCoord2;\n"
							"F8 shows baked irradiance in place.");
				}
			}
		}


		// Cooked light on this node (KHR punctual, import or pak).
		if (glm::vec3 aimWs(0.0f);
			const ::SceneLight * li = FindNodeLight(node, &aimWs)) {
			const attr::ScopedID lightId("light");
			ImGui::Separator();
			const char* tname = li->type == ::SceneLight::Type::Directional
				? "Directional (sun)"
				: li->type == ::SceneLight::Type::Spot ? "Spot" : "Point";
			ImGui::Text("Light: %s '%s'", tname, li->name.c_str());
			// (The readouts — the live color/intensity, the aim axis, the
			// world position, the cone angles — ride the ATTRIBUTE rows'
			// hover hints now; the info text rows below the attrs are the
			// killed presentation.)
			if ((li->flags & ::SceneLight::kSceneLightFlag_Static) != 0
				&& ImGui::IsItemHovered())
				ImGui::SetTooltip("STATIC: bake-only (excluded from runtime)");
			if (li->type == ::SceneLight::Type::Spot) {
				const bool lookingThrough =
					m_viewports[m_activeVp].boundNode == int(node);
				if (ImGui::Button(lookingThrough
					? "Return to editor camera" : "Look through spotlight")) {
					m_viewports[m_activeVp].boundNode =
						lookingThrough ? -1 : int(node);
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Lock the active viewport to the spotlight's "
						"position and -Z aim. The outer cone defines the view angle. "
						"Move or rotate the light with its transform gizmo.");
				ImGui::SameLine();
				ImGui::TextDisabled(lookingThrough ? "LIVE CONE VIEW" : "Preview aim");
			}

			// ---- editable light properties (point/spot): SetLightAttr,
			// undoable, persisted as setLightAttr tour verbs so cooks replay
			// them. Sliders coalesce one command per drag (mattr pattern);
			// chips commit immediately. Every row rides the attr stubs. ----
			if (li->type != ::SceneLight::Type::Directional) {
				const bool isSpot = li->type == ::SceneLight::Type::Spot;
				auto beginLightDrag = [&](const char* attr) {
					if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetLightAttr ||
						m_pending.cmd.node != node || m_pending.cmd.lightAttr != attr))
						FinalizePending();
					if (!m_pending.active) {
						m_pending.active = true;
						m_pending.cmd = EditCmd{};
						m_pending.cmd.verb = EditVerb::SetLightAttr;
						m_pending.cmd.node = node;
						m_pending.cmd.lightAttr = attr;
						ReadNodeLightAttr(node, attr, m_pending.cmd.beforeVec4,
							m_pending.cmd.beforeNum, m_pending.cmd.beforeFlag,
							m_pending.cmd.beforeText);
					}
				};
				auto endLightDrag = [&](const char* attr) {
					if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
						m_pending.cmd.verb == EditVerb::SetLightAttr &&
						m_pending.cmd.node == node &&
						m_pending.cmd.lightAttr == attr)
						FinishDrag();
				};
				glm::vec3 lcol = li->color;
				{
					float col4[4] = { lcol.x, lcol.y, lcol.z, 1.0f };
					char hint[160];
					std::snprintf(hint, sizeof(hint),
						"Light color (live %.2f %.2f %.2f)", lcol.x, lcol.y, lcol.z);
					if (attr::Color("Color", col4, hint)) {
						beginLightDrag("color");
						WriteNodeLightAttr(node, "color",
							glm::vec4(col4[0], col4[1], col4[2], 1.0f), 0, false, {});
						m_pending.cmd.vec4 =
							glm::vec4(col4[0], col4[1], col4[2], 1.0f);
					}
					endLightDrag("color");
				}
				float lint = li->intensity;
				if (attr::Float("Intensity", &lint, 1.0f, 0.0f, 1e7f, "%.1f",
						isSpot ? "Spot intensity (cd), attenuated by range"
						: "Point intensity (cd), attenuated by range")) {
					beginLightDrag("intensity");
					WriteNodeLightAttr(node, "intensity", glm::vec4(0), lint, false, {});
					m_pending.cmd.number = lint;
				}
				endLightDrag("intensity");
				float lrange = li->range;
				if (attr::Float("Range", &lrange, 0.1f, 0.0f, 500.0f, "%.1f",
						"Attenuation range in m (0 = infinite)")) {
					beginLightDrag("range");
					WriteNodeLightAttr(node, "range", glm::vec4(0), lrange, false, {});
					m_pending.cmd.number = lrange;
				}
				endLightDrag("range");
				if (isSpot) {
					float inner = glm::degrees(li->innerConeAngle);
					if (attr::Slider("Inner cone", &inner, 0.0f, 89.0f, "%.1f",
							"Inner cone angle (deg) — full intensity inside")) {
						beginLightDrag("inner");
						WriteNodeLightAttr(node, "inner", glm::vec4(0), inner, false, {});
						m_pending.cmd.number = inner;
					}
					endLightDrag("inner");
					float outer = glm::degrees(li->outerConeAngle);
					if (attr::Slider("Outer cone", &outer, 0.0f, 89.0f, "%.1f",
							"Outer cone angle (deg) — the falloff edge")) {
						beginLightDrag("outer");
						WriteNodeLightAttr(node, "outer", glm::vec4(0), outer, false, {});
						m_pending.cmd.number = outer;
					}
					endLightDrag("outer");
				}
				bool lstatic = (li->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
				if (attr::Bool("Static", lstatic,
						"STATIC — bake-only (excluded from the runtime "
						"Forward+ set)")) {
					EditCmd c{};
					c.verb = EditVerb::SetLightAttr;
					c.node = node;
					c.lightAttr = "static";
					c.afterFlag = lstatic ? 0 : 1;
					ReadNodeLightAttr(node, "static", c.beforeVec4, c.beforeNum,
						c.beforeFlag, c.beforeText);
					Do(c);
				}
				// Cast-shadow participation: spots get one perspective tile,
				// points get six cube-face tiles - both ride the local-light
				// shadow atlas (spotshadow.h).
				if (isSpot || li->type == ::SceneLight::Type::Point) {
					bool lshadow = (li->flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0;
					if (attr::Bool("Shadows", lshadow,
							isSpot ? "Spot shadow map participation"
								: "Point shadow map participation (six cube-face tiles)")) {
						EditCmd c{};
						c.verb = EditVerb::SetLightAttr;
						c.node = node;
						c.lightAttr = "shadow";
						c.afterFlag = lshadow ? 0 : 1;
						ReadNodeLightAttr(node, "shadow", c.beforeVec4, c.beforeNum,
							c.beforeFlag, c.beforeText);
						Do(c);
					}
				}
				if (isSpot) {
					// Spot COOKIE (gobo): a .texture asset path (or image URI)
					// projected through the cone — the authored cookieUri the
					// light build resolves to the bindless index (empty = none).
					// Browse imports the picked image as a .texture beside the
					// asset tree in one click (see CreateTextureAsset).
					ImGui::SeparatorText("Cookie (spot gobo)");
					char cookieBuf[512] = {};
					std::snprintf(cookieBuf, sizeof(cookieBuf), "%s",
						li->cookieUri.c_str());
					attr::LabelCell("Cookie",
						"The .texture (or image) projected through the cone "
						"as a gobo. Empty = none. Unresolvable refs warn at "
						"the light build and drop (the light renders "
						"unmodified).");
					ImGui::SetNextItemWidth(
						ImGui::GetContentRegionAvail().x - 92.0f);
					if (ImGui::InputText("##lightcookie", cookieBuf,
						sizeof(cookieBuf),
						ImGuiInputTextFlags_EnterReturnsTrue)) {
						EditCmd c{};
						c.verb = EditVerb::SetLightAttr;
						c.node = node;
						c.lightAttr = "cookie";
						c.afterText = cookieBuf;
						ReadNodeLightAttr(node, "cookie", c.beforeVec4, c.beforeNum,
							c.beforeFlag, c.beforeText);
						ImportCookieTexture(cookieBuf);
						Do(c);
					}
					ImGui::SameLine();
					if (ImGui::Button("Browse...")) {
						std::string picked;
						if (NativeOpenFile(picked, true) && !picked.empty()) {
							// Full asset flow in one click: the picked image
							// becomes a .texture (import settings wrap it from
							// here on) and the cookie ref points at it.
							const std::string tex =
								CreateTextureAsset(picked, m_assetsDir);
							if (!tex.empty()) {
								std::string rel = ProjectRelativePath(tex);
								for (char& ch : rel)
									if (ch == '\\')
										ch = '/';
								EditCmd c{};
								c.verb = EditVerb::SetLightAttr;
								c.node = node;
								c.lightAttr = "cookie";
								c.afterText =
									rel.empty() ? tex : rel;
								ReadNodeLightAttr(node, "cookie", c.beforeVec4,
									c.beforeNum, c.beforeFlag, c.beforeText);
								ImportCookieTexture(c.afterText);
								Do(c);
							}
						}
					}
				}
				// Procedural light FUNCTION (points + spots): per-pixel intensity
				// modulation — flicker (candle-like time noise), pulse (sine
				// breathing), bands (moving diagonal bands across the cone;
				// points fall back to pulse). Speed scales time, scale is the
				// modulation depth (1 = dips to black). See light.hlsli.
				ImGui::SeparatorText("Light function (procedural)");
				{
					int fn = int(std::clamp(li->functionType, 0u, 3u));
					if (attr::Enum("Function", &fn,
							"none\0flicker\0pulse\0bands\0",
							"Per-pixel intensity modulation (points fall "
							"back to pulse)")) {
						EditCmd c{};
						c.verb = EditVerb::SetLightAttr;
						c.node = node;
						c.lightAttr = "function";
						c.number = double(fn);
						ReadNodeLightAttr(node, "function", c.beforeVec4,
							c.beforeNum, c.beforeFlag, c.beforeText);
						Do(c);
					}
					float fnspeed = li->functionSpeed;
					if (attr::Float("Speed", &fnspeed, 0.01f, 0.01f,
							10.0f, "%.2f", "Scales time")) {
						beginLightDrag("functionSpeed");
						WriteNodeLightAttr(node, "functionSpeed", glm::vec4(0),
							fnspeed, false, {});
						m_pending.cmd.number = fnspeed;
					}
					endLightDrag("functionSpeed");
					float fnscale = li->functionScale;
					if (attr::Float("Depth", &fnscale, 0.01f, 0.0f, 1.0f,
							"%.2f", "The modulation depth (1 = dips to "
							"black)")) {
						beginLightDrag("functionScale");
						WriteNodeLightAttr(node, "functionScale", glm::vec4(0),
							fnscale, false, {});
						m_pending.cmd.number = fnscale;
					}
					endLightDrag("functionScale");
				}
			}
		}

		// Authored GI volume on this node (GI volume inspector).
		// Components register their existing draw handlers in display order.
		InspectorSectionRegistry sections;
		sections.Register("gi-volume", [this](uint32_t n) { DrawGiVolumeSection(n); });
		sections.Register("trigger", [this](uint32_t n) { DrawTriggerVolumeSection(n); });
		sections.Register("physics", [this](uint32_t n) { DrawPhysicsBodySection(n); });
		sections.Register("cubemap", [this](uint32_t n) { DrawCubemapSection(n); });
		sections.Register("camera", [this](uint32_t n) { DrawCamSection(n); });
		sections.Register("audio", [this](uint32_t n) { DrawAudioSection(n); });
		sections.Register("audio-room", [this](uint32_t n) { DrawAudioRoomSection(n); });
		sections.DrawAll(node);
		// Single discoverable add-component flow. Only list missing
		// associations and reuse the existing undoable attach commands.
		ImGui::Separator();
		if (ImGui::Button("Add component..."))
			ImGui::OpenPopup("##add_component_picker");
		if (ImGui::BeginPopup("##add_component_picker")) {
			ImGui::TextUnformatted("Add component");
			ImGui::Separator();
			static ImGuiTextFilter componentFilter;
			componentFilter.Draw("Search##component", 230.0f);
			struct ComponentChoice { const char* label; EditVerb verb; bool present; };
			const ComponentChoice choices[] = {
				{ "Physics Body", EditVerb::AttachPhysicsBody,
					tour::scene::FindPhysicsBody(scene, node) != nullptr },
				{ "Trigger Volume", EditVerb::AttachTriggerVolume,
					tour::scene::FindTriggerVolume(scene, node) != nullptr },
				{ "GI Volume", EditVerb::AttachGiVolume,
					FindGiVolume(scene, node) != nullptr },
				{ "Audio Room", EditVerb::AttachAudioRoom,
					tour::scene::FindAudioRoom(scene, node) != nullptr },
			};
			bool any = false;
			for (const ComponentChoice& choice : choices) {
				if (choice.present || !componentFilter.PassFilter(choice.label))
					continue;
				any = true;
				if (ImGui::Selectable(choice.label)) {
					EditCmd cmd{};
					cmd.verb = choice.verb;
					cmd.node = node;
					Do(cmd);
					ImGui::CloseCurrentPopup();
					break;
				}
			}
			if (!any)
				ImGui::TextDisabled("No available components");
			ImGui::EndPopup();
		}
		// The scene's export config node (cook/export knobs).
		DrawExportSettingsSection(node);

		// Static subtree (transform freeze): the root mark affects the WHOLE
		// tree (Scene::staticRoot) — frozen transforms after the initial
		// build (skip per-frame updates + BVH rebuilds) + mega-mesh collapse
		// eligibility at cook.
		{
			bool self = node < scene.staticRoot.size()
				&& scene.staticRoot[node] != 0;
			if (attr::Check("Static", &self,
					"Transforms freeze after the initial build (skip "
					"per-frame updates + BVH rebuilds); cook may collapse "
					"the subtree into one block")) {
				EditCmd c{};
				c.verb = EditVerb::SetStatic;
				c.node = node;
				c.afterFlag = self;
				c.beforeFlag = !self;
				Do(c);
			}
		}

		// Lightmap participation: opt-IN (default off) + legacy opt-out.
		// A mesh charts only when opted in here AND not excluded below.
		{
			bool self = node < scene.lightmap.size() && scene.lightmap[node] != 0;
			if (attr::Check("Bake lightmaps", &self,
					"A mesh charts only when opted in here AND not "
					"excluded below")) {
				EditCmd c{};
				c.verb = EditVerb::SetLightmap;
				c.node = node;
				c.afterFlag = self;
				c.beforeFlag = !self;
				Do(c);
			}
		}
		{
			bool self = node < scene.noLightmap.size() && scene.noLightmap[node] != 0;
			if (attr::Check("Exclude lightmaps", &self,
					"The legacy opt-out; the opt-in above is still "
					"required")) {
				EditCmd c{};
				c.verb = EditVerb::SetNoLightmap;
				c.node = node;
				c.afterFlag = self;
				c.beforeFlag = !self;
				Do(c);
			}
		}

		// Dither on camera closeup (subtree flag; the GPU fade is LIVE — per
		// frame, per draw, no rebake needed: the renderer re-resolves the flag
		// every frame, so a checkbox toggle reflects on the next frame).
		{
			bool self = node < scene.ditherCloseup.size() && scene.ditherCloseup[node] != 0;
			if (attr::Check("Dither closeup", &self,
					"1 m fully hidden .. 8 m solid, screen-door fade from "
					"the subtree anchor; LIVE per frame (no rebake)")) {
				EditCmd c{};
				c.verb = EditVerb::SetDitherCloseup;
				c.node = node;
				c.afterFlag = self;
				c.beforeFlag = !self;
				Do(c);
			}
		}

		// Volumetric-shadow caster mark (SELF flag - the coarse fog tile):
		// the dedicated god-ray shadow pass renders ONLY flagged meshes
		// (large occluders). Live per frame (the renderer re-resolves the
		// flag each frame via the CastVolumetricShadow iterator bit).
		{
			bool self = IsCastsVolumetricShadowNode(scene, int(node));
			if (attr::Check("Volumetric shadow", &self,
					"Large occluders only (buildings, roofs, walls, "
					"cliffs): the coarse fog-shadow tile; small props and "
					"foliage belong to the surface CSM's detail. LIVE per "
					"frame.")) {
				EditCmd c{};
				c.verb = EditVerb::SetCastsVolumetricShadow;
				c.node = node;
				c.afterFlag = self;
				c.beforeFlag = !self;
				Do(c);
			}
		}

		// Terrain group/tile: height paint controls (editor paint mode).
		if (const uint32_t group = TerrainGroupOf(node); group != UINT32_MAX) {
			ImGui::Separator();
			auto it = m_terrainGroups.find(group);
			if (it != m_terrainGroups.end()) {
				ImGui::Text("Terrain '%s'  (%ux%u tiles)",
					getNodeName(scene, int(group)).c_str(),
					it->second.data.tilesX, it->second.data.tilesZ);
				{
					char hm[96];
					std::snprintf(hm, sizeof(hm),
						"%u x %u samples, %.0f m, group #%u",
						it->second.data.heights.width,
						it->second.data.heights.height,
						it->second.data.params.size, group);
					attr::Readonly("Heightmap", hm);
				}
				attr::Enum("Brush", &m_paintOp,
					"Raise\0Lower\0Flatten\0Smooth\0",
					"The height paint operation");
				attr::Float("Radius", &m_paintRadius, 0.25f, 0.5f, 256.0f,
					"%.1f", "The brush radius (m)");
				attr::Float("Strength", &m_paintStrength, 0.01f, 0.01f,
					64.0f, "%.2f", "The per-stamp height delta scale");
				DrawBrushPresetControls(false);
				if (attr::Check("Paint mode", &m_paintTerrain,
						"While on, LMB in a 3D viewport paints the "
						"selected terrain instead of picking. Each stamp "
						"is one undoable command; paint persists as tour "
						"terrainPaint verbs (deterministic replay).")) {
					if (m_paintTerrain) m_paintScatter = false;
					// (The old "surface the Terrain toolbar tab" hint went
					// with the tabs — the paint cluster is ALWAYS in the
					// top toolbar's Terrain section now.)
				}
			}
		}

		// Clarisse-style override summary for this node (visibility control +
		// per-attr inherited/local/wide badges + wide-rule matches).
		DrawInspectorOverrides(node);

		// Environment node: mark/use this node as an IBL source (scene
		// definition — the block lives in the scene, the selection below
		// resolves into the rendered look).
		{
			ImGui::Separator();
			ImGui::Text("Environment node");
			if (IsEnvNode(node)) {
				if (ImGui::Button("Use as current IBL source")) {
					m_envCurrent = node;
					ResolveSceneEnv();
				}
				ImGui::SameLine();
				if (ImGui::Button("Unmark##envnode"))
					UnmarkEnvNode(node);
			}
			else if (ImGui::Button("Mark as Environment node")) {
				MarkEnvNode(node);
				m_envCurrent = node;
				ResolveSceneEnv();
			}
		}

		// Scene reference block (placeholder nodes).
		DrawReferenceSection(node);
		// Material reassignment.
		if (const MaterialRow* matRow = FindMaterial(scene, node)) {
			ImGui::Separator();
			ImGui::Text("Material");
			const uint32_t curMat = matRow->material;
			std::string preview = "mat." + std::to_string(curMat);
			if (ImGui::BeginCombo("##mat", preview.c_str())) {
				const uint32_t count = uint32_t(m_world->materialLocations.size());
				for (uint32_t i = 0; i < count; ++i) {
					const auto& loc = m_world->materialLocations[i];
					std::string uri = loc.cluster
						? std::string(loc.cluster->GetMaterialUri(loc.handle).Str()) : "";
					char item[320];
					std::snprintf(item, sizeof(item), "%u: %s", i,
						uri.empty() ? "(unnamed)" : uri.c_str());
					const bool sel = (i == curMat);
					if (ImGui::Selectable(item, sel) && i != curMat) {
						EditCmd c{};
						c.verb = EditVerb::AssignMaterial;
						c.node = node;
						c.material = i;
						c.beforeMaterial = curMat;
						Do(c);
						Log("Node " + std::to_string(node) + " -> material " +
							std::to_string(i));
					}
					if (sel)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			// Travel: open this material in the material editor page.
			ImGui::SameLine();
			if (ImGui::SmallButton("Edit material >>")) {
				m_materialTrail.clear();
				m_materialFocus = curMat;
			}
			// 2D material preview + THE SHARED BODY (the consolidation:
			// this block used to carry its own preview + scalar/flags
			// copy — the same surface the Material tab and the focus
			// page render, from ONE function).
			DrawMaterialBody(curMat);
		}
		// NOTE: the active-viewport orbit camera used to live here; it moved
		// to the World panel ("Viewport camera"). Node pages show authored
		// state only — camera nodes get the lens section above instead.
	}

	void EditorApp::DrawCameraSection() {
		ImGui::Separator();
		ImGui::Text("Editor camera (viewport %d)", m_activeVp);
		if (m_viewports[m_activeVp].camIdx >= int(m_editorCams.size()))
			return;
		EditorCam& cam = m_editorCams[m_viewports[m_activeVp].camIdx];
		int type = int(cam.type);
		if (attr::Enum("Projection", &type,
				"Perspective\0Orthographic\0", "The lens projection")) {
			cam.type = EditorCam::Type(type);
			// Switching projection modes: give the ortho a sensible extent
			// from the current framing distance.
			if (cam.type == EditorCam::Type::Orthographic && cam.orthoHalfH < 0.1f)
				cam.orthoHalfH = std::max(cam.dist * 0.25f, 1.0f);
		}
		if (cam.type == EditorCam::Type::Perspective) {
			attr::Slider("FOV", &cam.fovYDeg, 10.0f, 120.0f, "%.0f",
				"The vertical field of view (deg)");
		}
		else {
			attr::Float("Ortho half-height", &cam.orthoHalfH, 0.25f, 0.1f,
				5000.0f, "%.1f", "The orthographic vertical half-extent (m)");
		}
		attr::Float("Near clip", &cam.nearClip, 0.005f, 0.001f, 50.0f,
			"%.3f", "The near plane (m)");
		attr::Float("Far clip", &cam.farClip, 5.0f, 10.0f, 100000.0f,
			"%.0f", "The far plane (m)");
		if (cam.farClip <= cam.nearClip * 1.1f)
			cam.farClip = cam.nearClip * 1.1f + 10.0f;
	}

} // namespace editor
