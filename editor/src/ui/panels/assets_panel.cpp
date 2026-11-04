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
	void EditorApp::RevealInExplorer(const std::string& absPath) {
		// explorer /select needs backslashes on Windows.
#ifdef _WIN32
		std::string win = absPath;
		for (char& c : win)
			if (c == '/')
				c = '\\';
		// "explorer /select," fails for non-existing paths: open the
		// parent folder instead, plain-open the file's dir as the last
		// resort. A non-zero "success" HINSTANCE just means it launched.
		std::error_code ec;
		if (std::filesystem::is_regular_file(absPath, ec)) {
			// /select,<path> pre-selects the file in an Explorer window.
			if (reinterpret_cast<INT_PTR>(::ShellExecuteA(nullptr, "open",
				"explorer", ("/select," + win).c_str(), nullptr,
				SW_SHOWNORMAL)) <= 32)
				Log("Reveal failed: '" + absPath + "'.");
		}
		else {
			const std::string dir =
				std::filesystem::path(absPath).parent_path().string();
			if (reinterpret_cast<INT_PTR>(::ShellExecuteA(nullptr, "open",
				dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
				Log("Reveal failed: '" + dir + "'.");
		}
#else
		Log("Reveal in Explorer is Windows-only.");
#endif
	}

	void EditorApp::DrawAssetGenericPage() {
		// Fallback for every browser entry without a specialized inspector.
		// Never read arbitrary files on every frame; keep previews bounded.
		namespace fs = std::filesystem;
		const fs::path path(m_assetsSel);
		if (m_assetGenericPath != m_assetsSel) {
			m_assetGenericPath = m_assetsSel;
			m_assetGenericPreview = ReadBrowserFilePreview(path);
			m_animationInspectDoc.reset();
			m_animationInspectError.clear();
			m_animationInspectWarnings.clear();
			m_animationInspectClip = 0;
			m_animationInspectTrack = 0;
		}
		// Animation assets have their own multi-clip inspector, not just a
		// truncated source preview. Parse once per asset selection.
		if (m_assetGenericPath == m_assetsSel) {
			const std::string ext = path.extension().string();
			if (ext == ".tourAnim" || ext == ".touranim") {
				if (!m_animationInspectDoc && m_animationInspectError.empty()) {
					m_animationInspectDoc.emplace();
					if (!tour::asset::LoadTourAnim(path.string(),
						*m_animationInspectDoc, m_animationInspectError,
						&m_animationInspectWarnings))
						m_animationInspectDoc.reset();
				}
			} else {
				m_animationInspectDoc.reset();
				m_animationInspectError.clear();
				m_animationInspectWarnings.clear();
			}
		}
		const auto& preview = m_assetGenericPreview;
		ImGui::TextWrapped("%s", path.filename().string().c_str());
		ImGui::TextDisabled("%s", m_assetsSel.c_str());
		ImGui::Separator();
		ImGui::Text("Type: %s", preview.isDirectory ? "Directory"
			: (path.extension().empty() ? "File" : path.extension().string().c_str()));
		if (preview.readOk && !preview.isDirectory)
			ImGui::Text("Size: %llu bytes",
				static_cast<unsigned long long>(preview.fileSize));
		if (!preview.error.empty())
			ImGui::TextWrapped("%s", preview.error.c_str());
		if (preview.isDirectory) {
			ImGui::TextDisabled("Double-click in Assets to open this folder.");
			return;
		}
		ImGui::Separator();
		if (path.extension() == ".tour" && preview.readOk) {
			if (ImGui::Button("Edit as prefab world..."))
				RequestOpenPrefab(path.string());
			ImGui::SameLine();
			ImGui::TextDisabled("Opens a separate live-edit world");
		}
		if (!preview.readOk)
			return;
		if (m_animationInspectDoc || !m_animationInspectError.empty()) {
			ImGui::SeparatorText("Animation clips");
			if (!m_animationInspectError.empty()) {
				ImGui::TextWrapped("Cannot parse animation: %s",
					m_animationInspectError.c_str());
				if (ImGui::Button("Retry loading animation")) {
					m_animationInspectError.clear();
					m_animationInspectDoc.reset();
				}
				return;
			}
			const auto& doc = *m_animationInspectDoc;
			ImGui::Text("Source skeleton: %s",
				doc.source.empty() ? "(not specified)" : doc.source.c_str());
			ImGui::Text("%zu clips", doc.clips.size());
			for (const auto& warning : m_animationInspectWarnings)
				ImGui::TextWrapped("Warning: %s", warning.c_str());
			if (doc.clips.empty()) {
				ImGui::TextDisabled("This animation file contains no clips.");
				return;
			}
			m_animationInspectClip = std::clamp(m_animationInspectClip,
				0, int(doc.clips.size()) - 1);
			if (ImGui::BeginListBox("##animationClips",
				ImVec2(-1, std::min(170.0f, 30.0f * doc.clips.size() + 8.0f)))) {
				for (int i = 0; i < int(doc.clips.size()); ++i) {
					const auto& clip = doc.clips[size_t(i)];
					ImGui::PushID(i);
					const std::string name = clip.name.empty()
						? "Unnamed clip " + std::to_string(i + 1) : clip.name;
					if (ImGui::Selectable(name.c_str(), i == m_animationInspectClip)) {
						m_animationInspectClip = i;
						m_animationInspectTrack = 0;
					}
					ImGui::PopID();
				}
				ImGui::EndListBox();
			}
			const auto& clip = doc.clips[size_t(m_animationInspectClip)];
			ImGui::Text("Duration: %.3f s  |  %.1f FPS",
				clip.duration, clip.fps);
			ImGui::Text("%zu tracks", clip.tracks.size());
			if (ImGui::Button("Preview clip in prefab world")) {
				size_t previewCluster = 0;
				bool hasSkeleton = false;
				if (m_prefab.world) {
					for (size_t i = 0; i < m_prefab.world->clusters.size(); ++i) {
						if (m_prefab.world->clusters[i]->hasSkeleton) {
							previewCluster = i;
							hasSkeleton = true;
							break;
						}
					}
				}
				if (hasSkeleton) {
					// Never modify the parent scene. Keep the original
					// prefab locals so switching and scrubbing remain pure.
					if (!m_prefabAnimationPreview ||
						m_prefabAnimationClusterIndex != previewCluster)
						m_prefabAnimationRest =
							m_prefab.world->clusters[previewCluster]->scene.localTransform;
					m_prefabAnimationClusterIndex = previewCluster;
					m_prefabAnimationClip = clip;
					m_prefabAnimationSource = path.string();
					m_prefabAnimationTime = 0.0f;
					m_prefabAnimationPlaying = true;
					m_prefabAnimationPreview = true;
					m_prefab.world->clusters[previewCluster]->SetAnimClip(clip);
					m_showPrefabWindow = true;
				} else {
					ImGui::OpenPopup("##animationPreviewHelp");
				}
			}
			if (ImGui::BeginPopup("##animationPreviewHelp")) {
				ImGui::TextWrapped("Open a .tour prefab with a skinned mesh "
					"and skeleton first (select its .tour asset and choose "
					"'Edit as prefab world...'). Then preview this clip. "
					"The clip joint IDs must match the prefab source skeleton.");
				ImGui::EndPopup();
			}
			ImGui::TextDisabled("Read-only tracks; select a clip to preview.");
			if (ImGui::BeginTable("##animationTracks", 4,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_ScrollY, ImVec2(0, 175))) {
				ImGui::TableSetupColumn("Joint");
				ImGui::TableSetupColumn("Channel");
				ImGui::TableSetupColumn("Keys");
				ImGui::TableSetupColumn("Interpolation");
				ImGui::TableHeadersRow();
				for (size_t i = 0; i < clip.tracks.size(); ++i) {
					const auto& track = clip.tracks[i];
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("%u %s", track.joint, track.name.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(track.channel == tour::asset::Channel::Rotate
						? "Rotate" : track.channel == tour::asset::Channel::Scale
						? "Scale" : "Translate");
					ImGui::TableNextColumn();
					ImGui::Text("%zu", track.keys.size());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(track.spline == tour::asset::Spline::Step
						? "Step" : track.spline == tour::asset::Spline::Linear
						? "Linear" : "Catmull-Rom");
				}
				ImGui::EndTable();
			}
			if (m_prefabAnimationPreview &&
				m_prefabAnimationSource == path.string()) {
				ImGui::TextDisabled("Playback controls are in the Prefab World pane.");
			}
			return;
		}
		if (!preview.isText) {
			ImGui::TextDisabled("Binary or non-UTF-8 file (metadata only).");
			return;
		}
		if (path.extension() == ".shape") {
			// Per-selection editing state: changes are only persisted on Save.
			// A truncated preview cannot safely round-trip the source.
			static std::string shapePath;
			static tour::asset::ShapeAsset shape;
			static std::string shapeError;
			static bool shapeLoaded = false;
			static bool shapeDirty = false;
			if (shapePath != m_assetsSel) {
				shapePath = m_assetsSel;
				shapeLoaded = !preview.truncated &&
					tour::asset::ParseShapeAsset(preview.text, shape, shapeError);
				shapeDirty = false;
				if (preview.truncated)
					shapeError = "Shape file exceeds the 64 KiB inspector limit.";
			}
			if (!shapeLoaded) {
				ImGui::TextWrapped("Shape parse error: %s", shapeError.c_str());
				return;
			}
			ImGui::Text("Shape: %s%s",
				tour::asset::ShapeKindName(shape.kind),
				shapeDirty ? " (unsaved)" : "");
			// Lightweight CPU preview, intentionally independent of the
			// Vulkan scene: usable before line/point draw pipelines exist.
			// Project onto a fixed oblique view (X + 0.4 Z, Y - 0.3 Z).
			{
				tour::asset::ShapeGeometry geometry;
				std::string tessError;
				if (tour::asset::TessellateShape(shape, geometry, tessError)
					&& !geometry.vertices.empty()) {
					const float width = std::max(80.0f,
						std::min(ImGui::GetContentRegionAvail().x, 440.0f));
					const float height = 180.0f;
					const ImVec2 origin = ImGui::GetCursorScreenPos();
					const size_t maxScan = std::min(geometry.vertices.size(), size_t(200000));
					float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
					auto project = [](const tour::asset::ShapePoint& p) {
						return ImVec2(p.x + 0.4f*p.z, p.y - 0.3f*p.z);
					};
					for (size_t i=0; i<maxScan; ++i) {
						const ImVec2 p = project(geometry.vertices[i]);
						minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
						minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
					}
					const float scale = std::min((width-24.0f)/std::max(0.01f,maxX-minX),
						(height-24.0f)/std::max(0.01f,maxY-minY));
					auto screen = [&](uint32_t idx) {
						const ImVec2 p = project(geometry.vertices[idx]);
						return ImVec2(origin.x + width*0.5f +
							(p.x-(minX+maxX)*0.5f)*scale,
							origin.y + height*0.5f -
							(p.y-(minY+maxY)*0.5f)*scale);
					};
					ImDrawList* draw = ImGui::GetWindowDrawList();
					draw->AddRectFilled(origin, ImVec2(origin.x+width,origin.y+height),
						IM_COL32(25,28,34,255), 5.0f);
					const ImU32 tint = IM_COL32(110,205,220,255);
					if (geometry.topology == tour::asset::ShapeGeometry::Topology::Points) {
						for (size_t i=0; i<std::min(geometry.vertices.size(),size_t(2048)); ++i)
							draw->AddCircleFilled(screen(static_cast<uint32_t>(i)),2.0f,tint);
					} else if (geometry.topology == tour::asset::ShapeGeometry::Topology::Lines) {
						for (size_t i=0; i+1<geometry.indices.size() && i<8192; i+=2)
							draw->AddLine(screen(geometry.indices[i]),
								screen(geometry.indices[i+1]),tint,1.4f);
					} else {
						for (size_t i=0; i+2<geometry.indices.size() && i<12000; i+=3) {
							const ImVec2 a=screen(geometry.indices[i]);
							const ImVec2 b=screen(geometry.indices[i+1]);
							const ImVec2 c=screen(geometry.indices[i+2]);
							draw->AddLine(a,b,tint);
							draw->AddLine(b,c,tint);
							draw->AddLine(c,a,tint);
						}
					}
					ImGui::Dummy(ImVec2(width,height));
				} else if (!tessError.empty())
					ImGui::TextDisabled("Preview: %s", tessError.c_str());
			}
			if (shape.kind == tour::asset::ShapeKind::Spline)
				shapeDirty |= ImGui::Checkbox("Closed", &shape.closed);
			int segments = static_cast<int>(shape.segments);
			if (ImGui::SliderInt("Segments", &segments, 1, 256)) {
				shape.segments = static_cast<uint32_t>(segments);
				shapeDirty = true;
			}
			if (shape.kind == tour::asset::ShapeKind::Sphere ||
				shape.kind == tour::asset::ShapeKind::Cylinder ||
				shape.kind == tour::asset::ShapeKind::Cone) {
				shapeDirty |= ImGui::DragFloat("Radius", &shape.radius, 0.02f, 0.01f, 10000.0f);
				if (shape.kind != tour::asset::ShapeKind::Sphere)
					shapeDirty |= ImGui::DragFloat("Height", &shape.height, 0.02f, 0.01f, 10000.0f);
			}
			if (shape.kind == tour::asset::ShapeKind::Spline ||
				shape.kind == tour::asset::ShapeKind::PointCloud) {
				ImGui::Text("Control points: %zu", shape.points.size());
				if (ImGui::Button("Add point") && shape.points.size() < 100000) {
					shape.points.push_back(shape.points.empty()
						? tour::asset::ShapePoint{} : shape.points.back());
					shapeDirty = true;
				}
				if (shape.points.size() > 256)
					ImGui::TextDisabled("Showing first 256 points (of %zu).", shape.points.size());
				ImGui::BeginChild("##shape_points", ImVec2(0, 220), true);
				const size_t shown = std::min(shape.points.size(), size_t(256));
				for (size_t i = 0; i < shown; ++i) {
					ImGui::PushID(static_cast<int>(i));
					float xyz[3] = {shape.points[i].x, shape.points[i].y, shape.points[i].z};
					if (ImGui::DragFloat3("XYZ", xyz, 0.02f)) {
						shape.points[i] = {xyz[0], xyz[1], xyz[2]};
						shapeDirty = true;
					}
					ImGui::SameLine();
					const bool canRemove = shape.points.size() >
						(shape.kind == tour::asset::ShapeKind::PointCloud ? 1u :
						(shape.closed ? 3u : 2u));
					ImGui::BeginDisabled(!canRemove);
					if (ImGui::SmallButton("X")) {
						shape.points.erase(shape.points.begin() + static_cast<std::ptrdiff_t>(i));
						shapeDirty = true;
						ImGui::EndDisabled();
						ImGui::PopID();
						break;
					}
					ImGui::EndDisabled();
					ImGui::PopID();
				}
				ImGui::EndChild();
			}
			std::string validation;
			const bool valid = tour::asset::ValidShape(shape, validation);
			if (!valid) ImGui::TextWrapped("Invalid shape: %s", validation.c_str());
			ImGui::BeginDisabled(!shapeDirty || !valid);
			if (ImGui::Button("Save shape")) {
				// Only this authored source file is written; no cooked
				// geometry or GPU resource is modified by the inspector.
				std::ofstream file(path, std::ios::binary | std::ios::trunc);
				file << tour::asset::SerializeShapeAsset(shape);
				file.close();
				if (file) {
					shapeDirty = false;
					m_assetGenericPreview = ReadBrowserFilePreview(path);
					Log("Saved shape: " + path.generic_string());
				} else {
					Log("Failed to save shape: " + path.generic_string());
				}
			}
			ImGui::EndDisabled();
			if (shape.kind != tour::asset::ShapeKind::Spline &&
				shape.kind != tour::asset::ShapeKind::PointCloud) {
				ImGui::SameLine();
				ImGui::BeginDisabled(!valid || shapeDirty);
				if (ImGui::Button("Export OBJ...")) {
					std::string obj, exportError;
					const fs::path destination = UniqueAssetCreatePath(
						path.parent_path(), path.stem().string(), ".obj");
					if (tour::asset::ShapeToObj(shape, obj, exportError)
						&& !destination.empty()) {
						std::ofstream file(destination, std::ios::binary);
						file << obj;
						file.close();
						if (file)
							Log("Exported procedural OBJ: " + destination.generic_string());
						else
							Log("Failed to export OBJ: " + destination.generic_string());
					} else {
						Log("OBJ export failed: " + exportError);
					}
				}
				ImGui::EndDisabled();
			}
			ImGui::SameLine();
			if (ImGui::Button("Revert")) {
				shapeLoaded = tour::asset::ParseShapeAsset(
					preview.text, shape, shapeError);
				shapeDirty = false;
			}
			return;
		}
		if (ClassifyBrowserInspector(path) == BrowserInspectorKind::SpriteSet) {
			::tour::asset::SpriteSet sheet;
			std::string parseError;
			if (!::tour::asset::ParseSpriteSetText(preview.text, sheet, parseError)) {
				ImGui::TextWrapped("SpriteSet error: %s", parseError.c_str());
			} else {
				ImGui::Text("Texture: %s (%u x %u)", sheet.texture.c_str(), sheet.width, sheet.height);
				ImGui::Text("%zu sprites / %zu animations", sheet.sprites.size(), sheet.animations.size());
				const float width = std::min(360.0f, ImGui::GetContentRegionAvail().x);
				const float scale = width / float(sheet.width);
				const float height = float(sheet.height) * scale;
				const ImVec2 origin = ImGui::GetCursorScreenPos();
				ImDrawList* draw = ImGui::GetWindowDrawList();
				draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
					IM_COL32(28, 31, 37, 255));
				for (const auto& sprite : sheet.sprites) {
					const auto& r = sprite.rect;
					const ImVec2 a(origin.x + r.x * scale, origin.y + r.y * scale);
					const ImVec2 b(a.x + r.width * scale, a.y + r.height * scale);
					draw->AddRect(a, b, IM_COL32(120, 220, 170, 255));
					if (r.width * scale >= 40)
						draw->AddText(a, IM_COL32(235, 240, 245, 255), sprite.name.c_str());
				}
				ImGui::Dummy(ImVec2(width, height));
				if (ImGui::BeginTable("##sprites", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
					ImVec2(0, 160))) {
					ImGui::TableSetupColumn("Sprite");
					ImGui::TableSetupColumn("Rect");
					ImGui::TableSetupColumn("Pivot");
					ImGui::TableHeadersRow();
					for (const auto& sprite : sheet.sprites) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(sprite.name.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%u,%u %ux%u", sprite.rect.x, sprite.rect.y,
							sprite.rect.width, sprite.rect.height);
						ImGui::TableNextColumn(); ImGui::Text("%.2f, %.2f", sprite.pivotX, sprite.pivotY);
					}
					ImGui::EndTable();
				}
			}
		}
		if (path.extension() == ".statemachine") {
			if (ImGui::Button("Edit state machine graph...")) {
				if (!m_stateGraph.show || m_stateGraph.Path() != path.string())
					m_stateGraph.Open(path.string());
				m_stateGraph.show = true;
			}
			if (m_stateGraph.Dirty() && m_stateGraph.Path() == path.string())
				ImGui::TextDisabled("This graph has unsaved edits in the graph window.");
		}
		ImGui::TextDisabled("Text preview (read-only)%s",
			preview.truncated ? " - first 64 KiB" : "");
		ImGui::BeginChild("##assettextpreview", ImVec2(0, 0), true,
			ImGuiWindowFlags_HorizontalScrollbar);
		ImGui::TextUnformatted(preview.text.c_str(),
			preview.text.c_str() + preview.text.size());
		ImGui::EndChild();
	}

	void EditorApp::DrawAssetReadonlyPage() {
		// READ-ONLY asset inspector (a .material/.mesh/.gicook/.lmcook click
		// in the asset browser): view parameters only — edits happen on
		// SCENES (the tour-level overrides/assignments), never on the
		// derived/cook assets (derived assets regenerate on reimport; cook
		// assets rebake through the bake execution). The loaded asset caches
		// per selection (never per frame: .mesh blobs can be megabytes).
		// EXCEPTION — .texture (user-authored, not derived): its IMPORT
		// SETTINGS are the authoring, so LDR sources edit them here (a
		// KTX2 source pre-decided them in its header — read-only rows).
		const auto kind = ClassifyBrowserInspector(m_assetsSel);
		const bool wantTex = kind == BrowserInspectorKind::Texture;
		const bool wantMat = kind == BrowserInspectorKind::Material;
		const bool wantMesh = kind == BrowserInspectorKind::Geometry;
		const bool wantGi = kind == BrowserInspectorKind::GiCook;
		const bool wantLm = kind == BrowserInspectorKind::LmCook;
		const bool wantCm = kind == BrowserInspectorKind::Cubemap;
		const bool wantGiv = kind == BrowserInspectorKind::Givolume;
		if (!wantTex && !wantMat && !wantMesh && !wantGi && !wantLm && !wantCm && !wantGiv)
			return;
		// Cache refresh on selection change (or a failed load retries never:
		// a re-derive/re-bake + re-click reloads).
		if (m_assetInspectPath != m_assetsSel) {
			m_assetInspectPath = m_assetsSel;
			m_assetInspectMat.reset();
			m_assetInspectMesh.reset();
			m_assetInspectGiCook.reset();
			m_assetInspectLmCook.reset();
			m_assetInspectCubemap.reset();
			m_assetInspectGivolume.reset();
			m_assetInspectTex.reset();
			m_assetInspectTexInfoOk = false;
			std::string err;
			if (wantTex) {
				m_assetInspectTex.emplace();
				if (!smvk::assets::LoadTextureAsset(m_assetsSel,
					*m_assetInspectTex, err)) {
					Log("Assets: texture inspect failed: " + err);
					m_assetInspectTex.reset();
				}
				else {
					// Probe the SOURCE (beside the .texture): the read-only
					// header facts for KTX2, the dims for LDR.
					std::ifstream in(std::filesystem::path(m_assetsSel)
						.parent_path() / m_assetInspectTex->source,
						std::ios::binary);
					if (in) {
						std::vector<uint8_t> bytes(
							(std::istreambuf_iterator<char>(in)),
							std::istreambuf_iterator<char>());
						m_assetInspectTexInfoOk = ProbeTextureSource(
							bytes.data(), bytes.size(),
							m_assetInspectTexInfo);
					}
					if (!m_assetInspectTexInfoOk)
						Log("Assets: texture source not probeable: "
							+ m_assetInspectTex->source);
				}
			}
			else if (wantMat) {
				m_assetInspectMat.emplace();
				if (!smvk::assets::LoadMaterialAsset(m_assetsSel,
					*m_assetInspectMat, err)) {
					Log("Assets: material inspect failed: " + err);
					m_assetInspectMat.reset();
				}
			}
			else if (wantMesh) {
				m_assetInspectMesh.emplace();
				if (!smvk::assets::LoadMeshAsset(m_assetsSel,
					*m_assetInspectMesh, err)) {
					Log("Assets: mesh inspect failed: " + err);
					m_assetInspectMesh.reset();
				}
			}
			else if (wantGi) {
				m_assetInspectGiCook.emplace();
				if (!smvk::cook::LoadGiCookAsset(m_assetsSel,
					*m_assetInspectGiCook, err)) {
					Log("Assets: GI cook inspect failed: " + err);
					m_assetInspectGiCook.reset();
				}
			}
			else if (wantCm) {
				m_assetInspectCubemap.emplace();
				if (!smvk::cook::LoadCubemapAsset(m_assetsSel,
					*m_assetInspectCubemap, err)) {
					Log("Assets: cubemap inspect failed: " + err);
					m_assetInspectCubemap.reset();
				}
			}
			else if (wantGiv) {
				m_assetInspectGivolume.emplace();
				if (!smvk::cook::LoadGivolumeAsset(m_assetsSel,
					*m_assetInspectGivolume, err)) {
					Log("Assets: GI volume inspect failed: " + err);
					m_assetInspectGivolume.reset();
				}
			}
			else {
				m_assetInspectLmCook.emplace();
				if (!smvk::cook::LoadLightmapCookAsset(m_assetsSel,
					*m_assetInspectLmCook, err)) {
					Log("Assets: lightmap cook inspect failed: " + err);
					m_assetInspectLmCook.reset();
				}
			}
		}
		// TEXTURE ASSET page: user-authored import settings (the authoring
		// surface, not read-only). KTX2 sources show the header-decided rows
		// READ-ONLY; LDR sources edit them (saved into the .texture; applied
		// at the next import/cook — materials referencing this asset pick the
		// settings up through ImportTextureAssetToCluster).
		if (m_assetInspectTex) {
			smvk::assets::TextureAsset& t = *m_assetInspectTex;
			const TextureSourceInfo& info = m_assetInspectTexInfo;
			const bool infoOk = m_assetInspectTexInfoOk;
			ImGui::Text("Texture asset: '%s'",
				t.name.empty()
				? std::filesystem::path(m_assetsSel).stem().string().c_str()
				: t.name.c_str());
			ImGui::TextDisabled("guid %s", t.guid.c_str());
			ImGui::TextDisabled("source %s (%s)", t.source.c_str(),
				infoOk ? (info.isKtx2 ? "KTX2" : "LDR image") : "not probeable");
			if (infoOk)
				ImGui::TextDisabled("%ux%u%s", info.width, info.height,
					info.isKtx2 ? "" : (info.components
						? (info.components == 4 ? " RGBA" : " RGB") : ""));
			ImGui::Separator();
			if (infoOk && info.isKtx2) {
				// The .ktx2 pre-decided these in its header — read-only rows.
				ImGui::TextDisabled("colorSpace: %s (from the .ktx2 header)",
					info.srgb ? "sRGB" : "linear");
				ImGui::TextDisabled("format: %s (vkFormat %u)",
					info.bcn ? "BCn block-compressed"
					: (info.vkFormat == 37 ? "rgba8"
						: (info.vkFormat == 43 ? "rgba8 sRGB" : "other")),
					info.vkFormat);
				if (info.basis)
					ImGui::TextDisabled("payload: Basis-family (the cook "
						"transcodes to per-semantic BCn)");
				ImGui::TextDisabled("mip levels: %u (the .ktx2 chain)",
					info.mipLevels);
				ImGui::Separator();
				ImGui::TextWrapped("The KTX2 source carries its own encoding: "
					"colorSpace, format and mip levels are read-only. The "
					"sampler rows still apply.");
			}
			else {
				// LDR source: the import settings ARE the authoring (the
				// sampler rows below are shared with KTX2 sources).
				bool changed = false;
				{
					const char* items[] = { "sRGB (color)", "linear (data)" };
					int cs = t.colorspace == 1 ? 1 : 0;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::Combo("colorSpace##texasset", &cs, items, 2)) {
						t.colorspace = cs;
						changed = true;
					}
				}
				{
					const char* items[] = { "rgba8 (uncompressed)", "bc1 (RGB)",
						"bc3 (RGBA)", "bc4 (R)", "bc5 (RG)", "bc7 (RGBA hq)" };
					int fmt = std::clamp(t.format, 0, 5);
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::Combo("format##texasset", &fmt, items, 6)) {
						t.format = fmt;
						changed = true;
					}
				}
				{
					int mips = t.mipLevels;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::InputInt("mip levels (0 = auto)##texasset",
						&mips, 1, 1)) {
						t.mipLevels = std::clamp(mips, 0, 32);
						changed = true;
					}
				}
				if (changed) {
					std::string serr;
					if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
						Log("Texture asset saved: "
							+ std::filesystem::path(m_assetsSel)
							.filename().string()
							+ " (applies at the next import/cook).");
					else
						Log("Texture asset save failed: " + serr);
				}
			}
			// Sampler rows apply to BOTH source kinds (never baked into a
			// KTX2 payload; they ride the pak's per-texture sampler record).
			{
				const char* items[] = { "linear", "nearest" };
				int f = t.filter != 0 ? 1 : 0;
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("filter##texassetsmp", &f, items, 2)) {
					t.filter = f;
					std::string serr;
					if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
						Log("Texture asset saved: "
							+ std::filesystem::path(m_assetsSel)
							.filename().string() + " (sampler).");
				}
				const char* wraps[] = { "repeat", "clamp", "mirror" };
				int w = std::clamp(t.wrap, 0, 2);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("wrap##texassetsmp", &w, wraps, 3)) {
					t.wrap = w;
					std::string serr;
					if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
						Log("Texture asset saved: "
							+ std::filesystem::path(m_assetsSel)
							.filename().string() + " (sampler).");
				}
			}
			ImGui::Separator();
			ImGui::TextWrapped("Materials reference this asset by file name: "
				"tex.baseColor=<name>.texture in a .material (the import "
				"routes through these settings). The cook embeds the "
				"converted payload as KTX2 in the pak.");
			return;
		}
		ImGui::TextDisabled("Asset (read-only) - edits happen on scenes");
		ImGui::Separator();
		if (m_assetInspectMat) {
			const smvk::assets::MaterialAsset& m = *m_assetInspectMat;
			ImGui::Text("Material: '%s'", m.name.c_str());
			ImGui::TextDisabled("guid %s", m.guid.c_str());
			const glm::vec4& bc = m.info.baseColorFactor;
			ImGui::ColorEdit4("Base color##ro", const_cast<float*>(&bc.x),
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker
				| ImGuiColorEditFlags_NoAlpha);
			ImGui::TextDisabled("metallic: %.2f  roughness: %.2f",
				m.info.metallicFactor, m.info.roughnessFactor);
			const glm::vec3& em = m.info.emissiveFactor;
			ImGui::ColorEdit3("Emissive##ro", const_cast<float*>(&em.x),
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker);
			const char* alpha = (m.info.alphaMode == smvk::AlphaMode::Mask)
				? "mask" : (m.info.alphaMode == smvk::AlphaMode::Blend)
				? "blend" : "opaque";
			ImGui::TextDisabled("alpha: %s (%.2f cutoff)  %s",
				alpha, m.info.alphaCutoff,
				m.info.doubleSided ? "double-sided" : "single-sided");
			ImGui::Separator();
			ImGui::Text("Textures");
			auto tex = [&](const char* slot,
				const smvk::assets::MaterialAsset::TexRef& t) {
				ImGui::TextDisabled("%-18s %s", slot,
					t.path.empty() ? "-" : t.path.c_str());
			};
			tex("baseColor", m.baseColor);
			tex("metallicRoughness", m.metallicRoughness);
			tex("normal", m.normalTex);
			tex("occlusion", m.occlusion);
			tex("emissive", m.emissiveTex);
			ImGui::Separator();
			ImGui::TextDisabled("Assign: drag onto a viewport (selection) or "
				"use a scene's material override.");
		}
		else if (m_assetInspectMesh) {
			const smvk::assets::MeshAsset& k = *m_assetInspectMesh;
			ImGui::Text("Mesh: '%s'",
				std::filesystem::path(m_assetsSel).stem().string().c_str());
			uint64_t tris = 0, verts = 0;
			for (const auto& g : k.geometry) {
				verts += g.positions.size();
				tris += g.indices.size() / 3;
			}
			ImGui::TextDisabled("%u nodes, %u instances, %zu blobs, "
				"%llu verts, %llu tris",
				uint32_t(k.nodes.size()), uint32_t(k.instances.size()),
				k.geometry.size(),
				(unsigned long long)verts, (unsigned long long)tris);
			uint32_t lights = uint32_t(k.lights.size());
			uint32_t cams = uint32_t(k.cameras.size());
			ImGui::TextDisabled("%u light(s), %u camera(s)", lights, cams);
			ImGui::Separator();
			ImGui::Text("Hierarchy (first 32)");
			ImGui::BeginChild("##meshnodes", ImVec2(0, 0), true);
			const size_t n = std::min(k.nodes.size(), size_t(32));
			for (size_t i = 0; i < n; ++i) {
				ImGui::TextDisabled("%2zu p%-4d %s", i, k.nodes[i].parent,
					k.names[i].empty() ? "(node)" : k.names[i].c_str());
			}
			if (k.nodes.size() > n)
				ImGui::TextDisabled("... (%zu more)", k.nodes.size() - n);
			ImGui::EndChild();
			ImGui::Separator();
			ImGui::TextDisabled("Drop onto a viewport to add a scene "
				"reference; the tour binds it by GUID.");
			if (m_world && !m_cookRunning && ImGui::Button("Add to scene")) {
				if (AddSceneReference(m_assetsSel, {}, true))
					Log("Assets: added geometry asset '" + m_assetsSel
						+ "' (asset bind).");
			}
		}
		else if (m_assetInspectGiCook) {
			// GI cook asset (settings + blob result): the Bake GI execution
			// that fills the blob is the follow-up — this page only reports
			// WHAT would bake and WHERE the result points.
			const smvk::cook::GiCookAsset& c = *m_assetInspectGiCook;
			ImGui::Text("GI cook: '%s'", c.name.c_str());
			ImGui::TextDisabled("guid %s", c.guid.c_str());
			ImGui::TextDisabled("scene: %s",
				c.scene.empty() ? "(unbound)" : c.scene.c_str());
			ImGui::TextDisabled("convex volumes: %s   (the world-field "
				"flag is retired with the octree)",
				c.giVolumes ? "on" : "off");
			ImGui::Separator();
			if (c.blobGuid.empty()) {
				ImGui::TextDisabled("blob: unbaked (no result yet)");
			}
			else {
				const bool stale = smvk::cook::GiCookBlobStale(c);
				ImGui::TextDisabled("blob: %s (%s)%s", c.blobGuid.c_str(),
					c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
					stale ? "  STALE (settings changed)" : "  fresh");
			}
		}
		else if (m_assetInspectLmCook) {
			// Lightmap cook asset (settings + blob result): same contract —
			// the atlas bake that fills the blob is the follow-up.
			const smvk::cook::LightmapCookAsset& c = *m_assetInspectLmCook;
			ImGui::Text("Lightmap cook: '%s'", c.name.c_str());
			ImGui::TextDisabled("guid %s", c.guid.c_str());
			ImGui::TextDisabled("scene: %s",
				c.scene.empty() ? "(unbound)" : c.scene.c_str());
			ImGui::TextDisabled("density: %.3f m/texel  lobes: %u",
				c.lmMetersPerTexel, c.lmLobes);
			ImGui::TextDisabled("atlas width: %u  rays: %u  proxy: %d",
				c.lmAtlasWidth, c.lmRays, c.lmProxyRes);
			if (c.lmExcludeSmall > 0.0f || !c.lmExcludeKw.empty()) {
				ImGui::TextDisabled("exclude: small < %.3f  kw: '%s'",
					c.lmExcludeSmall, c.lmExcludeKw.c_str());
			}
			ImGui::TextDisabled("sun: az %.1f el %.1f int %.0f  sky %.0f",
				c.sunAzimuth, c.sunElevation, c.sunIntensity,
				c.skyIntensity);
			ImGui::Separator();
			if (c.blobGuid.empty()) {
				ImGui::TextDisabled("blob: unbaked (no result yet)");
			}
			else {
				const bool stale = smvk::cook::LightmapCookBlobStale(c);
				ImGui::TextDisabled("blob: %s (%s)%s", c.blobGuid.c_str(),
					c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
					stale ? "  STALE (settings changed)" : "  fresh");
			}
		}
		else if (m_assetInspectCubemap) {
			const smvk::cook::CubemapAsset& c = *m_assetInspectCubemap;
			ImGui::Text("Cubemap: '%s'", c.name.c_str());
			ImGui::TextDisabled("guid %s", c.guid.c_str());
			ImGui::TextDisabled("scene: %s",
				c.scene.empty() ? "(unbound)" : c.scene.c_str());
			ImGui::TextDisabled("capture: %s  size: %d  rays: %d",
				c.capture.empty() ? "(scene stem)" : c.capture.c_str(),
				c.size, c.rays);
			ImGui::Separator();
			if (c.blobGuid.empty()) {
				ImGui::TextDisabled("blob: unbaked (no result yet)");
			}
			else {
				const bool stale = smvk::cook::CubemapBlobStale(c);
				ImGui::TextDisabled("blob: %s (%s)%s", c.blobGuid.c_str(),
					c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
					stale ? "  STALE (settings changed)" : "  fresh");
			}
		}
		else if (m_assetInspectGivolume) {
			const smvk::cook::GivolumeAsset& c = *m_assetInspectGivolume;
			ImGui::Text("GI volume: '%s'", c.name.c_str());
			ImGui::TextDisabled("guid %s", c.guid.c_str());
			ImGui::TextDisabled("scene: %s",
				c.scene.empty() ? "(unbound)" : c.scene.c_str());
			ImGui::TextDisabled("node: %u  priority: %u", c.node, c.priority);
			ImGui::Separator();
			if (c.blobGuid.empty()) {
				ImGui::TextDisabled("blob: unbaked (no result yet)");
			}
			else {
				const bool stale = smvk::cook::GivolumeBlobStale(c);
				ImGui::TextDisabled("blob: %s (%s)%s", c.blobGuid.c_str(),
					c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
					stale ? "  STALE (settings changed)" : "  fresh");
			}
		}
		else {
			ImGui::TextDisabled("(asset load failed - see the console)");
		}
	}

	void EditorApp::DrawImportableTexturePage() {
		// IMPORTABLE SOURCE page: a supported image format selected in the
		// browser but not yet imported as a .texture asset. Probes the file
		// (dims + kind) and offers the import button — the .texture wrap
		// carries the import settings (colorSpace/format/mipLevels/filter/
		// wrap) from here on. A project is required (the .texture lands
		// beside the source, adopted into the assetdb).
		namespace fs = std::filesystem;
		ImGui::Text("Texture source: '%s'",
			fs::path(m_assetsSel).filename().string().c_str());
		ImGui::TextDisabled("%s", m_assetsSel.c_str());
		ImGui::Separator();
		// Probe on selection change (cached like the asset pages).
		if (m_assetInspectPath != m_assetsSel) {
			m_assetInspectPath = m_assetsSel;
			m_assetInspectTex.reset();
			m_assetInspectTexInfoOk = false;
			std::ifstream in(m_assetsSel, std::ios::binary);
			if (in) {
				std::vector<uint8_t> bytes(
					(std::istreambuf_iterator<char>(in)),
					std::istreambuf_iterator<char>());
				m_assetInspectTexInfoOk = ProbeTextureSource(
					bytes.data(), bytes.size(), m_assetInspectTexInfo);
			}
			if (!m_assetInspectTexInfoOk)
				Log("Assets: texture source not probeable: " + m_assetsSel);
		}
		if (m_assetInspectTexInfoOk) {
			const TextureSourceInfo& info = m_assetInspectTexInfo;
			ImGui::TextDisabled("%ux%u %s", info.width, info.height,
				info.isKtx2 ? "KTX2 (settings read-only at import)"
				: (info.components == 4 ? "RGBA" : "RGB"));
			ImGui::TextWrapped("Importing wraps this source in a .texture "
				"asset: import settings (colorSpace, format, mip levels, "
				"filter, wrap) %s, identity (.meta + assetdb), and "
				"materials reference it as tex.<slot>=<name>.texture.",
				info.isKtx2
				? "read from the .ktx2 header (it pre-decided them)"
				: "you author in the Inspector after the import");
		}
		ImGui::Separator();
		ImGui::BeginDisabled(m_projectRoot.empty());
		if (ImGui::Button("Import as .texture")) {
			// The .texture lands beside the source (same dir — the copy in
			// CreateTextureAsset is a no-op for an in-project file).
			const std::string dir =
				fs::path(m_assetsSel).parent_path().generic_string();
			CreateTextureAsset(m_assetsSel, dir);
		}
		ImGui::EndDisabled();
		if (m_projectRoot.empty() && ImGui::IsItemHovered())
			ImGui::SetTooltip("Open or create a project first (File > New / "
				"Open Project...).");
	}

	// DCC edits the authoring file; the explicit build stage exports
	// a validated glTF publication. The content server observes the
	// atomic replace and routes it through normal selective reimport.
	void EditorApp::OpenAssetInDcc(const std::string& selected) {
		DccAsset asset;
		std::string err;
		if (!ResolveDccAsset(selected, asset, err)) {
			Log("Open in DCC: " + err);
			return;
		}
		if (!NativeOpenExternalDocument(asset.document, err)) {
			Log("Open in DCC: " + err + " (" + asset.document + ")");
			return;
		}
		Log("Open in DCC: '" + asset.document + "'"
			+ (asset.linked
				? " (linked authoring source; use Publish glTF to rebuild)"
				: " (no DCC source link; opens the published source directly)"));
	}

	void EditorApp::LinkAssetToDcc(const std::string& selected) {
		std::string published, err;
		if (!ResolveDccPublishedAsset(selected, published, err)
				|| !IsDccPublishable(published)) {
			Log("Link DCC source: " + (err.empty()
				? "select a published mesh/image or a derived asset" : err));
			return;
		}
		std::string source;
		if (!NativeOpenFile(source, false, true))
			return;
		if (!LinkDccSource(selected, source, err)) {
			Log("Link DCC source: " + err);
			return;
		}
		Log("Linked DCC authoring document: '" + source
			+ "' -> published asset '" + published + "'.");
	}

	void EditorApp::UnlinkAssetFromDcc(const std::string& selected) {
		std::string err;
		if (!UnlinkDccSource(selected, err))
			Log("Unlink DCC source: " + err);
		else
			Log("DCC source unlinked for '" + selected + "'.");
	}

	void EditorApp::PublishAssetFromDcc(const std::string& selected) {
		if (m_dccPublishRunning || m_cookRunning) {
			Log("Publish glTF: a DCC publish or cook is already running.");
			return;
		}
		DccAsset asset;
		std::string err;
		if (!ResolveDccAsset(selected, asset, err)) {
			Log("Publish glTF: " + err);
			return;
		}
		const char* basePath = SDL_GetBasePath();
		const std::filesystem::path publisher = std::filesystem::path(
			basePath ? basePath : ".") / "tools" / "dcc" / "publish_gltf.py";
		if (!std::filesystem::is_regular_file(publisher)) {
			Log("Publish glTF: runner script not staged beside Editor: "
				+ publisher.generic_string());
			return;
		}
		m_dccPublishRunning = true;
		const std::string output = asset.published;
		const std::string document = asset.document;
		const bool needsLink = asset.fromNative;
		if (!m_fibers.Post("dcc-publish",
				[asset, script = publisher.generic_string()]() {
					std::string error;
					if (!RunDccPublish(asset, script, error))
						throw std::runtime_error(error);
				},
				[this, output, document, needsLink](
						const FiberBackend::Completion& completion) {
					m_dccPublishRunning = false;
					if (completion.ok) {
						if (needsLink) {
							std::string linkError;
							if (!LinkDccSource(output, document, linkError))
								Log("Publish glTF: could not persist DCC source "
									"association: " + linkError);
						}
						Log("Publish glTF succeeded: '" + output
							+ "' — awaiting content-server hot reload.");
					}
					else
						Log("Publish glTF failed: " + completion.error
							+ " — previous publication retained.");
				})) {
			m_dccPublishRunning = false;
			Log("Publish glTF: editor worker pool is not available.");
			return;
		}
		Log("Publish glTF: exporting '" + asset.document + "' -> '"
			+ asset.published + "' (background build stage).");
	}

	void EditorApp::DrawAssetsPanel() {
		// Asset browser: a GRID directory explorer of the project folder
		// (Unity Project window style). Breadcrumb + Up; folders first;
		// SVG icon tiles (m_icons via ImDrawList::AddImage, drawn shapes
		// as the fallback); a green badge marks assets with a registered
		// GUID (assets.db); click selects, double-click enters folders /
		// opens scenes; Import + Rescan ride the header.
		if (m_projectRoot.empty()) {
			ImGui::TextDisabled("Assets: open a project first (File > New / "
				"Open Project...).");
			return;
		}
		namespace fs = std::filesystem;
		const auto clearAssetSelection = [&]() {
			ClearAssetBrowserSelection(
				m_assetsSel, m_assetsSelSet, m_assetsAnchor);
		};
		// Creating/reimporting an internal artifact can select it from code.
		// Drop that stale browser selection: the corresponding tile is hidden.
		if (!m_assetsSel.empty()) {
			std::error_code selEc;
			if (std::filesystem::is_regular_file(m_assetsSel, selEc)
				&& HideAssetBrowserFile(m_assetsSel))
				clearAssetSelection();
		}
		const auto navigateAssetDirectory = [&](const std::string& path) {
			m_assetsDir = path;
			m_assetsExpandGltf.clear();
			m_assetsExpandTour.clear();
			m_assetsFilter = 0;
			clearAssetSelection();
		};
		ImGui::BeginChild(
			"##assets_root",
			ImVec2(0, 0),
			false,
			ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse
		);

		// Navigation state heal: home = <root>/content, but the project
		// root itself is also a valid browsing location.
		std::error_code ec;
		if (m_assetsDir.empty()
			|| !fs::is_directory(fs::path(m_assetsDir), ec)
			|| (!InProjectDir(m_assetsDir)
				&& !AssetBrowserAtProjectRoot(m_projectRoot, m_assetsDir))) {
			navigateAssetDirectory(
				(fs::path(m_projectRoot) / "content").generic_string());
		}

		// ---- OS drag-and-drop target (Windows Explorer -> browser) -------
		// The input hook (OnInputEvent) collects FileDrop batches;
		// route them HERE by the drop point against this panel's rect (the
		// only drop target in the editor): images become .texture assets in
		// the browsed dir, .gltf/.glb run the full import. A drop outside
		// the panel drops with a log (nothing silently swallowed).
		if (m_pendingDropX >= 0.0f) {
			const ImVec2 wp = ImGui::GetWindowPos();
			const ImVec2 ws = ImGui::GetWindowSize();
			const ImVec2 vp = ImGui::GetMainViewport()->Pos;
			const float dx = vp.x + m_pendingDropX;
			const float dy = vp.y + m_pendingDropY;
			if (dx >= wp.x && dx < wp.x + ws.x
				&& dy >= wp.y && dy < wp.y + ws.y)
				HandleDroppedFiles(m_pendingDropFiles);
			else if (!m_pendingDropFiles.empty())
				Log("Drop: landed outside the asset browser ("
					+ std::to_string(m_pendingDropFiles.size())
					+ " file(s) ignored — drop onto the Assets panel).");
			m_pendingDropX = -1.0f;
			m_pendingDropY = -1.0f;
			m_pendingDropFiles.clear();
		}

		// Header controls live in this non-scrolling root.  Wrap every
		// action when the dock gets narrow instead of spilling into the grid.
		auto SameLineIfFits = [](const char* nextLabel) {
			const ImGuiStyle& style = ImGui::GetStyle();
			const float width = ImGui::CalcTextSize(nextLabel, nullptr, true).x
				+ style.FramePadding.x * 2.0f;
			const float right = ImGui::GetWindowPos().x
				+ ImGui::GetWindowContentRegionMax().x;
			if (ImGui::GetItemRectMax().x + style.ItemSpacing.x
				+ width <= right)
				ImGui::SameLine();
		};
		// Wrap a group of neighboring widgets as one unit (breadcrumb arrow
		// and the button after it must stay together).
		auto SameLineIfWidthFits = [](float width) {
			const ImGuiStyle& style = ImGui::GetStyle();
			const float right = ImGui::GetWindowPos().x
				+ ImGui::GetWindowContentRegionMax().x;
			if (ImGui::GetItemRectMax().x + style.ItemSpacing.x
				+ width <= right)
				ImGui::SameLine();
		};
		// Truncate status text so paths/UUIDs cannot escape the dock's width.
		auto FitLabel = [](const std::string& text, float maxWidth) {
			if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
				return text;
			std::string shown = text;
			while (!shown.empty() && ImGui::CalcTextSize(
				("..." + shown).c_str()).x > maxWidth)
				shown.erase(shown.begin());
			return "..." + shown;
		};

		// ---- header row 1: navigation breadcrumb (wrapping) ------------
		ImGui::BeginDisabled(ProjectRelativePath(m_assetsDir).empty());
		if (ImGui::SmallButton("^ Up")) {
			const std::string rel = ProjectRelativePath(m_assetsDir);
			const size_t slash = rel.find_last_of('/');
			navigateAssetDirectory((slash == std::string::npos)
				? m_projectRoot
				: (fs::path(m_projectRoot) / rel.substr(0, slash))
					.generic_string());
		}
		ImGui::EndDisabled();
		SameLineIfFits(m_projectName.c_str());
		if (ImGui::SmallButton(m_projectName.c_str())) {
			navigateAssetDirectory(m_projectRoot);
		}
		{
			fs::path acc = fs::path(m_projectRoot);
			std::string rel = ProjectRelativePath(m_assetsDir);
			int depth = 0;
			while (!rel.empty()) {
				const size_t slash = rel.find('/');
				const std::string comp = (slash == std::string::npos)
					? rel : rel.substr(0, slash);
				acc /= comp;
				const std::string shown = comp.size() > 22
					? comp.substr(0, 19) + "..." : comp;
				const ImGuiStyle& style = ImGui::GetStyle();
				const float width = ImGui::CalcTextSize(">").x
					+ style.ItemSpacing.x
					+ ImGui::CalcTextSize(shown.c_str()).x
					+ style.FramePadding.x * 2.0f;
				SameLineIfWidthFits(width);
				ImGui::TextDisabled(">");
				ImGui::SameLine();
				ImGui::PushID(depth++);
				if (ImGui::SmallButton(shown.c_str())) {
					navigateAssetDirectory(acc.generic_string());
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", acc.generic_string().c_str());
				ImGui::PopID();
				rel = (slash == std::string::npos)
					? std::string() : rel.substr(slash + 1);
			}
		}

		// Own line, NEVER SameLine with the filter or actions.
		const std::string currentPath = "Path: " + m_assetsDir;
		const std::string pathLabel = FitLabel(currentPath,
			ImGui::GetContentRegionAvail().x);
		ImGui::TextDisabled("%s", pathLabel.c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", m_assetsDir.c_str());

		// ---- header row 2: collection filter and expanded views --------
		// Internal cubemap / GI-cook artifacts are not browsable collections.
		const char* filters[7] = { "All items", "Meshes", "Materials",
			"Animations", "GI bakes", "Lightmap cooks", "GI volumes" };
		ImGui::SetNextItemWidth(std::min(180.0f,
			std::max(1.0f, ImGui::GetContentRegionAvail().x)));
		if (ImGui::Combo("##assetsfilter", &m_assetsFilter, filters, 7))
			clearAssetSelection();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Show files and folders, or browse assets by type.");
		if (!m_assetsExpandGltf.empty()) {
			const std::string stem = fs::path(m_assetsExpandGltf).stem().string();
			const std::string btn = stem + " X##expand";
			SameLineIfFits(btn.c_str());
			if (ImGui::SmallButton(btn.c_str())) {
				m_assetsExpandGltf.clear();
				clearAssetSelection();
			}
		}
		if (!m_assetsExpandTour.empty()) {
			const std::string stem = fs::path(m_assetsExpandTour).stem().string();
			const std::string btn = stem + " X##expandtour";
			SameLineIfFits(btn.c_str());
			if (ImGui::SmallButton(btn.c_str())) {
				m_assetsExpandTour.clear();
				clearAssetSelection();
			}
		}

		// ---- header row 3: actions (responsive, not scrolling) ----------
		ImGui::Separator();
		if (ImGui::Button("Import asset...")) {
			std::string picked;
			if (NativeOpenFile(picked) && !picked.empty()
				&& ImportAsset(picked)) {
				// Navigate to where the import landed.
				navigateAssetDirectory((fs::path(m_projectRoot) / "content"
					/ "models").generic_string());
			}
		}
		// The toolbar and grid context menu share the same creation
		// actions; new assets are placed in the browsed directory.
		const auto drawNewAssetMenu = [&]() {
            if (ImGui::MenuItem("New Scene (.tour)", nullptr, false,
                    !m_cookRunning))
                CreateSceneAsset(m_assetsDir);
            if (ImGui::MenuItem("New Folder"))
                CreateAssetFolder(m_assetsDir);
            ImGui::Separator();
			// Authoring-only .shape documents. The importer/runtime draw
			// paths are intentionally separate from this source creation.
			if (ImGui::BeginMenu("New Procedural Shape")) {
				for (tour::asset::ShapeKind kind : {
					tour::asset::ShapeKind::Spline,
					tour::asset::ShapeKind::PointCloud,
					tour::asset::ShapeKind::Plane,
					tour::asset::ShapeKind::Box,
					tour::asset::ShapeKind::Sphere,
					tour::asset::ShapeKind::Cylinder,
					tour::asset::ShapeKind::Cone}) {
					const char* name = tour::asset::ShapeKindName(kind);
					if (!ImGui::MenuItem(name)) continue;
					const fs::path dir = AssetCreateDirectory(m_projectRoot,
						m_assetsDir, "shapes");
					const fs::path target = UniqueAssetCreatePath(dir,
						name, ".shape");
					std::error_code ec;
					fs::create_directories(dir, ec);
					if (ec || target.empty()) {
						Log("Could not create shape asset directory");
						continue;
					}
					std::ofstream output(target, std::ios::binary);
					output << tour::asset::SerializeShapeAsset(
						tour::asset::DefaultShapeAsset(kind));
					output.close();
					if (!output) {
						Log("Could not write shape asset: " + target.generic_string());
						continue;
					}
					Log("Created shape source: " + target.generic_string());
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("New GI Volume Asset (.givolume)"))
				CreateGivolumeAsset(m_assetsDir);
			if (ImGui::MenuItem("New Texture (from image)...")) {
				std::string picked;
				if (NativeOpenFile(picked, true) && !picked.empty())
					CreateTextureAsset(picked, m_assetsDir);
			}
		};
		SameLineIfFits("New...");
		if (ImGui::Button("New..."))
			ImGui::OpenPopup("##newcookasset");
		if (ImGui::BeginPopup("##newcookasset")) {
			drawNewAssetMenu();
			ImGui::EndPopup();
		}
		if (m_assetDb.IsOpen()) {
			SameLineIfFits("Rescan");
			if (ImGui::Button("Rescan")) {
				smvk::cache::RescanStats stats;
				std::string rerr;
				if (m_assetDb.RescanProject(stats, rerr))
					Log("Asset rescan: "
						+ std::to_string(stats.adopted) + " adopted, "
						+ std::to_string(stats.moved) + " re-pointed, "
						+ std::to_string(stats.derived) + " derived, "
						+ std::to_string(stats.missing) + " missing.");
				else
					Log("Asset rescan failed: " + rerr);
			}
		}
		// Selection actions (Open = scene files only; paks run in the App).
		static const auto isOpenable = [](const std::string& p) {
			std::string e = fs::path(p).extension().string();
			for (char& c : e)
				c = char(std::tolower((unsigned char)c));
			return smvk::assets::IsMeshSourceExtension(e) || e == ".tour";
		};
		{
			const bool selFile = !m_assetsSel.empty()
				&& fs::is_regular_file(fs::path(m_assetsSel), ec);
			SameLineIfFits("Open");
			ImGui::BeginDisabled(!(selFile && isOpenable(m_assetsSel)));
			if (ImGui::SmallButton("Open##sel")) {
				if (!m_cookRunning)
					OpenAnySceneFile(m_assetsSel);
				else
					Log("Assets: a cook is running - wait for it to "
						"finish.");
			}
			ImGui::EndDisabled();
			SameLineIfFits("Reimport");
            const std::string selectedExt =
                fs::path(m_assetsSel).extension().string();
            const bool canReimport = selFile && !m_cookRunning
                && (smvk::assets::IsMeshSourceFile(fs::path(m_assetsSel))
                    || selectedExt == ".texture");
            ImGui::BeginDisabled(!canReimport);
            if (ImGui::SmallButton("Reimport##sel"))
                ReimportAsset(m_assetsSel);
            ImGui::EndDisabled();
            SameLineIfFits("Reveal");
			ImGui::BeginDisabled(m_assetsSel.empty());
			if (ImGui::SmallButton("Reveal##sel"))
				RevealInExplorer(m_assetsSel);
			ImGui::EndDisabled();
			// DCC operations are always one click away, without four
			// separate toolbar buttons squeezing the asset grid.
			SameLineIfFits("DCC...");
			ImGui::BeginDisabled(!selFile);
			if (ImGui::SmallButton("DCC...##sel"))
				ImGui::OpenPopup("##selected_asset_dcc");
			ImGui::EndDisabled();
			if (ImGui::BeginPopup("##selected_asset_dcc")) {
				if (ImGui::MenuItem("Open in DCC"))
					OpenAssetInDcc(m_assetsSel);
				if (ImGui::MenuItem("Link DCC source..."))
					LinkAssetToDcc(m_assetsSel);
				if (ImGui::MenuItem("Publish glTF", nullptr, false,
						!m_dccPublishRunning))
					PublishAssetFromDcc(m_assetsSel);
				if (ImGui::MenuItem("Unlink DCC source"))
					UnlinkAssetFromDcc(m_assetsSel);
				ImGui::EndPopup();
			}
			// Add-to-scene (mesh/geometry selection): the click twin of the
			// viewport drag-drop (AddSceneReference asset-bind). Imports
			// land in-project first (ImportAsset), so this only ever adds
			// in-project assets. A mesh-source container adds its first
			// derived geometry (same as expanding it and dropping the tile).
			{
				auto isMeshAsset = [](const std::string& p) {
					std::string e = fs::path(p).extension().string();
					for (char& c : e)
						c = char(std::tolower((unsigned char)c));
					return e == ".geometry" || e == ".mesh";
				};
				auto isGltfContainer = [](const std::string& p) {
					std::string e = fs::path(p).extension().string();
					for (char& c : e)
						c = char(std::tolower((unsigned char)c));
					return smvk::assets::IsMeshSourceExtension(e);
				};
				const bool canAdd = selFile && m_world != nullptr && !m_cookRunning
					&& (isMeshAsset(m_assetsSel) || isGltfContainer(m_assetsSel));
				SameLineIfFits("Add to scene");
				ImGui::BeginDisabled(!canAdd);
				if (ImGui::SmallButton("Add to scene##sel")) {
					std::string target = m_assetsSel;
					if (isGltfContainer(target) && !isMeshAsset(target)) {
						// Resolve the container's first derived geometry.
						smvk::assets::AssetManifest man;
						std::string merr;
						if (smvk::assets::LoadAssetManifest(target, man, merr)) {
							const std::string base = fs::path(target)
								.parent_path().generic_string();
							for (const auto& e : man.entries) {
								if (e.kind == smvk::assets::AssetKind::Geometry) {
									target = (fs::path(base) / e.file)
										.generic_string();
									break;
								}
							}
						}
					}
					if (isMeshAsset(target) || target != m_assetsSel) {
						if (AddSceneReference(target, {}, true))
							Log("Assets: added geometry asset '" + target
								+ "' (asset bind).");
					}
					else {
						Log("Assets: '" + m_assetsSel
							+ "' has no derived mesh yet — reimport it first.");
					}
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Add the selected mesh to the scene\n"
						"(same as dropping its tile on a viewport).");
			}
			// Delete (selection action): plans + confirms through the modal
			// (refusals — open scene, derived assets, cache — log + no-op).
			// MULTI: the set deletes as one request when >1.
			SameLineIfFits("Delete");
			ImGui::BeginDisabled(m_assetsSel.empty());
			const std::string deleteLabel = m_assetsSelSet.size() > 1
				? "Delete (" + std::to_string(m_assetsSelSet.size()) + ")##sel"
				: "Delete##sel";
			if (ImGui::SmallButton(deleteLabel.c_str())) {
				if (m_assetsSelSet.size() > 1)
					RequestAssetDelete(m_assetsSelSet.data(),
						m_assetsSelSet.size());
				else
					RequestAssetDelete(&m_assetsSel, 1);
			}
			ImGui::EndDisabled();
			// Selected file is informational and has its OWN line. Never
			// put a potentially long filename/UUID beside the action buttons.
			if (!m_assetsSel.empty()) {
				std::string selGuid;
				if (m_assetDb.IsOpen())
					m_assetDb.GuidForPath(m_assetsSel, selGuid);
				const std::string selected =
					fs::path(m_assetsSel).filename().string();
				const std::string label = FitLabel("Selected: " + selected,
					ImGui::GetContentRegionAvail().x);
				ImGui::TextDisabled("%s", label.c_str());
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", m_assetsSel.c_str());
				if (!selGuid.empty()) {
					SameLineIfFits("Copy UUID");
					if (ImGui::SmallButton("Copy UUID##sel"))
						ImGui::SetClipboardText(selGuid.c_str());
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", selGuid.c_str());
				}
			}
		}

		// ---- scrollable grid --------------------------------------------
		// Defer tile-context deletes until after the grid child has ended:
		// OpenPopup() and BeginPopupModal() must run at the same popup depth.
		std::string tileDeleteRequested;
		bool tileDeleteMulti = false;
		static std::string renameTarget;
		static char renameName[256]{};
		bool openRename = false;
		const auto requestRename = [&](const std::string& path) {
			if (path.empty() || m_cookRunning)
				return;
			// Never move an active scene: autosave still targets its path.
			if (path == m_gltfPath || path == m_gltfHostPath) {
				Log("Rename: close the open scene first.");
				return;
			}
			renameTarget = path;
			const std::string filename =
				fs::path(path).filename().string();
			std::snprintf(renameName, sizeof(renameName), "%s",
				filename.c_str());
			openRename = true;
		}; // the multi-set delete request
		ImGui::Separator();
		// The grid alone fills the remaining height, so toolbar rows never
		// compete with tiles. IMPORTANT: obtain dl after BeginChild; using
		// the parent draw list causes custom labels/icons to bleed over the
		// toolbar when scrolling.
		ImGui::BeginChild("##assetgrid", ImVec2(0, 0), false,
			ImGuiWindowFlags_AlwaysVerticalScrollbar);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		// Background context menu shares the header's New... actions.
		// .gicook/.lmcook are bake outputs, never user-created.
		if (ImGui::BeginPopupContextWindow("##assetgrid_ctx",
			ImGuiPopupFlags_MouseButtonRight |
			ImGuiPopupFlags_NoOpenOverItems)) {
			drawNewAssetMenu();
			ImGui::EndPopup();
		}
		// Registered GUID map (assets.db) for badges + tooltips. The rows
		// also feed the bake-asset collections below (kept in scope: the
		// collection branch reads them).
		std::unordered_map<std::string, std::string> guidByAbs;
		// The reverse map (guid -> abs path): the .tour folder view resolves
		// its bake refs (the tour's editorSet bake.*.guid keys) through the
		// assetdb — moves/renames heal through the rescan.
		std::unordered_map<std::string, std::string> absByGuid;
		std::unordered_map<std::string, std::string> kindByGuid;
		std::vector<smvk::cache::AssetRecord> assetRows;
		if (m_assetDb.IsOpen()) {
			std::string derr;
			if (m_assetDb.All(assetRows, derr)) {
				for (const auto& r : assetRows) {
					const std::string abs = m_assetDb.ResolvePath(r.path);
					guidByAbs[abs] = r.guid;
					absByGuid[r.guid] = abs;
					kindByGuid[r.guid] = r.kind;
				}
			}
		}
		// A .tour with only hidden bake references remains a normal scene
		// tile (not a folder containing just a back button + itself).
		const auto visibleBakeRef = [&](const std::string& guid) {
			const auto path = absByGuid.find(guid);
			if (path == absByGuid.end())
				return false;
			const auto kind = kindByGuid.find(guid);
			if (HideAssetBrowserFile(path->second,
				kind == kindByGuid.end() ? "" : kind->second))
				return false;
			std::error_code ec;
			return fs::is_regular_file(path->second, ec) && !ec;
		};
		// The tour's bake-asset references (RecordBakeAssetsFromCook writes
		// them; SaveEnvToDoc persists them into the tour): the editorSet
		// guids the .tour folder view resolves through the assetdb. Tolerant
		// — an unreadable/unref'd tour presents as a plain openable file.
		auto tourBakeRefs = [](const std::string& tourAbs,
			std::vector<std::string>& out) {
			smvk::tour::Doc td;
			std::string terr;
			if (!smvk::tour::LoadTour(tourAbs, td, terr))
				return;
			for (const auto& [k, v] : td.editorSettings) {
				if (k != "bake.gicook.guid" && k != "bake.cubemap.guid")
					continue;
				std::string g = v;
				// The tokenizer glues the statement ';' onto the last
				// quoted -v token (see the tour tests); trim it.
				while (!g.empty()
					&& (g.back() == ';' || g.back() == ' '
						|| g.back() == '\r'))
					g.pop_back();
				if (!g.empty())
					out.push_back(g);
			}
		};
		// Entries: directories first, then files, both by name (skip
		// .meta sidecars + dotfiles).
		// FRAME-ARENA STRINGS (the App's pattern — Memory/arena.h): the
		// per-frame entry storage rides smvk::FrameArena() (AllocateString
		// copies in, NUL-terminated, zero heap churn; the arena resets at
		// the frame boundary — MainLoop's ResetFrameArena). The views are
		// FRAME-SCOPED: everything that OUTLIVES the frame (the selection,
		// the expand state, the drag payload SINKS) copies out via Str().
		auto& fArena = smvk::FrameArena();
		auto put = [&](const std::string& s) {
			return fArena.AllocateString(smvk::StringViewT(s));
		};
		struct Entry {
			smvk::StringViewT name;
			smvk::StringViewT abs;
			bool dir = false;
			IconType icon{};
			bool gltf = false;  // .gltf with a manifest: presents as folder
			bool tour = false;  // .tour with bake refs: presents as folder
			bool back = false;  // the expanded view's ".." tile
			// Drag-drop payload kinds ("" = not draggable).
			bool dragMesh = false;
			bool dragMaterial = false;
			// >= 0: a VIRTUAL FOLDER tile — the canon collection view of
			// that filter index (see the kVFolders table below). Chrome,
			// not an asset: never selects, never deletes, never drags.
			int vfilter = -1;
		};
		std::vector<Entry> dirs, files;
		// ---- VIRTUAL FOLDERS (the canon collection views) ---------------
		// At the project root, the header filter views appear as FOLDER
		// TILES in the grid: each enters its collection
		// on double-click; the active view's ".." leaves back to All. The
		// tile ids ride the "vfolder://<n>" scheme — never a real path
		// (plus the per-tile numeric uid below makes every tile id unique
		// regardless — the duplicate-abs conflict fix).
		{
			struct VFolderDef { const char* label; int filter; IconType icon; };
			static const VFolderDef kVFolders[6] = {
				{ "Meshes", 1, IconType::Model },
				{ "Materials", 2, IconType::Texture },
				{ "Animations", 3, IconType::Animation },
				{ "GI bakes", 4, IconType::GI },
				{ "Lightmap cooks", 5, IconType::GI },
				{ "GI volumes", 6, IconType::GI },
			};
			if (m_assetsFilter != 0) {
				// The active collection view's leave tile (".." -> All).
				// (The expanded glTF/tour views build their own back tiles
				// in their branches below — no second one here.)
				Entry back{};
				back.name = "..";
				back.abs = put("vfolder://up");
				back.dir = true;
				back.back = true;
				back.icon = IconType::Folder;
				dirs.push_back(back);
			}
			else if (ShowAssetBrowserCollectionFolders(
				m_projectRoot, m_assetsDir,
				!m_assetsExpandGltf.empty(),
				!m_assetsExpandTour.empty())) {
				// Only the project root shows collection tiles; keep them
				// ahead of real directories in the stable sort below.
				for (const VFolderDef& v : kVFolders) {
					Entry t{};
					t.name = put(v.label);
					t.abs = put("vfolder://" + std::to_string(v.filter));
					t.dir = true;
					t.vfilter = v.filter;
					t.icon = v.icon;
					dirs.push_back(t);
				}
			}
		}
		const auto iconForAsset = [&](smvk::assets::AssetKind k) {
			switch (k) {
			case smvk::assets::AssetKind::Geometry: return IconType::Model;
			case smvk::assets::AssetKind::Material:
				return IconType::Texture;
			case smvk::assets::AssetKind::Anim: return IconType::Animation;
			}
			return IconType::File;
		};
		// A glTF's derived assets (its manifest children), for BOTH the
		// expanded view and the collection filters.
		const std::string expandGltf = m_assetsExpandGltf;
		const std::string expandTour = m_assetsExpandTour;
		auto manifestChildren = [&](const std::string& gltfAbs,
			int kindFilter, std::vector<Entry>& out) {
			smvk::assets::AssetManifest man;
			std::string merr;
			if (!smvk::assets::LoadAssetManifest(gltfAbs, man, merr))
				return;
			const std::string base =
				fs::path(gltfAbs).parent_path().generic_string();
			for (const auto& e : man.entries) {
				if (kindFilter >= 0
					&& int(e.kind) != kindFilter)
					continue;
				const fs::path assetPath = fs::path(base) / e.file;
				if (HideAssetBrowserFile(assetPath))
					continue;
				Entry t{};
				t.name = put(e.name.empty() ? e.file : e.name);
				t.abs = put(assetPath.generic_string());
				t.icon = iconForAsset(e.kind);
				t.dragMesh = (e.kind == smvk::assets::AssetKind::Geometry);
				t.dragMaterial =
					(e.kind == smvk::assets::AssetKind::Material);
				out.push_back(std::move(t));
			}
		};
		// Recursive .gltf.meta scan under the current directory (the
		// collection views list every derived asset of the kind).
		std::vector<std::string> gltfMetas;
		if (m_assetsFilter != 0 || !expandGltf.empty()) {
			std::vector<fs::path> stack{ fs::path(m_assetsDir) };
			while (!stack.empty()) {
				const fs::path d = stack.back();
				stack.pop_back();
				std::error_code sec;
				for (fs::directory_iterator it(d, sec), end;
					!sec && it != end; it.increment(sec)) {
					if (sec)
						break;
					std::error_code fec;
					if (it->is_directory(fec) && !fec) {
						stack.push_back(it->path());
						continue;
					}
					if (!it->is_regular_file(fec) || fec)
						continue;
					const std::string n =
						it->path().filename().string();
					if (n.size() > 10
						&& n.compare(n.size() - 10, 10, ".gltf.meta") == 0)
						gltfMetas.push_back(it->path().string());
				}
			}
		}
		if (m_assetsFilter != 0) {
			if (m_assetsFilter <= 3) {
				// Collection view: every derived asset of the kind under the
				// current directory tree.
				for (const std::string& meta : gltfMetas) {
					const std::string gltf = meta.substr(
						0, meta.size() - 5); // strip ".meta"
					manifestChildren(gltf, m_assetsFilter - 1, files);
				}
			}
			else {
				// Visible bake/cook collections (GI bakes / lightmap cooks /
				// GI volumes): the assetdb rows adopted from
				// the .meta sidecars (see Engine/Asset/bake_asset.h for the
				// bake rows, Engine/Asset/cook_asset.h for the cook rows) —
				// moves/renames heal through the rescan, so these stay
				// correct where stem-named loose listings would rot. Rows
				// whose file is gone are skipped (the db keeps them as
				// retargets).
				const char* want = (m_assetsFilter == 4) ? "gi"
					: (m_assetsFilter == 5) ? "lmcook" : "givolume";
				for (const auto& r : assetRows) {
					if (r.kind != want)
						continue;
					const std::string abs = m_assetDb.ResolvePath(r.path);
					if (HideAssetBrowserFile(abs, r.kind))
						continue;
					std::error_code fec;
					if (abs.empty()
						|| !fs::is_regular_file(abs, fec) || fec)
						continue;
					Entry t{};
					t.name = put(fs::path(abs).filename().generic_string());
					t.abs = put(abs);
					t.icon = IconType::GI;
					files.push_back(std::move(t));
				}
			}
		}
		else if (!expandGltf.empty()) {
			// Expanded glTF: its derived assets + a back tile (Unity-style
			// folder view; the glTF itself stays listed as the source).
			Entry back{};
			back.name = "..";
			back.abs = put(expandGltf);
			back.dir = true; // plain-directory styling
			back.back = true; // double-click = leave the folder
			back.icon = IconType::Folder;
			dirs.push_back(back);
			manifestChildren(expandGltf, -1, files);
			Entry src{};
			src.name = put(fs::path(expandGltf).filename().string());
			src.abs = put(expandGltf);
			src.icon = IconType::Model;
			src.gltf = false; // inside the folder, double-click opens the source
			files.push_back(src);
		}
		else if (!expandTour.empty()) {
			// Expanded .tour: the SAME folder presentation, over the tour's
			// BAKE ASSETS — the refs (RecordBakeAssetsFromCook) resolve
			// through the assetdb (moves/renames heal through the rescan).
			// The tour itself stays listed as the source tile: double-click
			// OPENS it (isOpenable — the tour remains openable from the
			// browser, nested inside its own folder view).
			Entry back{};
			back.name = "..";
			back.abs = put(expandTour);
			back.dir = true;
			back.back = true;
			back.icon = IconType::Folder;
			dirs.push_back(back);
			std::vector<std::string> bakeRefs;
			tourBakeRefs(expandTour, bakeRefs);
			for (const std::string& guid : bakeRefs) {
				if (!visibleBakeRef(guid))
					continue; // hidden or unavailable bake artifact
				const auto git = absByGuid.find(guid);
				std::error_code fec;
				if (!fs::is_regular_file(git->second, fec) || fec)
					continue;
				Entry t{};
				t.name = put(fs::path(git->second).filename().generic_string());
				t.abs = put(git->second);
				t.icon = (t.name.size() > 13
					&& t.name.Str().compare(t.name.size() - 13, 13,
						".cubemanifest") == 0)
					? IconType::Env : IconType::GI;
				files.push_back(std::move(t));
			}
			// The source itself: a plain openable .tour tile (double-click
			// opens the scene — see the tile handler's isOpenable branch).
			Entry src{};
			src.name = put(fs::path(expandTour).filename().string());
			src.abs = put(expandTour);
			src.icon = IconTypeFor(src.name.Str(), false);
			files.push_back(src);
		}
		else {
			for (fs::directory_iterator it(m_assetsDir, ec), end;
				!ec && it != end; it.increment(ec)) {
				if (ec)
					break;
				std::error_code fec;
				const bool isDir = it->is_directory(fec) && !fec;
				if (!isDir && (!it->is_regular_file(fec) || fec))
					continue;
				const fs::path& p = it->path();
				std::string name = p.filename().string();
				if (name.empty() || name[0] == '.')
					continue;
				bool isGltf = false;
				bool isTour = false;
				bool dragMesh = false;
				bool dragMaterial = false;
				IconType icon = IconTypeFor(name, isDir);
				if (!isDir) {
					std::string e = p.extension().string();
					for (char& c : e)
						c = char(std::tolower((unsigned char)c));
					if (e == ".meta" || HideAssetBrowserFile(p))
						continue; // sidecar or internal bake artifact
					if (smvk::assets::IsMeshSourceExtension(e)) {
						// With a manifest the source presents as a FOLDER
						// of its derived assets; without one it opens as
						// a scene (the legacy path).
						std::string merr;
						smvk::assets::AssetManifest man;
						isGltf = smvk::assets::LoadAssetManifest(
							p.generic_string(), man, merr);
						if (isGltf)
							icon = IconType::Folder;
					}
					else if (e == ".tour") {
						// With bake-asset refs (a cook ran —
						// RecordBakeAssetsFromCook) the tour presents as a
						// FOLDER of its bakes, mirroring the glTF-with-manifest
						// presentation; without refs it stays a plain openable
						// file (the legacy behavior).
						std::vector<std::string> refs;
						tourBakeRefs(p.generic_string(), refs);
						isTour = std::any_of(refs.begin(), refs.end(), visibleBakeRef);
						if (isTour)
							icon = IconType::Folder;
					}
					else if (e == ".geometry" || e == ".mesh")
						dragMesh = true;
					else if (e == ".material")
						dragMaterial = true;
				}
				(isDir ? dirs : files).push_back({
					put(name),
					put(p.generic_string()),
					isDir,
					icon,
					isGltf,
					isTour,
					false,          // back
					dragMesh,
					dragMaterial
					});
			}
		}
		const auto byName = [](const Entry& a, const Entry& b) {
			std::string la = a.name.Str(), lb = b.name.Str();
			for (char& c : la)
				c = char(std::tolower((unsigned char)c));
			for (char& c : lb)
				c = char(std::tolower((unsigned char)c));
			return la < lb;
		};
		// The canon VIRTUAL FOLDERS stay PINNED to the front of the dirs
		// row (pre-sort push order preserved: a stable partition — the
		// vfolders first in their authored order, the real directories
		// byName behind them). The Unity feel: the collection roots are
		// the affordance, the real folders follow.
		const auto vfolderFirst = [](const Entry& a, const Entry& b) {
			if ((a.vfilter >= 0) != (b.vfilter >= 0))
				return a.vfilter >= 0;
			if (a.vfilter >= 0 && b.vfilter >= 0)
				return a.vfilter < b.vfilter;
			return false;
		};
		std::stable_sort(dirs.begin(), dirs.end(),
			[&](const Entry& a, const Entry& b) {
				if (vfolderFirst(a, b))
					return true;
				if (vfolderFirst(b, a))
					return false;
				return byName(a, b);
			});
		std::sort(files.begin(), files.end(), byName);

		const float tileW = 96.0f, tileH = 104.0f, iconS = 56.0f;
		int cols = std::max(1,
			int(ImGui::GetContentRegionAvail().x / tileW));
		int col = 0;
		// Collection views can produce duplicate paths/names. A per-frame
		// ordinal scopes EVERY tile uniquely, including filter/back tiles;
		// the remaining path/name IDs still distinguish its child controls.
		int tileOrdinal = 0;
		auto tile = [&](const Entry& e) {
			ImGui::PushID(tileOrdinal++);
			ImGui::PushID(e.abs.data());
			ImGui::PushID(e.name.data());
			ImGui::PushID(e.back ? 1 : 0);
			ImGui::PushID(e.vfilter);
			// The SET highlight (the multi-selection); the primary
			// (m_assetsSel) is redundant inside the set on plain clicks.
			// NAVIGATION CHROME NEVER HIGHLIGHTS: the ".." back tile
			// carries the SAME abs as the expanded view's SOURCE tile
			// (both = the glTF path) — selecting the source lit the
			// ".." too ("the .. up directory gets highlighted"); the
			// VIRTUAL FOLDERS are chrome the same way.
			const bool chrome = e.back || e.vfilter >= 0;
			const bool sel = !chrome
				&& (std::find(m_assetsSelSet.begin(),
					m_assetsSelSet.end(), e.abs.Str())
					!= m_assetsSelSet.end()
					|| m_assetsSel == e.abs.Str());
			const ImVec2 tileSz(tileW - 8.0f, tileH);
			(void)ImGui::InvisibleButton("##tile", tileSz);
			const bool hovered = ImGui::IsItemHovered();
			const bool clicked = ImGui::IsItemClicked();
			const bool dbl = hovered
				&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			const ImVec2 bmin = ImGui::GetItemRectMin();
			const ImVec2 bmax = ImGui::GetItemRectMax();
			// hover/selection chrome under the icon art.
			if (sel) {
				dl->AddRectFilled(bmin, bmax,
					IM_COL32(70, 130, 190, 60), 6.0f);
				dl->AddRect(bmin, bmax, IM_COL32(120, 200, 255, 220),
					6.0f, 0, 1.5f);
			}
			else if (hovered) {
				dl->AddRectFilled(bmin, bmax,
					IM_COL32(255, 255, 255, 16), 6.0f);
			}
			// icon centered (SVG texture, drawn shapes as fallback).
			const ImVec2 ico(bmin.x + (tileSz.x - iconS) * 0.5f,
				bmin.y + 6.0f);
			// THE .material SIDECAR PREVIEW (the matball pass's PNG): the
			// ball-under-default-sky thumbnail replaces the generic icon
			// when the sidecar exists (the Material tab's Ball preview
			// button renders it). Missing/stale = the generic icon (the
			// button regenerates on demand).
			ImTextureID sidecar = 0;
			if (!e.dir) {
				const std::string name = e.name.Str();
				std::string ext = fs::path(name).extension().string();
				for (char& c : ext)
					c = char(std::tolower((unsigned char)c));
				if (ext == ".material")
					sidecar = AssetSidecarThumb(e.abs.Str() + ".png");
				else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg"
					|| ext == ".bmp" || ext == ".tga")
					sidecar = AssetSidecarThumb(e.abs.Str());
			}
			if (sidecar != 0) {
				dl->AddImage(sidecar, ico,
					ImVec2(ico.x + iconS, ico.y + iconS));
			}
			else {
				const IconTex& icon = m_icons[size_t(e.icon)];
				if (icon.imgui != 0) {
					dl->AddImage(icon.imgui, ico,
						ImVec2(ico.x + iconS, ico.y + iconS));
				}
				else {
					DrawAssetIconFallback(dl, ico, iconS, e.icon);
				}
			}
			// Registered-asset badge (green dot, bottom-right of icon).
			if (!e.dir && guidByAbs.contains(e.abs.Str())) {
				dl->AddCircleFilled(
					ImVec2(ico.x + iconS - 3.0f, ico.y + iconS - 3.0f),
					3.5f, IM_COL32(90, 220, 120, 255), 10);
			}
			// Label: up to two centered lines, clipped with "...".
			{
				ImFont* font = ImGui::GetFont();
				const float fsize = ImGui::GetFontSize();
				const float maxW = (tileSz.x) - 10.0f;
				auto textW = [&](const char* s) {
					return font->CalcTextSizeA(fsize, 10000.0f, 0.0f, s).x;
				};
				std::string l1 = e.name.Str(), l2;
				if (textW(l1.c_str()) > maxW) {
					// Longest fitting prefix, preferring a separator.
					std::string growing;
					size_t best = 0;
					for (size_t i = 0; i < e.name.size(); ++i) {
						growing.push_back(e.name[i]);
						if (textW(growing.c_str()) > maxW)
							break;
						best = i + 1;
					}
					size_t cut = best;
					for (size_t i = best; i > 1; --i)
						if (e.name[i - 1] == '_' || e.name[i - 1] == '-'
							|| e.name[i - 1] == ' ' || e.name[i - 1] == '.') {
							cut = i;
							break;
						}
					l1 = e.name.Str().substr(0, cut);
					l2 = e.name.Str().substr(cut);
					if (textW(l2.c_str()) > maxW) {
						while (!l2.empty()
							&& textW((l2 + "...").c_str()) > maxW)
							l2.pop_back();
						l2 += "...";
					}
				}
				const ImU32 tcol =
					ImGui::GetColorU32(ImGuiCol_Text);
				dl->PushClipRect(bmin, bmax, true);
				const float yBase = bmax.y - 2.0f * fsize - 3.0f;
				const float cx = bmin.x + tileSz.x * 0.5f;
				if (l2.empty()) {
					dl->AddText(font, fsize,
						ImVec2(cx - textW(l1.c_str()) * 0.5f,
							yBase + fsize * 0.5f), tcol, l1.c_str());
				}
				else {
					dl->AddText(font, fsize,
						ImVec2(cx - textW(l1.c_str()) * 0.5f, yBase),
						tcol, l1.c_str());
					dl->AddText(font, fsize,
						ImVec2(cx - textW(l2.c_str()) * 0.5f,
							yBase + fsize + 1.0f), tcol, l2.c_str());
				}
				dl->PopClipRect();
			}
			// ---- SELECTION: single / ctrl-toggle / shift-range ----------
			// The SET is the browser's selection (m_assetsSel stays the
			// PRIMARY for the single-flow consumers: drags, the header
			// buttons). A plain click replaces the set; ctrl toggles the
			// entry; shift selects the anchor..entry RANGE over the
			// display order (dirs then files — the vectors in scope).
			// NAVIGATION TILES NEVER JOIN: the virtual-folder chrome (the
			// ".." back tile, the canon VIRTUAL FOLDERS) is a move, not
			// an asset — chrome entering the set made ranges span it +
			// the delete target the folder's PARENT.
			if (clicked && !e.back && e.vfilter < 0) {
				const bool ctrl = ImGui::GetIO().KeyCtrl;
				const bool shift = ImGui::GetIO().KeyShift;
				m_assetsSel = e.abs.Str();
				if (e.back) {
					// Navigation-only; handled on double-click below.
				}
				else if (ctrl) {
					// Toggle; the anchor moves (the next shift ranges
					// from HERE).
				auto it = std::find(m_assetsSelSet.begin(),
					m_assetsSelSet.end(), e.abs.Str());
				if (it != m_assetsSelSet.end())
						m_assetsSelSet.erase(it);
					else
						m_assetsSelSet.push_back(e.abs.Str());
					m_assetsAnchor = e.abs.Str();
				}
				else if (shift && !m_assetsAnchor.empty()) {
					// The anchor..entry range over the display order.
					const auto span = [&](const Entry& en) {
						return en.abs.Str() == e.abs.Str() || en.abs.Str() == m_assetsAnchor;
					};
					auto collectRange = [&](const std::vector<Entry>& v) {
						auto first = std::find_if(v.begin(), v.end(), span);
						if (first == v.end())
							return;
						auto last = std::find_if(v.rbegin(), v.rend(),
							span).base();
						for (auto it = first; it != last; ++it)
							if (!(it->back || it->vfilter >= 0)
								// chrome never ranges in
								&& std::find(m_assetsSelSet.begin(),
										m_assetsSelSet.end(), it->abs.Str())
									== m_assetsSelSet.end())
								m_assetsSelSet.push_back(it->abs.Str());
					};
					// Clear the non-range picks? Shift-select ADDS the
					// range to the set (the ctrl-picks survive — the
					// standard additive model).
					collectRange(dirs);
					collectRange(files);
				}
				else {
					m_assetsSelSet = { e.abs.Str() };
					m_assetsAnchor = e.abs.Str();
				}
			}
			if (dbl) {
				if (e.vfilter >= 0) {
					// ENTER THE CANON COLLECTION VIEW (the virtual
					// folder): the filter combo and the tile are the same
					// knob.
					m_assetsFilter = e.vfilter;
					clearAssetSelection();
				}
				else {
				if (!e.back) {
					m_assetsSel = e.abs.Str();
					m_assetsSelSet = { e.abs.Str() };
					m_assetsAnchor = e.abs.Str();
				}
				if (e.back) {
					// Leave the expanded glTF/tour folder OR the canon
					// collection view (".." -> All).
					m_assetsExpandGltf.clear();
					m_assetsExpandTour.clear();
					m_assetsFilter = 0;
					clearAssetSelection();
				}
				else if (e.gltf) {
					// A glTF with a manifest: open the FOLDER (the derived
					// assets) — never the scene (the .tour references the
					// .mesh asset; the glTF is the container/source).
					m_assetsExpandGltf = e.abs.Str();
					clearAssetSelection();
				}
				else if (e.tour) {
					// A .tour with bake refs: open the FOLDER (its bake
					// assets). The tour itself stays openable INSIDE (the
					// source tile's isOpenable branch below) and via the
					// header Open button.
					m_assetsExpandTour = e.abs.Str();
					clearAssetSelection();
				}
				else if (e.dir) {
					navigateAssetDirectory(e.abs.Str());
				}
				else if (isOpenable(e.abs.Str())) {
					if (!m_cookRunning)
						OpenAnySceneFile(e.abs.Str());
					else
						Log("Assets: a cook is running - wait for it "
							"to finish.");
				}
				}
			}
			// Drag-and-drop sources (mesh -> viewport adds a scene
			// reference; material -> the selection assigns it).
			// (The canon VIRTUAL FOLDERS never drag — chrome.)
			if ((e.dragMesh || e.dragMaterial) && e.vfilter < 0) {
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
					const char* payloadType =
						e.dragMesh ? "VT_ASSET_MESH" : "VT_ASSET_MAT";

					ImGui::SetDragDropPayload(
						payloadType,
						e.abs.data(),
						e.abs.size() + 1,
						ImGuiCond_Once
					);

					ImGui::TextUnformatted(e.name.data());
					ImGui::Separator();

					ImGui::TextDisabled(
						e.dragMesh
						? "Drop on viewport to add scene reference"
						: "Drop on viewport to assign material"
					);

					ImGui::EndDragDropSource();
				}
			}
			if (hovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				if (e.vfilter >= 0) {
					ImGui::SetTooltip("View the %s collection.\n"
						"Double-click to open.", e.name.data());
				}
				else if (e.back) {
					ImGui::SetTooltip("Double-click to return.");
				}
				else {
					const auto git = guidByAbs.find(e.abs.Str());
					ImGui::SetTooltip("%s\n%s\n%s",
						e.name.data(), e.abs.data(),
						(git != guidByAbs.end() && !git->second.empty())
						? git->second.c_str() : "(no asset uuid)");
				}
			}
			// Navigation chrome (back/collection) has no Delete menu.
			// Real assets use the same confirm dialog as the header.
			if (!e.back && e.vfilter < 0
				&& ImGui::BeginPopupContextItem("##assettile_ctx")) {
				// DCC link resolves manifest-derived tiles to their source.
				std::error_code dccEc;
				if (fs::is_regular_file(e.abs.Str(), dccEc) && !dccEc) {
					const std::string assetExt =
                        fs::path(e.abs.Str()).extension().string();
                    if (ImGui::MenuItem("Reimport", nullptr, false,
                        !m_cookRunning &&
                        (smvk::assets::IsMeshSourceFile(fs::path(e.abs.Str()))
                            || assetExt == ".texture")))
                        ReimportAsset(e.abs.Str());
                    if (assetExt == ".tour" &&
                        ImGui::MenuItem("Edit in main editor", nullptr, false,
                            m_fullPrefabParentSnapshot.empty()))
                        RequestFullPrefabEdit(e.abs.Str());
                    if (ImGui::MenuItem("Open in DCC"))
						OpenAssetInDcc(e.abs.Str());
					if (ImGui::MenuItem("Link DCC source..."))
						LinkAssetToDcc(e.abs.Str());
					if (ImGui::MenuItem("Publish glTF", nullptr, false,
							!m_dccPublishRunning))
						PublishAssetFromDcc(e.abs.Str());
					if (ImGui::MenuItem("Unlink DCC source"))
						UnlinkAssetFromDcc(e.abs.Str());
					ImGui::Separator();
				}
				if (ImGui::MenuItem("Rename", nullptr, false,
					!m_cookRunning && m_assetsSelSet.size() <= 1))
					requestRename(e.abs.Str());
				// MULTI-AWARE: deleting a tile that sits in a multi-
				// selection (>1) requests the WHOLE set (one confirm,
				// every plan executes); a single/unselected tile keeps
				// the single flow.
				const bool inSet = std::find(m_assetsSelSet.begin(),
					m_assetsSelSet.end(), e.abs.Str())
					!= m_assetsSelSet.end();
				if (ImGui::MenuItem(m_assetsSelSet.size() > 1 && inSet
						? "Delete (selection)" : "Delete")) {
					m_assetsSel = e.abs.Str();
					if (inSet && m_assetsSelSet.size() > 1)
						tileDeleteMulti = true;
					else
						tileDeleteRequested = e.abs.Str();
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
		};
		for (const Entry& e : dirs) {
			if (col != 0)
				ImGui::SameLine(0.0f, 4.0f);
			tile(e);
			if (++col == cols)
				col = 0;
		}
		for (const Entry& e : files) {
			if (col != 0)
				ImGui::SameLine(0.0f, 4.0f);
			tile(e);
			if (++col == cols)
				col = 0;
		}
		if (dirs.empty() && files.empty())
			ImGui::TextDisabled("(empty folder)");
		ImGui::EndChild(); // ##assetgrid
		// MULTIDELETE on the Delete KEY while the grid holds the focus
		// (the key + the context menu are the two delete gestures).
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
			&& ImGui::IsKeyPressed(ImGuiKey_F2)
			&& m_assetsSelSet.size() == 1)
			requestRename(m_assetsSelSet.front());
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
			&& ImGui::IsKeyPressed(ImGuiKey_Delete)) {
			if (m_assetsSelSet.size() > 1)
				tileDeleteMulti = true;
			else if (!m_assetsSel.empty())
				tileDeleteRequested = m_assetsSel;
		}
		// THE DEFERRED OPEN: the request records the plans + the flag
		// from the tile-menu / key paths (deeper or mid-close scopes);
		// the OPEN itself lives INSIDE DrawAssetDeleteModal, on the line
		// directly above the BeginPopupModal — the same function, the
		// same id stack BY CONSTRUCTION (no intervening statement can
		// ever shift the stack again).
		if (tileDeleteMulti && !m_assetsSelSet.empty())
			RequestAssetDelete(m_assetsSelSet.data(), m_assetsSelSet.size());
		else if (!tileDeleteRequested.empty())
			RequestAssetDelete(&tileDeleteRequested, 1);
		DrawAssetDeleteModal();
		// Popup is hosted at the same ID depth on every frame (outside
		// the grid child), including the context-menu request frame.
		if (openRename)
			ImGui::OpenPopup("Rename asset##browser");
		if (ImGui::BeginPopupModal("Rename asset##browser", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextDisabled("Renaming may break path-based references.");
			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere();
			const bool enter = ImGui::InputText("Name", renameName,
				sizeof(renameName), ImGuiInputTextFlags_EnterReturnsTrue);
			if (enter || ImGui::Button("Rename")) {
				std::string result, err;
				if (RenameBrowserAsset(renameTarget, m_projectRoot,
					renameName, result, err)) {
					const auto retarget = [&](std::string& path) {
						if (path == renameTarget)
							path = result;
						else if (path.size() > renameTarget.size()
							&& path.compare(0, renameTarget.size(),
								renameTarget) == 0
							&& path[renameTarget.size()] == '/')
							path = result + path.substr(renameTarget.size());
					};
					retarget(m_assetsSel);
					retarget(m_assetsAnchor);
					retarget(m_assetsDir);
					retarget(m_assetsExpandGltf);
					retarget(m_assetsExpandTour);
					for (auto& path : m_assetsSelSet)
						retarget(path);
					m_assetInspectPath.clear();
					m_assetInspectMat.reset();
					m_assetInspectMesh.reset();
					m_assetInspectGiCook.reset();
					m_assetInspectLmCook.reset();
					if (m_assetDb.IsOpen()) {
						smvk::cache::RescanStats stats;
						std::string dbErr;
						if (!m_assetDb.RescanProject(stats, dbErr))
							Log("Rename: asset DB rescan failed: " + dbErr);
					}
					Log("Renamed asset to '" + result + "'.");
					renameTarget.clear();
					ImGui::CloseCurrentPopup();
				} else {
					Log("Rename: " + err);
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) {
				renameTarget.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		ImGui::EndChild(); // ##assets_root (non-scrolling header)
	}

	// THE delete request — ONE method, targets* + targetCount: the
	// single delete is count 1; the multi passes the selection set. Each
	// target validates (project containment, not the open scene,
	// planable); the plans ride m_assetsDeletePending (a vector; a
	// refused target logs + drops, the rest proceed) + the ONE confirm
	// modal opens.

} // namespace editor
