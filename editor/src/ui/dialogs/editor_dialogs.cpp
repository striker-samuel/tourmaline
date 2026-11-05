// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "system_modal.h"
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
	void EditorApp::RequestAssetDelete(const std::string* targets,
		size_t targetCount) {
		// TMP-DIAG: the request trace (the delete-flow hunt — which
		// gesture reached here, with what). Remove when stable.
		{
			std::string list;
			for (size_t i = 0; targets && i < targetCount && i < 6; ++i)
				list += (i ? ", '" : "'") + targets[i] + "'";
			if (targetCount > 6)
				list += " ...";
			Log("Delete request: " + std::to_string(targetCount)
				+ " target(s)" + (list.empty() ? "" : ": " + list));
		}
		m_assetsDeletePending.clear();
		if (m_projectRoot.empty()) {
			Log("Delete: open a project first.");
			return;
		}
		if (!targets || targetCount == 0)
			return;
		// The live-scene set (refused targets): deleting the open scene
		// from under the editor desyncs the tour autosave — refuse those
		// per-target, keep the rest.
		auto isLiveScene = [&](const std::string& absPath) {
			namespace fs = std::filesystem;
			std::error_code ec;
			const std::string abs =
				fs::absolute(fs::path(absPath), ec)
				.lexically_normal().generic_string();
			for (const std::string& live :
				{ m_gltfPath, m_gltfHostPath }) {
				if (live.empty())
					continue;
				std::error_code lec;
				const std::string norm =
					fs::absolute(fs::path(live), lec)
					.lexically_normal().generic_string();
				if (!lec && abs == norm)
					return true;
			}
			return false;
		};
		size_t refused = 0;
		for (size_t i = 0; i < targetCount; ++i) {
			const std::string& absPath = targets[i];
			if (absPath.empty())
				continue;
			if (isLiveScene(absPath)) {
				Log("Delete: '" + absPath + "' is open — close the "
					"scene first.");
				++refused;
				continue;
			}
			AssetDeletePlan plan;
			std::string err;
			if (!PlanAssetDelete(absPath, m_projectRoot, plan, err)) {
				Log("Delete: " + err + ".");
				++refused;
				continue;
			}
			m_assetsDeletePending.push_back(std::move(plan));
		}
		// TMP-DIAG (the outcome half of the request trace): ONE line
		// that tells the story — the refusals print their reasons above,
		// this makes the tally unmissable.
		Log("Delete request outcome: "
			+ std::to_string(m_assetsDeletePending.size()) + " planned, "
			+ std::to_string(refused) + " refused"
			+ (m_assetsDeletePending.empty()
				? " — nothing to confirm (see the refusal reasons above)"
				: " — confirm in the dialog"));
		if (!m_assetsDeletePending.empty()) {
			m_assetsDeletePopupWanted = true;
		}
	}

	// ---- THE GENERIC DIALOG (the ONE modal mechanism — see the header
	// block): a real BeginPopupModal behind the flag (DialogBegin below
	// owns the open/close). One dialog at a time.
	void EditorApp::DialogOpen() {
		m_dialogUp = true;
	}
	void EditorApp::DialogClose() {
		m_dialogUp = false;
	}
	bool EditorApp::DialogBegin(const char* title, const ImVec2& size) {
		// A REAL ImGui modal now (OpenPopup + BeginPopupModal — the
		// centralized dialog controller the delete-confirm flows
		// through): the POPUP STACK owns the layering (every later
		// ImGui element, and the composite before the UI pass, stays
		// under it) and the input blocking (windows below go inert —
		// the old hand-rolled full-screen dimmer + InvisibleButton is
		// gone), and ImGui's ModalWindowDimBg draws the dim. The popup
		// id is the TITLE, and OpenPopup + BeginPopupModal sit on
		// adjacent lines in THIS function — the same id stack by
		// construction (the popup-depth rule; no intervening statement
		// can ever shift it again).
		constexpr ImGuiWindowFlags kDialogFlags =
			ui::kSystemModalFlags | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_AlwaysAutoResize;
		// The flag dropped since last frame (DialogClose from OUTSIDE
		// the popup — the stale-dialog drop or a confirm button): the
		// popup can still be on the stack; CloseCurrentPopup is the
		// only public close and must run INSIDE it, so begin it once
		// more purely to close it.
		if (!m_dialogUp) {
			ui::PrepareSystemModal(size);
			if (ImGui::BeginPopupModal(title, nullptr, kDialogFlags)) {
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			return false;
		}
		if (!ImGui::IsPopupOpen(title, ImGuiPopupFlags_AnyPopupId))
			ImGui::OpenPopup(title);
		ui::PrepareSystemModal(size);
		if (!ImGui::BeginPopupModal(title, nullptr, kDialogFlags))
			return false;
		if (ui::SystemModalCloseRequested()) {
			DialogClose();
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return false;
		}
		return true;
	}
	void EditorApp::DialogEnd() {
		ImGui::EndPopup();
	}

	void EditorApp::DrawAssetDeleteModal() {
		// Stale dialog without plans: drop it. DialogBegin STILL runs —
		// its down leg closes the popup from the stack (the confirm
		// buttons only drop the flag; CloseCurrentPopup must run inside
		// the popup, one frame later at worst).
		if (m_assetsDeletePending.empty()) {
			if (DialogUp())
				DialogClose();
			(void)DialogBegin("Delete asset?", ImVec2(460, 0));
			return;
		}
		// The request sets the flag one frame (the menu-close frame);
		// this raises the GENERIC DIALOG (the one modal mechanism).
		if (m_assetsDeletePopupWanted) {
			m_assetsDeletePopupWanted = false;
			DialogOpen();
		}
		if (!DialogBegin("Delete asset?", ImVec2(460, 0)))
			return;
		// Count-aware display: ONE plan reads exactly like the old
		// single; N plans list the names (capped, the count carries the
		// rest) with each plan's also/notes folded into its row.
		const size_t count = m_assetsDeletePending.size();
		if (count == 1) {
			const AssetDeletePlan& plan = m_assetsDeletePending[0];
			const std::string rel = ProjectRelativePath(plan.target);
			ImGui::Text("Delete '%s'?",
				(rel.empty() ? plan.target.c_str() : rel.c_str()));
			if (plan.isDir) {
				ImGui::TextDisabled("a folder with %zu entr%s (recursive)",
					plan.entryCount, plan.entryCount == 1 ? "y" : "ies");
			}
			if (!plan.also.empty()) {
				ImGui::TextDisabled("+ %zu more file(s) (sidecar/derived)",
					plan.also.size());
			}
			for (const std::string& note : plan.notes)
				ImGui::TextDisabled("%s", note.c_str());
		}
		else {
			ImGui::Text("Delete %zu assets?", count);
			size_t shown = 0;
			for (const AssetDeletePlan& plan : m_assetsDeletePending) {
				if (shown++ >= 8) {
					ImGui::TextDisabled("  ... (%zu more)",
						count - 8);
					break;
				}
				const std::string rel = ProjectRelativePath(plan.target);
				ImGui::BulletText("%s%s",
					(rel.empty() ? plan.target.c_str() : rel.c_str()),
					plan.isDir ? " (folder, recursive)" : "");
				if (!plan.also.empty())
					ImGui::TextDisabled("    + %zu sidecar/derived",
						plan.also.size());
			}
		}
		ImGui::TextDisabled("References to deleted assets may become unresolved.");
		ImGui::Separator();
		if (ImGui::Button("Delete", ImVec2(120, 0))) {
			size_t okCount = 0, fileCount = 0;
			for (const AssetDeletePlan& plan : m_assetsDeletePending) {
				std::string err;
				if (!ExecuteAssetDelete(plan, err)) {
					Log("Delete failed ('" + plan.target + "'): " + err);
					continue;
				}
				++okCount;
				fileCount += plan.also.size() + 1;
				// Heal the browser state (the deleted paths are gone).
				namespace fs = std::filesystem;
				auto norm = [](const std::string& p) {
					std::error_code ec;
					return fs::absolute(fs::path(p), ec)
						.lexically_normal().generic_string();
				};
				auto under = [&](const std::string& p) {
					const std::string n = norm(p);
					return n == plan.target
						|| (n.size() > plan.target.size()
							&& n.compare(0, plan.target.size(),
								plan.target) == 0
							&& n[plan.target.size()] == '/');
				};
				if (!m_assetsSel.empty() && under(m_assetsSel))
					m_assetsSel.clear();
				if (!m_assetsDir.empty() && under(m_assetsDir))
					m_assetsDir.clear(); // healed to content/ next draw
				if (!m_assetsExpandGltf.empty()
					&& under(m_assetsExpandGltf)) {
					m_assetsExpandGltf.clear();
					m_assetsSel.clear();
				}
			}
			// The set heals wholesale: drop every entry under any target.
			m_assetsSelSet.erase(std::remove_if(m_assetsSelSet.begin(),
				m_assetsSelSet.end(), [&](const std::string& p) {
					for (const AssetDeletePlan& plan
						: m_assetsDeletePending) {
						if (p == plan.target)
							return true;
					}
					return false;
				}), m_assetsSelSet.end());
			m_assetInspectPath.clear();
			m_assetInspectMat.reset();
			m_assetInspectMesh.reset();
			m_assetInspectGiCook.reset();
			m_assetInspectLmCook.reset();
			if (okCount)
				Log("Deleted " + std::to_string(okCount) + " asset(s) ("
					+ std::to_string(fileCount) + " file(s)).");
		m_assetsDeletePending.clear();
		DialogClose();
		DialogEnd();
		return; // the confirm pass ends here (no Cancel row this frame)
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			m_assetsDeletePending.clear();
			DialogClose();
		}
		DialogEnd();
	}
	// Fallback icon art (drawn vector shapes): used when an SVG icon failed
	// to rasterize/register (imgui == 0). Color-tinted per browser kind.
	void EditorApp::DrawAssetIconFallback(ImDrawList* dl, const ImVec2& org,
		float s, IconType k) const {
		const float x = org.x, y = org.y;
		switch (k) {
		case IconType::Folder: {
			const ImU32 body = IM_COL32(233, 178, 64, 255);
			const ImU32 edge = IM_COL32(150, 108, 20, 255);
			dl->AddRectFilled(ImVec2(x + s * .08f, y + s * .22f),
				ImVec2(x + s * .46f, y + s * .36f), body, 2.0f);
			dl->AddRectFilled(ImVec2(x + s * .08f, y + s * .30f),
				ImVec2(x + s * .92f, y + s * .80f), body, 4.0f);
			dl->AddRect(ImVec2(x + s * .08f, y + s * .30f),
				ImVec2(x + s * .92f, y + s * .80f), edge, 4.0f, 0, 1.5f);
			break;
		}
		case IconType::Model: {
			const ImU32 fill = IM_COL32(72, 164, 172, 255);
			const ImU32 edge = IM_COL32(22, 94, 102, 255);
			const ImVec2 hex[6] = {
				{ x + s * .50f, y + s * .14f },{ x + s * .86f, y + s * .34f },
				{ x + s * .86f, y + s * .68f },{ x + s * .50f, y + s * .88f },
				{ x + s * .14f, y + s * .68f },{ x + s * .14f, y + s * .34f } };
			dl->AddConvexPolyFilled(hex, 6, fill);
			dl->AddPolyline(hex, 6, edge, ImDrawFlags_Closed, 1.5f);
			const ImVec2 c{ x + s * .50f, y + s * .51f };
			dl->AddLine(c, hex[0], edge, 1.5f);
			dl->AddLine(c, hex[2], edge, 1.5f);
			dl->AddLine(c, hex[4], edge, 1.5f);
			break;
		}
		case IconType::Scene:
		case IconType::File: {
			const ImU32 fill = IM_COL32(238, 240, 243, 255);
			const ImU32 edge = IM_COL32(120, 128, 138, 255);
			dl->AddRectFilled(ImVec2(x + s * .26f, y + s * .12f),
				ImVec2(x + s * .74f, y + s * .88f), fill, 3.0f);
			dl->AddRect(ImVec2(x + s * .26f, y + s * .12f),
				ImVec2(x + s * .74f, y + s * .88f), edge, 3.0f, 0, 1.5f);
			for (int i = 0; i < 3; ++i)
				dl->AddRectFilled(
					ImVec2(x + s * .34f, y + s * (.30f + i * .14f)),
					ImVec2(x + s * .66f, y + s * (.36f + i * .14f)),
					IM_COL32(150, 156, 164, 255), 1.0f);
			break;
		}
		case IconType::Texture: {
			const ImU32 fill = IM_COL32(70, 96, 150, 255);
			const ImU32 edge = IM_COL32(36, 48, 82, 255);
			dl->AddRectFilled(ImVec2(x + s * .14f, y + s * .20f),
				ImVec2(x + s * .86f, y + s * .82f), fill, 3.0f);
			dl->AddRect(ImVec2(x + s * .14f, y + s * .20f),
				ImVec2(x + s * .86f, y + s * .82f), edge, 3.0f, 0, 1.5f);
			dl->AddCircleFilled(ImVec2(x + s * .32f, y + s * .36f),
				s * .07f, IM_COL32(255, 214, 90, 255), 12);
			dl->AddTriangleFilled(
				ImVec2(x + s * .20f, y + s * .74f),
				ImVec2(x + s * .44f, y + s * .44f),
				ImVec2(x + s * .62f, y + s * .74f),
				IM_COL32(120, 150, 96, 255));
			dl->AddTriangleFilled(
				ImVec2(x + s * .46f, y + s * .74f),
				ImVec2(x + s * .66f, y + s * .50f),
				ImVec2(x + s * .84f, y + s * .74f),
				IM_COL32(96, 128, 84, 255));
			break;
		}
		case IconType::Pak: {
			const ImU32 fill = IM_COL32(150, 108, 74, 255);
			const ImU32 edge = IM_COL32(92, 62, 38, 255);
			dl->AddRectFilled(ImVec2(x + s * .18f, y + s * .26f),
				ImVec2(x + s * .82f, y + s * .84f), fill, 3.0f);
			dl->AddRect(ImVec2(x + s * .18f, y + s * .26f),
				ImVec2(x + s * .82f, y + s * .84f), edge, 3.0f, 0, 1.5f);
			dl->AddLine(ImVec2(x + s * .18f, y + s * .55f),
				ImVec2(x + s * .82f, y + s * .55f), edge, 1.5f);
			dl->AddLine(ImVec2(x + s * .50f, y + s * .26f),
				ImVec2(x + s * .50f, y + s * .84f), edge, 1.5f);
			break;
		}
		default: {
			// The remaining node-type icons (light/camera/env/...):
			// a generic tinted page (they are not browser kinds).
			dl->AddRectFilled(ImVec2(x + s * .28f, y + s * .14f),
				ImVec2(x + s * .72f, y + s * .86f),
				IM_COL32(120, 120, 128, 200), 3.0f);
			break;
		}
		}
	}

} // namespace editor
