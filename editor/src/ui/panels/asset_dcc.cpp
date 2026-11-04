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

} // namespace editor
