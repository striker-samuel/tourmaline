// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// EditorApp: init / frame loop / scene loading / frame-data upload.
// Rendering passes live in editor_render.cpp, UI in editor_ui.cpp.

// Editor app implementation. NOMINMAX must precede EVERY include: the
// prefs path helper pulls <shlobj.h> -> <windows.h>, whose min/max
// macros would poison every std::min/std::max in this TU.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "editor_app.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
#include <shlobj.h> // SHGetKnownFolderPath/FOLDERID_Documents (prefs path)
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/bake_asset.h>
#include <Engine/Asset/pak.h>
#include <Engine/GI/cubemap_volume.h>
#include <Engine/Profile/profile.h>
#include <Engine/Renderer/mesh.h>
#include <gltf_import/gltf.h>
#include <stb_image_write.h> // PNG writers (probe previews)
#include <Engine/core.h>

#include <imgui.h>

namespace editor {

bool EditorApp::HasPakExtension(const std::string& path) {
	if (path.size() < 4)
		return false;
	const std::string ext = path.substr(path.size() - 4);
	return ext == ".pak" || ext == ".PAK" || ext == ".Pak";
}

bool EditorApp::SceneEmpty() const {
	// The "a scene worth acting on" gate (replaces the old m_hasGltfModel
	// gates — a .tour definition just references ASSET UUIDs, so "has a
	// source glTF" was the wrong question): true when no world is loaded
	// or the merged scene carries no content (no meshes, no lights, no
	// cameras — a sourceless tour with no bound references). With pak
	// opens gone (paks run in the App), every non-empty editor scene is
	// definition-backed by construction.
	return !m_world
		|| (m_world->scene.meshForNode.empty()
			&& m_world->scene.lightForNode.empty()
			&& m_world->scene.cameraForNode.empty());
}

bool EditorApp::OpenScene(const std::string& path) {
	// Queued when called before Initialize (CLI path); otherwise loads now.
	// SOURCE scenes only: the editor is the WYSIWYG authoring tool — cooked
	// paks run in the App (Run in App), never read back here.
	if (!m_context) {
		m_pendingGltf = path;
		return true;
	}
	if (HasPakExtension(path)) {
		Log("Pak scenes run in the App (Run in App) — the editor authors "
			"source scenes (WYSIWYG): '" + path + "'");
		return false;
	}
	// The .gltf redirect happens INSIDE LoadGltfIntoWorld (the session
	// funnel — direct opens AND tour-recorded sources both land there; see
	// its head).
	if (!LoadGltfIntoWorld(path))
		return false;
	// Project outputs follow the project (unless customized), and the
	// source joins the asset directory (GUID sidecar issued on demand).
	PrimeProjectCookDefaults();
	AdoptProjectAsset(path);
	return true;
}

// Open-dialog dispatch: .tour files replay into the stack (source auto-
// loads when it differs); everything else is a scene open.
bool EditorApp::OpenAnySceneFile(const std::string& path) {
	// Queue before-boot opens (CLI path) like OpenScene: a tour open must
	// not run either — OpenTourFile drives the FULL definition build
	// (LoadGltfIntoWorld -> ApplySceneReferences -> mesh uploads), which
	// needs the booted context (a pre-boot run built a ctx-null cluster,
	// took the GPU-less import branch and crashed vmaVirtualAllocate on
	// the never-allocated static heap).
	if (!m_context) {
		m_pendingGltf = path;
		return true;
	}
	if (path.size() >= 5) {
		const std::string ext = path.substr(path.size() - 5);
		if (ext == ".tour" || ext == ".TOUR")
			return OpenTourFile(path);
	}
	return OpenScene(path);
}

EditorApp::EditorApp()
	: m_guidResolver(*this) {
}

bool EditorApp::Initialize() {
	if (!Super::Initialize())
		return false;
	// Fibers backend (the editor's ONLY off-main-thread lane — the
	// cook/bake workers ride it; see editor_fibers.h). Before anything
	// that can Post a job.
	m_fibers.Start(2);
	// Dev quad-layout switch (TOURMALINE_EDITOR_VP_QUAD=1): start in the
	// four-viewport layout so headless smoke runs exercise the per-
	// viewport editor passes. The V toggle keeps working after.
	if (const char* quad = std::getenv("TOURMALINE_EDITOR_VP_QUAD")) {
		if (std::atoi(quad) != 0) {
			m_vpLayout = 1;
			std::cerr << "[Editor] quad viewport layout (dev env)\n";
		}
	}
	// Static mesh heap (VMA virtual blocks + GPU buffers) backing
	// Cluster::UploadStaticMesh. Without this, the first glTF import
	// crashes in vmaVirtualAllocate on a null m_staticVertexBlock.
	smvk::AllocateStaticVertexFmt(*m_context);
	{
		namespace fs = std::filesystem;
		char* docs = nullptr;
		// SHGetKnownFolderPath is Windows-specific; fallback to getenv on others
		std::string docsPath;
#if defined(_WIN32)
		PWSTR path = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) {
			docsPath = std::filesystem::path(path).string();
			CoTaskMemFree(path);
		}
#else
		const char* home = std::getenv("HOME");
		if (home) docsPath = std::filesystem::path(home) / "Documents";
#endif
		if (!docsPath.empty()) {
			m_prefsPath = (std::filesystem::path(docsPath) / "tourmalineEditor" / "editor.prefs").string();
		} else {
			// Fallback: next to the exe
			m_prefsPath = "editor.prefs";
		}
	}
	LoadPrefs(); // load editor prefs (window sizes, panel widths, etc.)
	SetRelativeMouseMode(false);

	// Project: reopen the last project when kept (it owns the artifact
	// cache: OpenProject opens cache.db + the asset directory there).
	// Without a project the cache is OFF (no global disk — cooks and
	// imports still work, the cache only accelerates processing).
	{
		const std::string last = m_projectRoot;
		m_projectRoot.clear();
		m_projectName.clear();
		if (!last.empty() && !OpenProject(last)) {
			Log("Last project unavailable — running without a project "
				"(artifact cache off; open a project for imports).");
		}
		else if (last.empty()) {
			Log("No project open — artifact cache off (File > New / "
				"Open Project... to work project-bound).");
		}
	}
	// FORCED project selection on startup (interactive runs only):
	// the editor is project-bound — the blocking dialog (recents +
	// New/Browse) demands one. Smoke flows drive their own projects
	// and never see it.
	m_projectDialogPending = true;


	m_imgui = smvk::mem::MakeProcess<smvk::ImGuiOverlay>();
	smvk::ImGuiOverlayDesc imguiDesc{};
	if (!m_imgui->Init(*m_context, *m_swapchain, window, imguiDesc)) {
		std::cerr << "[Editor] ImGui init failed, continuing without UI\n";
		m_imgui.reset();
	}
	else {
		imguiEventHook_ = &smvk::ImGuiOverlay::ProcessEvent;
		// Opaque menu/window surfaces: the dark theme leaves these
		// slightly translucent (the 3D viewport bleeds through menus).
		// ONLY the top-level surfaces change â€” every other entry keeps
		// its theme alpha. Notably ChildBg stays transparent (it is
		// transparent BLACK by design: forcing it opaque paints every
		// child window black), and frames/scrollbars/tabs keep their
		// designed translucency. Re-applied here, not in the shared
		// overlay: the theme belongs to the app, and ApplyDpiScale
		// preserves the colors it copies.
		ImGuiStyle& style = ImGui::GetStyle();
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		style.Colors[ImGuiCol_PopupBg].w = 1.0f;
		style.Colors[ImGuiCol_MenuBarBg].w = 1.0f;
		style.Colors[ImGuiCol_TitleBg].w = 1.0f;
		style.Colors[ImGuiCol_TitleBgActive].w = 1.0f;
		style.Colors[ImGuiCol_TitleBgCollapsed].w = 1.0f;
		// SVG icon set (asset browser grid): the Vulkan backend must
		// be up for the ImGui user-texture registration.
		EnsureSvgIcons();
	}

	// Default editor cameras (Perspective / Top ortho / Side ortho).
	SetupDefaultCameras();

	// Tourmaline Editor branding: window title + generated logo icon.
	ApplyTourmalineBranding();

	// NOTE: no second LoadPrefs() here — prefs (incl. the kept project)
	// loaded once above, before the project/scratch open. Re-reading
	// would restore stale values over the live state.
	if (!m_pendingGltf.empty()) {
		const std::string p = m_pendingGltf;
		m_pendingGltf.clear();
		// Dispatch through the open-dialog path: a queued .tour must
		// replay (OpenScene alone fails the tour as a glTF import).
		if (!OpenAnySceneFile(p))
			std::cerr << "[Editor] failed to load '" << p << "'\n";
	}

	Log("Tourmaline Editor ready. File > Open scene... to load a scene.");
	return true;
}

void EditorApp::Shutdown() {
	// Flush the working tour to the project .tour on exit (per-edit
	// saves went to the %APPDATA% autosave) — same contract as
	// CloseScene, so closing the editor never loses edits.
	if (m_world && !TourTarget().empty())
		SaveTour();
	SavePrefs(); // save editor prefs on close
	WaitForDeviceIdle();
	m_textureStreamer.reset();
	// Fibers shutdown: deliver any queued completions, wait for
	// in-flight jobs (workers drain them), deliver the rest — all
	// BEFORE the teardown below (completions touch task-window state;
	// a cook's own data died with the tools). Polls are gone (the
	// finish logic rides the completion callbacks now).
	m_fibers.Pump();
	m_fibers.Stop();
	m_fibers.Pump();
	m_imgui.reset();
	m_depth = smvk::DepthTarget{};
	// Preview HDR + tonemapper teardown (see m_sceneHdr in editor_app.h).
	m_sceneHdr.Destroy(*m_rendergraph);
	m_previewTonemap.DestroyShaders();
	m_frameUniforms = {};
	m_insetFrameUniforms = {};
	m_materialsBuf = {};
	m_lightsBuf = {};
	m_cascadeBuf = {};
	ClearSglmLayers();
	m_vsModule = {};
	m_unlitFsModule = {};
	m_litFsModule = {};
	m_wireFsModule = {};
	m_bakedFsModule = {};
	m_meshDebugVsModule = {};
	m_meshDebugFsModule = {};
	m_lineVsModule = {};
	m_lineFsModule = {};
	m_gizmoLines = {};
	m_meshletIndices = {};
	m_lmExportLines = {};
	m_lmExportLinesCap = 0;
	m_lmExportLinesCount = 0;
	if (!m_lmExportLogical.empty()) {
		m_rendergraph->FreeImageView(m_lmExportLogical);
		m_lmExportLogical = {};
	}
	if (!m_lmExportDepthLogical.empty()) {
		m_rendergraph->FreeImageView(m_lmExportDepthLogical);
		m_lmExportDepthLogical = {};
	}
	m_lmExportView = {};
	m_lmExportImage = {};
	m_lmExportDepthView = {};
	m_lmExportDepth = {};
	m_unlitProgram = {};
	m_litProgram = {};
	m_bakedProgram = {};
	m_wireProgram = {};
	m_lineProgram = {};
	Super::Shutdown();
}

void EditorApp::LoadPrefs() {
	// Sectioned panel layout (px). No backward compat: anything
	// unparseable (missing file, corrupt line, unknown key) keeps
	// the in-code defaults below.
	m_outlinerW = kEditorLeftW;
	m_inspectorW = kEditorRightW;
	m_bottomDockH = kEditorBottomH;
	if (m_prefsPath.empty())
		return;
	std::ifstream in(m_prefsPath);
	if (!in) return;
	std::string section;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		if (line.front() == '[' && line.back() == ']') {
			section = line.substr(1, line.size() - 2);
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue; // unknown line: defaults stand
		const std::string key = line.substr(0, eq);
		const std::string val = line.substr(eq + 1);
		try {
			// Same clamps as the panel splitters (a hand-edited
			// prefs value must not degenerate the layout â€” a
			// sub-tab-height dock, for instance, made the console
			// tab look dead).
			if (section == "outliner" && key == "width")
				m_outlinerW = std::clamp(std::stof(val), 200.0f, 720.0f);
			else if (section == "inspector" && key == "width")
				m_inspectorW = std::clamp(std::stof(val), 200.0f, 720.0f);
			else if (section == "bottomDock" && key == "height")
				m_bottomDockH = std::clamp(std::stof(val), 120.0f, 600.0f);
			// [scratch] dir retired: no scratch disk anymore (the cache
			// is project-owned) — a stale entry is ignored.
			else if (section == "project" && key == "dir")
				m_projectRoot = val;
			// Recents (most recent first; recentN index-ordered).
			else if (section == "projects"
				&& key.rfind("recent", 0) == 0 && key.size() > 6) {
				const int idx = std::atoi(key.c_str() + 6);
				if (idx >= 0 && idx < 12 && !val.empty()) {
					if (size_t(idx) >= m_recentProjects.size())
						m_recentProjects.resize(size_t(idx) + 1);
					m_recentProjects[size_t(idx)] = val;
				}
			}
			// preview key retired: the editor passes are the
			// viewport driver unconditionally (no toggle to persist).
		}
		catch (...) {
			// corrupt value: keep the default
		}
	}
}

void EditorApp::SavePrefs() {
	if (m_prefsPath.empty())
		return;
	std::filesystem::create_directories(std::filesystem::path(m_prefsPath).parent_path());
	std::ofstream out(m_prefsPath);
	if (!out) return;
	out << "# Tourmaline Editor prefs (panel layout, px)\n";
	out << "[outliner]\nwidth=" << m_outlinerW << "\n";
	out << "[inspector]\nwidth=" << m_inspectorW << "\n";
	out << "[bottomDock]\nheight=" << m_bottomDockH << "\n";
	if (!m_projectRoot.empty())
		out << "[project]\ndir=" << m_projectRoot << "\n";
	if (!m_recentProjects.empty()) {
		out << "[projects]\n";
		for (size_t i = 0; i < m_recentProjects.size(); ++i)
			out << "recent" << i << "=" << m_recentProjects[i] << "\n";
	}
}

bool EditorApp::OpenCacheDb() {
	m_cacheDb.Close();
	m_store.reset();
	m_texCacheAdapter.reset();
	// Project-only: the artifact cache lives in the project's
	// .tourmaline/ dir (cache.db + assets.db + pak/ per-class paks). Without
	// a project the cache stays off — imports/cooks still work (the
	// cache only accelerates processing, never gates it).
	if (m_projectRoot.empty()) {
		Log("Artifact cache off — no project open (File > New / Open "
			"Project...).");
		return false;
	}
	std::string root = ProjectArtifactDir();
	root = smvk::cache::EnsureRegistryRoot(root);
	if (root.empty()) {
		Log("Artifact cache unavailable (cannot create '"
			+ ProjectArtifactDir() + "') — cache disabled for this "
			"session.");
		return false;
	}
	const std::filesystem::path dbPath =
		std::filesystem::path(root) / "cache.db";
	std::string err;
	if (!m_cacheDb.Open(dbPath.generic_string(), err)) {
		Log("Artifact cache db open failed: " + err);
		return false;
	}
	m_store = std::make_unique<smvk::cache::ArtifactStore>(
		root, m_cacheDb);
	m_texCacheAdapter =
		std::make_unique<StoreArtifactCacheAdapter>(*m_store);
	std::string cerr;
	const uint64_t count = m_cacheDb.Count(cerr);
	Log("Artifact cache: " + root + " (cache.db ready, "
		+ std::to_string(count) + " artifact(s) registered)"
		+ " [project '" + m_projectName + "']");
	// Editor VFS: register the registry class paks (the cooked-resource
	// overlay the display path resolves through). ONLY tour + registry
	// mount here — never the release content tree (editor -> release
	// is one-way; reading content/paks back would feed stale cooked
	// content into authoring). The scene class is consumed by the
	// App (its own --pak VFS), not by this overlay; these are the
	// resource classes.
	for (const char* klass : { "texture", "mesh", "material", "light",
			"camera" }) {
		std::vector<uint8_t> img;
		std::string rerr;
		if (!m_store->ReadPakImage(klass, img, rerr))
			continue; // no artifacts of this class yet
		auto image = std::make_shared<const std::vector<uint8_t>>(
			std::move(img));
		std::string berr;
		auto backend = smvk::PakBackend::Open(image,
			std::string("registry-") + klass, berr);
		if (!backend)
			continue; // stale/corrupt class pak: cache, disposable
		const std::string name = std::string("registry-") + klass;
		m_editorVfs.Unmount(name);
		m_editorVfs.Mount(backend, 20, name);
		Log("Editor VFS: '" + name + "' registered (cooked "
			+ std::string(klass) + " artifacts).");
	}
	return true;
}

void EditorApp::CloseScene() {
	if (!m_world)
		return;
	SavePrefs();
	m_world.reset();
	m_gltfPath.clear();
	m_gltfHostPath.clear();
	m_importImages.clear();
	m_hasSglm = false;
	m_sglmPath.clear();
	m_sglmStatus = "no atlas loaded";
	m_sglmBuf[0] = '\0';
	ClearSglmLayers();
	m_cvolPreviews.clear();
	m_cvolPreviewMode = 0;
	m_overrideRules.clear();
	m_nextOverrideId = 1;
	m_refPlaceholders.clear();
	m_refPlaceholders.shrink_to_fit();
	m_references.clear();
 	m_materialAssets.clear(); // session pool-import records (fresh scene)
 	m_textureAssets.clear(); // cookie/.texture pool imports (fresh scene)
	m_refPhCluster.clear();
	m_envCurrent = UINT32_MAX;
	m_materialFocus = UINT32_MAX;
	m_materialTrail.clear();
	ClearNonDagSelection();
	m_imageFocus = UINT32_MAX;
	m_placement.graphs.clear();
	m_placement.active = 0;
	m_placement.nextGraph = 1;
	m_placement.selected = -1;
	m_placement.selectedLink = -1;
	m_placement.linkFrom = -1;
	m_placement.linkFromPin.clear();
	m_placement.pan[0] = 0.0f;
	m_placement.pan[1] = 0.0f;
	m_placement.undoStack.clear();
	m_placement.undoPos = -1;
	m_placement.preview = {};
	m_placement.showGraph = false;
	m_placement.showOverlay = true;
	m_placement.showRejected = true;
	// Flush the working state to the project .tour on close (per-edit
	// saves went to the %APPDATA% autosave; this is the one place the
	// real file catches up without an explicit save).
	if (m_world && !TourTarget().empty())
		SaveTour();
	m_terrainGroups.clear();
	m_deletedNodes.clear();
	m_images.clear();
	m_nextImageId = 1;
	m_refIssues.clear();
	m_refResolved.clear();
	m_defaultMatWorld = UINT32_MAX;
	ClearSelection();
	ClearHistory();
	m_tourDirty = false;
	Log("Scene closed.");
}

	bool EditorApp::LoadGltfIntoWorld(const std::string& pathIn,
		const std::string& tourOverride) {
		// Empty path = sourceless tour (DEPRECATED source mechanism): no
		// source to import — an empty world whose geometry comes entirely
		// from references. tourOverride pins the reference lookup to the
		// opened tour file (otherwise <stem>.tour next to the source,
		// which does not exist here).
		// THE MESH IMPORTER redirect (the glTF privileges are gone): a
		// .gltf/.glb NEVER loads as a session — the source routes through
		// the importer (derive-if-stale beside it, ImportMeshArtifactFor)
		// and the SESSION (m_gltfPath identity, the recorded definition
		// reference, SaveTour) names the derived .geometry. Every funnel
		// lands here: direct opens (OpenScene), tour-recorded sources
		// (OpenTourFile / ResolveTourSource) and reloads. A failed import
		// fails the load — NO glTF session fallback (the glTF is
		// import-only; the cooker/baker/session build refuse it outright).
		std::string path = pathIn;
		if (!path.empty()) {
			std::string ext =
				std::filesystem::path(path).extension().string();
			for (char& c : ext)
				c = char(std::tolower((unsigned char)c));
			if (ext == ".gltf" || ext == ".glb") {
				const std::string artifact = ImportMeshArtifactFor(path);
				if (artifact.empty())
					return false; // logged (no glTF session fallback)
				path = artifact;
			}
		}
		std::string loadPath = path;
		const bool directOpen = !path.empty(); // direct source open vs tour open
	// Texture-cache key scope for this import (GUID-namespaced when the
	// model lives in a project): identically-NAMED textures across models
	// stop thrashing the one (klass, uri) row. Nested reference imports
	// inherit this scope — consistent across sessions. Cleared at the end;
	// early returns are harmless (the next import re-scopes first).
	if (m_texCacheAdapter)
		m_texCacheAdapter->SetKeyScope(ProjectAssetGuid(loadPath));
	// The PARSE itself happens inside ApplySceneReferences (the import
	// system — per source, nested included); the editor never holds the
	// parsed model, its CPU geometry rides the clusters' retained uploads
	// and its image rows the importer records (m_importImages).
	WaitForDeviceIdle();
	// Fresh streamer per load: entries view the old cluster's stashed
	// payload bytes, so drop them before the world (and its clusters)
	// is replaced. GPU is idle, so teardown is safe.
	m_textureStreamer.reset();
	m_texHandleBySlot.clear();
	m_bindlessBySlot.clear();
	if (m_textureStreamingEnabled && m_texMode != TexMode::Full) {
		smvk::TextureStreamerConfig streamCfg;
		streamCfg.budgetBytes = 256ull * 1024 * 1024;
		// Coarsest-only mode pins the floor to the single coarsest mip
		// with no demand and no hysteresis (one Update() decays all).
		streamCfg.minResidentMips = (m_texMode == TexMode::Coarse) ? 1 : 2;
		streamCfg.initialResidentDimCap = 512; // cheapest tail first
		streamCfg.maxUpgradesPerFrame = 4;
		streamCfg.maxDowngradesPerFrame = 8;
		streamCfg.evictCooldownFrames = (m_texMode == TexMode::Coarse) ? 0 : 60;
		streamCfg.mipBias = 0.0f;
		// AUTHORING policy (see the config): the editor's clusters stash
		// SOURCE bytes by design (a foreign Basis .ktx2 rides the pool
		// verbatim until the cook converts it), so demand retargets that
		// re-decode the source MUST transcode in place (like the first
		// load does) — correct content with a hitch, never the pink
		// error texture. Cooked runs keep the default (refuse + pink).
		streamCfg.allowRetargetTranscode = true;
		m_textureStreamer =
			smvk::mem::MakeProcess<smvk::TextureStreamer>(*m_context, streamCfg);
	}
	m_world = smvk::mem::MakeProcess<smvk::World>();
	smvk::Cluster& cluster = m_world->LoadCluster(*m_context);
	// CPU geometry mirror (the editor's picking/UV/placement source):
	// every mesh this import lands keeps a retained StaticMeshUpload
	// (Cluster::GetRetainedUpload) — THE replacement for the old kept
	// tinygltf::Model (picking walks the world's asset registry).
	cluster.SetRetainUploads(true);
	// THE DEFINITION BUILD: the world assembles from the tour doc.
	// Reference #0 is the source glTF itself (a ROOT reference â€” no
	// placeholder, no namespace, cluster ids match a direct import
	// exactly); every other reference inlines beneath it; the command
	// log replays on top (below). No side imports: `path` non-empty
	// is a direct glTF open (its file IS the root reference's
	// source); empty means the opened tour carries the source.
	addNode(cluster.scene, -1, 0); // empty scene root
	Log(std::string("Binding scene definition (textures: ") +
		(m_textureStreamer ? (m_texMode == TexMode::Coarse ? "coarsest-only" : "streaming") : "full") +
		")...");
	m_references.clear();
	m_materialAssets.clear(); // rebuilt from the tour's records below
	m_textureAssets.clear(); // cookie/.texture imports (rebuilt below)
	m_refPlaceholders.clear();
	m_refPhCluster.clear();
	m_refIssues.clear();
	m_refResolved.clear();
	std::string rootSource; // bound source ("" = sourceless scene)
	{
		smvk::tour::Doc refDoc;
		std::string refErr;
		std::vector<std::string> refWarns;
		// The tour FILE (never the autosave): recovery from an autosave
		// is an explicit user choice (recovery dialog), never silent.
		// An explicit tourOverride (definition-tour opens / temp
		// reloads) pins the lookup to that file; otherwise
		// <stem>.tour next to the source.
		std::filesystem::path masterTour = tourOverride.empty()
			? std::filesystem::path(path)
			: std::filesystem::path(tourOverride);
		if (tourOverride.empty())
			masterTour.replace_extension(".tour");
		const std::string latestTour = masterTour.string();
		const bool haveDoc = !latestTour.empty()
			&& smvk::tour::LoadTour(latestTour, refDoc, refErr, &refWarns);
		if (haveDoc)
			for (auto& w : refWarns)
				Log(w);
		// The root reference leads the definition. Direct opens carry
		// the source (the file the user opened wins over any recorded
		// variant); tour opens take the doc's root reference as-is.
		if (directOpen) {
			smvk::tour::ReferenceCmd rootRef{};
			rootRef.root = true;
			rootRef.parent = 0;
			rootRef.source = loadPath;
			// The root reference's GUID = the SOURCE CONTENT identity
			// (smvk::assets::SourceAssetGuid — the same one the derived
			// .meta records): project sessions and from-zero re-imports
			// agree on ONE identity. (The project DB's guid is separate
			// bookkeeping — the tex cache scope above; the artifact
			// identity is the source's, not the DB's.)
			rootRef.guid = smvk::assets::SourceAssetGuid(loadPath);
			rootRef.name = std::filesystem::path(loadPath).stem().string();
			if (haveDoc && !refDoc.references.empty()
				&& refDoc.references[0].root)
				refDoc.references[0] = rootRef;
			else
				refDoc.references.insert(refDoc.references.begin(),
					rootRef);
		}
		if (!refDoc.references.empty()) {
			// Meshlets + LOD siblings at import (the live world renders
			// through the same GPU filter + LOD selection as cooked paks
			// - the editor view modes match the App's, and the F3/F5
			// views work on live worlds). Configs mirror the cooker
			// defaults (meshlets 64v/64t — the kMeshletMax constants —
			// and LOD ratios 60/30/12%) and ride
			// into every nested reference (ApplySceneReferences forwards
			// them). Kill-switches for giant imports
			// (TOURMALINE_EDITOR_MESHLETS=0 / TOURMALINE_EDITOR_LODS=0).
			smvk::MeshOptConfig importMeshOpt{}; // weld/cache/overdraw/fetch on
			importMeshOpt.generateLods = true;   // sibling blobs <base>/lodN
			smvk::MeshletConfig importMeshlets{};
			importMeshlets.enabled = true;
			if (const char* e = std::getenv("TOURMALINE_EDITOR_MESHLETS")) {
				if (std::atoi(e) == 0)
					importMeshlets.enabled = false;
			}
			if (const char* e = std::getenv("TOURMALINE_EDITOR_LODS")) {
				if (std::atoi(e) == 0)
					importMeshOpt.generateLods = false;
			}
			// Importer data out (see GltfImportImage): one row per
			// referenced image (loaded AND failed, users included) —
			// the path editor's texture rows replace the parsed model.
			m_importImages.clear();
			const size_t applied = ApplySceneReferences(cluster, refDoc,
				std::filesystem::path(loadPath).parent_path()
					.generic_string(),
				m_context.get(), m_textureStreamer.get(),
				nullptr /* host files — cook-parity resolution */,
				importMeshOpt, importMeshlets, 0,
			&m_refPhCluster, &m_refIssues, &m_refResolved,
			/*autoKtx=*/true, /*fileNodes=*/nullptr,
			/*artifactCache=*/m_texCacheAdapter.get(),
			/*guidResolver=*/&m_guidResolver,
			/*outImages=*/&m_importImages);
			Log("Definition: bound " + std::to_string(applied) +
				" reference subtree(s) (" +
				std::to_string(refDoc.references.size()) +
				" top-level).");
			// Editor-side mirror of the definition (save round-trip).
			m_references = refDoc.references;
			if (m_references[0].root) {
				rootSource = m_references[0].source;
				// Canonical host path the source bound to (picking,
				// Run-in-App, rebinds).
				loadPath = (!m_refResolved.empty()
					&& !m_refResolved[0].empty())
					? m_refResolved[0] : rootSource;
			}
		}
	}
	// (Tour-opened scenes need no extra parse: the definition's imports
	// landed retained uploads + m_importImages rows; the source binds
	// inside ApplySceneReferences.)
	m_context->FlushUploads();
	Log("Resolving world...");
	m_world->ResolveAndFixup_clusters();
	// LOD world-error finalize over the MERGED world: per-reference
	// imports finalize with partial globals (later references add more
	// instances), so re-run here where globals are final. Idempotent
	// (un-bakes the previous scale first) and a no-op without LOD
	// siblings — without it multi-reference LOD switch distances are
	// wrong (single-reference worlds are already exact from import).
	smvk::FinalizeLodWorldErrors(cluster);
	// Import geometry summary: meshlets + LOD chains feed the GPU filter
	// (F3 meshlets view, HZB culling) and the LOD selection (F5 view) on
	// live worlds, matching cooked paks.
	{
		uint32_t meshedMeshes = 0, meshletTotal = 0;
		cluster.staticMeshes.forEachHandle(
			[&](smvk::ClusterStaticMeshHandle h) {
				if (const smvk::StaticMesh* m = cluster.staticMeshes.get(h)) {
					if (m->meshlets && !m->meshlets->empty()) {
						++meshedMeshes;
						meshletTotal +=
							static_cast<uint32_t>(m->meshlets->meshlets.size());
					}
				}
			});
		uint32_t lodChains = 0, lodSiblings = 0;
		for (const smvk::LodInfo& li : m_world->meshLodInfos) {
			if (li.candidateCount > 1) {
				++lodChains;
				lodSiblings += li.candidateCount - 1;
			}
		}
		char mbuf[256];
		std::snprintf(mbuf, sizeof(mbuf),
			"Import geometry: %u meshes with meshlets (%u meshlets), "
			"%u LOD chains (%u siblings)",
			meshedMeshes, meshletTotal, lodChains, lodSiblings);
		Log(mbuf);
	}
	RebuildTexHandleMap();
	if (m_textureStreamer) {
		const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
		char sbuf[256];
		std::snprintf(sbuf, sizeof(sbuf),
			"Textures: %u streamed, resident %.1f MiB / full %.1f MiB (%u full-res)",
			stats.textureCount,
			double(stats.residentBytes) / (1024.0 * 1024.0),
			double(stats.fullBytes) / (1024.0 * 1024.0),
			stats.fullResidentCount);
		Log(sbuf);
	}

	// Scene identity: the opened path (direct opens keep their identity,
	// scheme paths included); tour opens take the definition's root
	// reference source.
	m_gltfPath = path.empty() ? rootSource : path;
	m_gltfHostPath = loadPath;
	m_tourPath = TourPath(); // fresh scene: default target (save-as changes it)
	m_hasSglm = false;
	m_sglm = smvk::gi::SgLightmapAtlas{};
	m_sglmPath.clear();
	ClearSglmLayers();
	m_sglmStatus = "no atlas loaded (glTF source has no bake)";
	ClearTexPreview();
	m_selected = UINT32_MAX;
	ClearHistory();
	m_terrainGroups.clear(); // fresh world: stale group grids die with it
	m_deletedNodes.clear(); // fresh world: deletes die with it
	m_images.clear(); // fresh world: image defs die with it
	m_nextImageId = 1;
	// (m_refIssues/m_refResolved are NOT cleared here: the reference
	// bind above just filled them for THIS world â€” clearing would wipe
	// the panel's reporting. They reset at load start/close instead.)
	m_defaultMatWorld = UINT32_MAX;
	m_overrideRules.clear();
	m_nextOverrideId = 1;
	// Scene-backed env nodes are restored by LoadTour (tour env blocks);
	// only the selection resets here.
	m_envCurrent = UINT32_MAX;
	m_materialFocus = UINT32_MAX; // inspector navigation state
	m_materialTrail.clear();
	m_imageFocus = UINT32_MAX;
	ClearSelection();
	ResetMeshletDebug(); // cooked meshlets are per-scene
	// Cluster base for tour id translation (single-cluster scenes only).
	m_clusterBase = UINT32_MAX;
	if (m_world->clusters.size() == 1 &&
		m_world->scene.hierarchy.size() ==
		1 + m_world->clusters[0]->scene.hierarchy.size())
		m_clusterBase = 1;
	// Map the reference placeholder CLUSTER ids -> world ids (cluster
	// base just became valid). Root references carry a -1 sentinel (no
	// placeholder by design) â€” mapped to UINT32_MAX so the parallel
	// arrays stay index-aligned with m_references.
	if (!m_refPhCluster.empty()) {
		m_refPlaceholders.clear();
		for (int cn : m_refPhCluster)
			m_refPlaceholders.push_back(cn < 0
				? UINT32_MAX : ClusterToWorld(uint32_t(cn)));
		m_refPhCluster.clear();
	}
	// Node indexing died with the kept glTF (picking resolves through the
	// world's asset registry + retained uploads); the tour replays next.
	Log("Loading tour...");
	if (!tourOverride.empty() && directOpen) {
		// Temp-doc reload (reference Reload): the snapshot â€” not the
		// on-disk file â€” is the document. No autosave offer either:
		// the temp already IS the latest working state.
		smvk::tour::Doc tmpDoc;
		std::string tmpErr;
		std::vector<std::string> tmpWarns;
		if (smvk::tour::LoadTour(tourOverride, tmpDoc, tmpErr,
				&tmpWarns)) {
			for (auto& w : tmpWarns)
				Log(w);
			ReplayTourDoc(tmpDoc, path);
		}
		else {
			Log("reload snapshot unreadable (" + tmpErr
				+ ") â€” falling back to the tour file.");
			LoadTour();
			CheckAutosaveOffer();
		}
	}
	else if (!tourOverride.empty()) {
		// Opened definition tour: OpenTourFile replays its own doc
		// right after this build â€” nothing loads here (a second load
		// would double-apply the commands).
	}
	else {
		LoadTour(); // replays <stem>.tour into the undo stack when present
		// A newer (or orphan) autosave never loads silently: offer recovery
		// as an explicit choice instead (recovery dialog).
		CheckAutosaveOffer();
	}
	// Reference checkbox = the placeholder visibility AUTHORITY, applied
	// AFTER the tour replay: disabled refs hide their subtrees (nodes
	// kept, ids stable â€” the per-frame visibility filters stop draws/
	// picks and the outliner drops the tree), enabled refs clear any
	// stale inherited/legacy hidden state (an old session's visibility
	// command must never outvote the checkbox). Runs here because the
	// placeholder WORLD ids (m_refPlaceholders) only resolve above.
	RebuildDisabledRefHides();
	// Texture relinks ride the SOURCE assets' .meta input plugs now (the
	// importer applies them during the definition build above) — no
	// post-import material-slot patching.
	// Bake-output probes for the live view extras (scene-id keyed, never
	// pak siblings — paks run in the App, not the editor).
	TryLoadSceneSglm();
	TryLoadCvolumes();
	RebuildCookedLights(); // census + seeds the viewport sun from the scene
	EnsureDefaultMaterial(); // editor fallback material (editor://default)

	// Fit the default perspective camera to the scene bounds.
	const ::Aabb bounds = m_world->ComputeSceneBounds();
	if (bounds.mx.x >= bounds.mn.x) {
		const glm::vec3 center = (bounds.mn + bounds.mx) * 0.5f;
		const float radius = glm::length(bounds.mx - bounds.mn) * 0.5f;
		SetupDefaultCameras();
		EditorCam& fitCam = m_editorCams[m_viewports[0].camIdx];
		fitCam.target = center;
		fitCam.dist = std::max(radius * 2.2f, 1.0f);
	}
	// Import finished: commit any texture-conversion artifacts the
	// import staged into the project's artifact cache (one pak grow
	// per touched class — a flush per import batch, not per texture).
	// Cache-off or an empty stage = a no-op.
	if (m_store && m_store->StagedCount() > 0) {
		const size_t stagedCount = m_store->StagedCount();
		std::string flushErr;
		if (m_store->Flush(flushErr))
			Log("Artifact cache: " + std::to_string(stagedCount)
				+ " texture artifact(s) flushed.");
		else
			Log("Artifact cache flush failed: " + flushErr
				+ " (artifacts stay staged; nothing is lost).");
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf), "Loaded '%s' (nodes=%zu meshes=%zu materials=%zu)",
		path.c_str(), m_world->scene.hierarchy.size(),
		m_world->meshLocations.size(), m_world->materialLocations.size());
	Log(buf);
	// Import done: drop the texture-cache key scope (bare-uri keys for
	// anything importing outside this path).
	if (m_texCacheAdapter)
		m_texCacheAdapter->SetKeyScope({});
	return true;
}

std::string EditorApp::GuidResolverHook::PathForGuid(const std::string& guid) {
	// Closed db (no project / cache off) answers "": the reference falls
	// back to its recorded -source path — exactly the pre-GUID behavior.
	// Unknown guid: ONE rescan before giving up (mirrors
	// ResolveReferenceAssetEx) — a guid minted by another session's
	// import (or a manifest entry the rescan had not seen yet: the
	// geometry= parser gap) heals here instead of silently skipping the
	// reference.
	std::string path;
	if (!app.m_assetDb.IsOpen())
		return {};
	if (!app.m_assetDb.PathForGuid(guid, path) || path.empty()) {
		std::string dberr;
		smvk::cache::RescanStats stats;
		if (app.m_assetDb.RescanProject(stats, dberr))
			app.m_assetDb.PathForGuid(guid, path);
	}
	if (path.empty())
		return {};
	return path;
}


std::string EditorApp::SceneIdForHost() const {
	// Scene id (the open scene's stem; the resolved source
	// wins — paks never open here). Shared by the launch and the in-place
	// rebind (the GI sidecar loaders resolve gi/<id>.* through the
	// project-aware VFS).
	namespace fs = std::filesystem;
	if (!m_gltfHostPath.empty())
		return fs::path(m_gltfHostPath).stem().string();
	if (!m_gltfPath.empty())
		return fs::path(m_gltfPath).stem().string();
	return std::string("scene");
}

bool EditorApp::TryLoadSceneSglm() {
	m_hasSglm = false;
	m_sglm = smvk::gi::SgLightmapAtlas{};
	m_sglmPath.clear();
	// Candidate stems: the clusters' lightmap Gi bindings first (the
	// live-view rebind resolved the bake stems), then the scene id (the
	// stable sidecar convention). Candidate ROOTS mirror the binding
	// resolution: the project content first, the bundled content second.
	std::vector<std::string> stems;
	if (m_world) {
		for (const auto& clusterPtr : m_world->clusters) {
			if (!clusterPtr)
				continue;
			for (const smvk::GiBinding& b : clusterPtr->giBindings) {
				if (b.kind == smvk::GiBinding::Kind::Lightmap &&
					!b.name.empty() &&
					std::find(stems.begin(), stems.end(), b.name) == stems.end())
					stems.push_back(b.name);
			}
		}
	}
	const std::string stem = SceneIdForHost();
	if (!stem.empty() &&
		std::find(stems.begin(), stems.end(), stem) == stems.end())
		stems.push_back(stem);
	std::vector<std::string> roots;
	if (!m_projectRoot.empty())
		roots.push_back(m_projectRoot);
	roots.push_back("content");
	std::vector<std::string> candidates;
	for (const std::string& r : roots) {
		for (const std::string& s : stems)
			candidates.push_back(
				(std::filesystem::path(r) / "gi" / (s + ".sglm"))
					.lexically_normal().string());
	}
	for (const std::string& c : candidates) {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(c, ec))
			continue;
		if (LoadSglm(c))
			return true;
	}
	m_sglmStatus = "no baked atlas found for '" + stem
		+ "' (tried " + std::to_string(candidates.size()) + " paths)";
	return false;
}

bool EditorApp::LoadSglm(const std::string& sglmPath) {
	smvk::gi::SgLightmapAtlas atlas;
	std::string err;
	if (!smvk::gi::SgLightmapAtlas::ReadFile(sglmPath, atlas, err)) {
		m_hasSglm = false;
		m_sglmStatus = "sglm load failed: " + err;
		Log("Lightmap atlas load failed '" + sglmPath + "': " + err);
		return false;
	}
	m_sglm = std::move(atlas);
	m_sglmPath = sglmPath;
	m_hasSglm = true;
	std::snprintf(m_sglmBuf, sizeof(m_sglmBuf), "%s", sglmPath.c_str());
	uint32_t l5 = 0, l6 = 0, l9 = 0, l12 = 0;
	for (const auto& c : m_sglm.chunks) {
		if (c.lobeCount == 5) ++l5;
		else if (c.lobeCount == 6) ++l6;
		else if (c.lobeCount == 9) ++l9;
		else ++l12;
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf),
		"Lightmap atlas '%s': %ux%u chunks=%zu (5:%u 6:%u 9:%u 12:%u) gpu=%.1f MiB",
		sglmPath.c_str(), m_sglm.width, m_sglm.height, m_sglm.chunks.size(),
		l5, l6, l9, l12, double(m_sglm.GpuBytes()) / (1024.0 * 1024.0));
	m_sglmStatus = buf;
	Log(m_sglmStatus);
	UploadSglmLayers();
	return true;
}

void EditorApp::ClearSglmLayers() {
	for (auto& l : m_lmLayers)
		l = {};
	for (auto& s : m_lmLayerSlots)
		s = UINT32_MAX;
	m_lmLayerKind = 0;
	m_lmGpu = false;
}

// ---------------------------------------------------------------------------
// Convex GI volume preview data (probes-only pivot). Loads the
// gi/<scene>.cvmanifest + .cvol BAKE sidecars (project content first,
// bundled content second — the same roots the live view's bindings
// resolve) and builds each volume's hull: the bake AABB (gridMin..
// gridMin + dims*spacing) clipped by the volume's plane set (3D
// Sutherland-Hodgman). The hull feeds the LIGHTING-mode view option
// (wireframe / translucent fill, color-coded by priority) — draw lives
// in DrawCvolPreviewOverlay (editor_viewports.cpp).
// ---------------------------------------------------------------------------
bool EditorApp::TryLoadCvolumes() {
	m_cvolPreviews.clear();
	namespace fs = std::filesystem;
	const std::string stem = SceneIdForHost();
	if (stem.empty())
		return false;
	// Manifest candidates (same roots as the sglm probe).
	std::vector<fs::path> manifests;
	if (!m_projectRoot.empty())
		manifests.push_back(
			(fs::path(m_projectRoot) / "gi" / (stem + ".cvmanifest"))
				.lexically_normal());
	manifests.push_back((fs::path("content") / "gi" / (stem + ".cvmanifest")));
	fs::path manifestPath;
	std::error_code ec;
	for (const fs::path& m : manifests)
		if (fs::is_regular_file(m, ec)) {
			manifestPath = m;
			break;
		}
	if (manifestPath.empty())
		return false;
	// One .cvol vfs path per line (inline '#' comments + legacy guid
	// columns handled by the shared parser, like every other manifest
	// reader). Relative lines resolve against the CONTENT ROOT (the
	// manifest's parent's parent); absolute host paths pass through.
	std::ifstream in(manifestPath);
	if (!in)
		return false;
	const fs::path contentRoot = manifestPath.parent_path().parent_path();
	std::string line;
	int loaded = 0;
	while (std::getline(in, line)) {
		const std::string row = smvk::bake::SplitManifestRow(line);
		if (row.empty())
			continue;
		fs::path volPath = fs::path(row);
		if (volPath.is_relative())
			volPath = (contentRoot / volPath).lexically_normal();
		smvk::gi::CvVolume vol;
		std::string err;
		if (!smvk::gi::CvVolume::ReadFile(volPath.string(), vol, err)) {
			Log("Convex volume load failed '" + volPath.string() + "': " + err);
			continue;
		}
		CvolPreview pv;
		pv.name = volPath.stem().string();
		pv.priority = vol.def.priority;
		pv.gridMin = vol.gridMin;
		pv.gridMax = vol.gridMin
			+ glm::vec3(vol.dims[0], vol.dims[1], vol.dims[2]) * vol.spacing;
		// Hull: start with the 6 AABB faces, clip every polygon by every
		// plane (inside: n.P + d <= 0). Orientation does not matter for
		// the preview (wireframe + X-ray fill).
		{
			const glm::vec3 mn = pv.gridMin, mx = pv.gridMax;
			const glm::vec3 q[8] = {
				{ mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z },
				{ mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
				{ mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
				{ mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
			};
			static const int kFaces[6][4] = {
				{ 0, 1, 2, 3 }, { 5, 4, 7, 6 }, { 4, 0, 3, 7 },
				{ 1, 5, 6, 2 }, { 3, 2, 6, 7 }, { 4, 5, 1, 0 },
			};
			std::vector<std::vector<glm::vec3>> polys;
			for (int f = 0; f < 6; ++f)
				polys.push_back({ q[kFaces[f][0]], q[kFaces[f][1]],
					q[kFaces[f][2]], q[kFaces[f][3]] });
			for (const smvk::gi::CvPlane& plane : vol.def.planes) {
				const auto inside = [&](const glm::vec3& p) {
					return glm::dot(plane.normal, p) + plane.d <= 0.0f;
				};
				std::vector<std::vector<glm::vec3>> clipped;
				for (std::vector<glm::vec3>& poly : polys) {
					if (poly.size() < 3)
						continue;
					std::vector<glm::vec3> out;
					out.reserve(poly.size() + 1);
					for (size_t i = 0; i < poly.size(); ++i) {
						const glm::vec3& a = poly[i];
						const glm::vec3& b = poly[(i + 1) % poly.size()];
						const bool inA = inside(a), inB = inside(b);
						if (inA)
							out.push_back(a);
						if (inA != inB) {
							const float da = glm::dot(plane.normal, a) + plane.d;
							const float db = glm::dot(plane.normal, b) + plane.d;
							const float t = da / (da - db);
							out.push_back(glm::mix(a, b, std::clamp(t, 0.0f, 1.0f)));
						}
					}
					if (out.size() >= 3)
						clipped.push_back(std::move(out));
				}
				polys = std::move(clipped);
				if (polys.empty())
					break;
			}
			pv.polys = std::move(polys);
		}
		glm::vec3 centroidSum(0.0f);
		int centroidCount = 0;
		for (const auto& poly : pv.polys)
			for (const glm::vec3& p : poly) {
				centroidSum += p;
				++centroidCount;
			}
		pv.center = centroidCount > 0
			? centroidSum / float(centroidCount) : pv.gridMin;
		m_cvolPreviews.push_back(std::move(pv));
		++loaded;
	}
	if (loaded > 0) {
		Log("Convex GI volumes: loaded " + std::to_string(loaded)
			+ " from '" + manifestPath.string() + "' (J in LIGHTING toggles "
			+ "the preview)");
		return true;
	}
	return false;
}

void EditorApp::UploadSglmLayers() {
	// GPU mirror of the CPU atlas for the F8 baked-irradiance view (App
	// parity): the sglm v3 BC6H payload uploads verbatim as one
	// BC6H_UFloat texture per lobe (kind 1); v1/v2 files go through
	// SgLightmapAtlas::PackGpuLayers into 9 RGBA16F stream layers
	// (kind 0). Without a device (pre-Initialize) the CPU atlas still
	// serves the stats panel; the layers upload on the next LoadSglm.
	ClearSglmLayers();
	if (!m_context || !m_hasSglm || m_sglm.chunks.empty() ||
		m_sglm.width == 0 || m_sglm.height == 0)
		return;
	const bool bc6h = m_sglm.compression
		!= smvk::gi::SgLightmapCompression::Uncompressed;
	if (bc6h) {
		const size_t blockBytes = size_t(m_sglm.width / 4)
			* (m_sglm.height / 4) * 16;
		for (uint32_t l = 0; l < m_sglm.bc6hLobes; ++l) {
			smvk::ImageCreateInfo ci{};
			ci.extent = { m_sglm.width, m_sglm.height, 1 };
			ci.format = smvk::Format::BC6H_UFloat;
			ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
			ci.mipLevels = 1;
			ci.arrayLayers = 1;
			ci.name = "editor sglm bc6h lobe";
			m_lmLayers[l] = m_context->CreateTexture(ci,
				m_sglm.bc6hBlocks.data() + size_t(l) * blockBytes);
			if (m_lmLayers[l].empty()) {
				Log("Lightmap BC6H upload failed at lobe " + std::to_string(l));
				ClearSglmLayers();
				return;
			}
			m_lmLayerSlots[l] = m_context->GetTextureBindlessIndex(m_lmLayers[l]);
		}
		m_lmLayerKind = 1;
	} else {
		std::vector<uint16_t> layers[smvk::gi::kLightmapLayerCount];
		m_sglm.PackGpuLayers(layers);
		for (uint32_t t = 0; t < smvk::gi::kLightmapLayerCount; ++t) {
			smvk::ImageCreateInfo ci{};
			ci.extent = { m_sglm.width, m_sglm.height, 1 };
			ci.format = smvk::Format::RGBA16_Float;
			ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
			ci.mipLevels = 1;
			ci.arrayLayers = 1;
			ci.name = "editor sglm atlas layer";
			m_lmLayers[t] = m_context->CreateTexture(ci, layers[t].data());
			if (m_lmLayers[t].empty()) {
				Log("Lightmap GPU upload failed at layer " + std::to_string(t));
				ClearSglmLayers();
				return;
			}
			m_lmLayerSlots[t] = m_context->GetTextureBindlessIndex(m_lmLayers[t]);
		}
		m_lmLayerKind = 0;
	}
	m_lmGpu = true;
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"Lightmap GPU: %s %ux%u (%.1f MiB, F8 = baked view)",
		bc6h ? "BC6H, per-lobe" : "9x RGBA16F",
		m_sglm.width, m_sglm.height,
		double(m_sglm.GpuBytes()) / (1024.0 * 1024.0));
	Log(buf);
}

void EditorApp::UpdateTextureStreaming(smvk::Extent2D renderExtent) {
	if (!m_textureStreamingEnabled || !m_textureStreamer || !m_world)
		return;
	const uint32_t streamFrame = static_cast<uint32_t>(m_frameCounter);
	// Handle-based demand (NOT RequestMipsByDistance: that matches caller
	// keys against bindless slots, but MaterialValues::textures stores RHI
	// pool indices â€” equal only by creation-order coincidence. Resolving
	// the live handle per slot is exact either way). Coarsest-only mode
	// files no demand so Update() decays everything to the 1-mip floor.
	if ((m_frameCounter & 3u) == 0u && m_texMode == TexMode::Streaming &&
		!m_texHandleBySlot.empty()) {
		std::unordered_map<uint32_t, float> minDist;
		minDist.reserve(256);
		const glm::vec3 eye = EditorCamEye(ActiveCam());
		auto note = [&](int slot, float dist) {
			if (slot < 0)
				return;
			const uint32_t s = static_cast<uint32_t>(slot);
			auto it = minDist.find(s);
			if (it == minDist.end() || dist < it->second)
				minDist[s] = dist;
		};
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_Renderable,
			[&](const smvk::World::RenderItem& item) {
				if (item.material == smvk::World::kInvalidIndex)
					return;
				const smvk::Material* mat = m_world->GetMaterial(item.material);
				if (!mat)
					return;
				const glm::mat4& model = m_world->scene.globalTransform[item.node];
				const float dist = glm::distance(eye, glm::vec3(model[3]));
				note(mat->values.textures.x, dist);
				note(mat->values.textures.y, dist);
				note(mat->values.textures.z, dist);
				note(mat->values.textures.w, dist);
				note(mat->values.textures2.x, dist);
			});
		// Per-entry chain shape for the mip math (fresh each demand tick;
		// entries only change on retarget, ~15 Hz scan over a few hundred).
		std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> chain;
		chain.reserve(256);
		for (const auto& d : m_textureStreamer->GetEntriesDebug()) {
			const uint64_t key =
				(static_cast<uint64_t>(d.handleGen) << 32) | d.handleIndex;
			const uint32_t baseLong = std::max(
				d.residentWidth << std::min(d.residentBase, 24u),
				d.residentHeight << std::min(d.residentBase, 24u));
			chain[key] = { d.fullMips, std::max(baseLong, 1u) };
		}
		const float fovY = glm::radians(kEditorFovYDeg);
		const float bias = m_textureStreamer->GetConfig().mipBias;
		for (const auto& [slot, dist] : minDist) {
			auto hit = m_texHandleBySlot.find(slot);
			if (hit == m_texHandleBySlot.end() || hit->second.empty())
				continue;
			const uint64_t key = (static_cast<uint64_t>(hit->second.gen()) << 32)
				| hit->second.index();
			auto cit = chain.find(key);
			if (cit == chain.end())
				continue;
			const uint32_t want = smvk::TextureStreamer::WantedMipForDistance(
				cit->second.first, cit->second.second, dist,
				static_cast<float>(renderExtent.height), fovY, 2.0f, bias);
			m_textureStreamer->RequestMip(hit->second, want, streamFrame);
		}
	}
	m_textureStreamer->Update(streamFrame);
	const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
	if (stats.upgradesLastUpdate > 0 || stats.downgradesLastUpdate > 0)
		m_context->FlushUploads();
}

void EditorApp::RebuildTexHandleMap() {
	m_texHandleBySlot.clear();
	m_bindlessBySlot.clear();
	if (!m_world || !m_context)
		return;
	uint32_t mismatched = 0;
	for (const auto& clusterPtr : m_world->clusters) {
		if (!clusterPtr)
			continue;
		for (const auto& [uri, h] : clusterPtr->ListTextureUris()) {
			m_texHandleBySlot[h.index()] = h;
			const uint32_t bindless = m_context->GetTextureBindlessIndex(h);
			m_bindlessBySlot[h.index()] = bindless;
			if (bindless != h.index())
				++mismatched;
		}
	}
	if (!m_texHandleBySlot.empty()) {
		char buf[256];
		std::snprintf(buf, sizeof(buf),
			"Texture slots: %zu registered, %u pool!=bindless%s",
			m_texHandleBySlot.size(), mismatched,
			mismatched ? " (remapped at material upload)" : "");
		Log(buf);
	}
}

bool EditorApp::ApplyTexMode() {
	// A re-openable scene identity is required (the switch reimports; the
	// streamer must exist at import). Sourceless scenes have nothing to
	// reimport through.
	if (!m_world || m_gltfPath.empty()) {
		Log("Texture mode: only imported scenes can switch.");
		return false;
	}
	// The streamer must exist at import, so every switch reimports (the
	// tour file is current â€” every edit saves it â€” so edits replay back).
	Log("Texture mode switch: reimporting scene...");
	return OpenScene(m_gltfPath);
}

void EditorApp::DumpStreamerStats() {
	if (!m_textureStreamer) {
		Log("Streamer: none (full-resident textures).");
		return;
	}
	const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"Streamer: %u textures, resident %.1f MiB / full %.1f MiB, %u full-res (+%u/-%u last)",
		stats.textureCount,
		double(stats.residentBytes) / (1024.0 * 1024.0),
		double(stats.fullBytes) / (1024.0 * 1024.0),
		stats.fullResidentCount,
		stats.upgradesLastUpdate, stats.downgradesLastUpdate);
	Log(buf);
	auto entries = m_textureStreamer->GetEntriesDebug();
	uint32_t shown = 0;
	for (const auto& d : entries) {
		if (++shown > 12)
			break;
		std::snprintf(buf, sizeof(buf),
			"  %-40.40s base=%u want=%u %ux%u",
			d.name.c_str(), d.residentBase, d.desiredBase,
			d.residentWidth, d.residentHeight);
		Log(buf);
	}
	if (entries.size() > 12) {
		std::snprintf(buf, sizeof(buf), "  ... +%zu more",
			entries.size() - 12);
		Log(buf);
	}
}

void EditorApp::UpdateCamera(float dt) {
	(void)dt;
	// Orbit-only cameras (no WASD/QE fly: held movement keys leaked into
	// text fields and fought the editor). Navigate with Alt+LMB/MMB/RMB,
	// the wheel, and F / Shift+F framing. Everything acts on the ACTIVE
	// viewport's camera. ORTHOGRAPHIC cameras (Top/Side) keep their axis
	// lock: tumble is disabled so controls can never coerce them into
	// angled perspective-like views â€” only the Projection combo changes
	// the type. Ortho zoom scales the half-height.
	SetupDefaultCameras();
	// The UV editor window owns its wheel + MMB (window zoom/pan in
	// DrawUvEditorWindow via ImGui): the 3D orbit camera never moves
	// while the UV canvas is hovered.
	if (m_showUvEditor && m_uvWinHovered)
		return;
	// Locked look-through: orbit navigation is parked while the active
	// viewport follows a live camera node (move the node via gizmo or
	// the transform inspector â€” the view tracks it). A lost node falls
	// back to orbit, where navigation works again.
	{
		const int b = m_viewports[m_activeVp].boundNode;
		if (b >= 0 && m_world && FindNodeCamera(uint32_t(b)) != nullptr)
			return;
	}
	EditorCam& cam = ActiveCam();
	const bool isOrtho = cam.type == EditorCam::Type::Orthographic;
	const glm::vec3 fwd = glm::normalize(
		EditorCamEye(cam) - cam.target);
	const bool imguiEats = m_imgui && m_imgui->WantsCaptureMouse();
	const bool alt = m_input.isKeyHeld(GLFW_KEY_LEFT_ALT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_ALT);
	if (!imguiEats && !m_gizmoDragging) {
		if (alt) {
			if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT) &&
				!isOrtho) {
				cam.yaw -= float(m_input.getMouseDeltaX()) * 0.005f;
				cam.pitch -= float(m_input.getMouseDeltaY()) * 0.005f;
				cam.pitch = std::clamp(cam.pitch, -1.55f, 1.55f);
			}
			if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_MIDDLE)) {
				glm::vec3 right = glm::cross(fwd, glm::vec3(0, 1, 0));
				if (glm::length(right) < 1e-6f)
					right = glm::vec3(1, 0, 0);
				right = glm::normalize(right);
				const glm::vec3 camUp = glm::normalize(glm::cross(right, fwd));
				const float k = isOrtho
					? cam.orthoHalfH * 0.0018f
					: cam.dist * 0.0016f;
				// Track: horizontal follows the pivot-follows-cursor
				// convention; vertical is screen-space grab (the scene
				// slides WITH the cursor). SDL deltas are Y-down
				// (mouse down = +dy).
				cam.target += right * float(m_input.getMouseDeltaX()) * k;
				cam.target += camUp * float(m_input.getMouseDeltaY()) * k;
			}
			if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_RIGHT)) {
				const float dy = float(m_input.getMouseDeltaY());
				if (isOrtho)
					cam.orthoHalfH = std::clamp(
						cam.orthoHalfH * (1.0f + dy * 0.005f), 0.05f, 5000.0f);
				else
					cam.dist = std::clamp(cam.dist * (1.0f + dy * 0.005f),
						0.2f, 500.0f);
			}
		}
		else {
			const bool orbitBtn =
				m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_RIGHT) ||
				(m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT) &&
					m_selected == UINT32_MAX && !m_vpBoxDragging);
			if (orbitBtn && !isOrtho) {
				cam.yaw -= float(m_input.getMouseDeltaX()) * 0.005f;
				cam.pitch -= float(m_input.getMouseDeltaY()) * 0.005f;
				cam.pitch = std::clamp(cam.pitch, -1.55f, 1.55f);
			}
		}
	}
	// Wheel zoom (perspective: distance; ortho: half-height).
	const float wheel = float(m_input.getScrollDeltaY());
	if (wheel != 0.0f && !(m_imgui && m_imgui->WantsCaptureMouse())) {
		if (cam.type == EditorCam::Type::Orthographic)
			cam.orthoHalfH = std::clamp(
				cam.orthoHalfH * (wheel > 0 ? 0.9f : 1.1f), 0.05f, 5000.0f);
		else
			cam.dist = std::clamp(cam.dist * (wheel > 0 ? 0.9f : 1.1f),
				0.2f, 500.0f);
	}
}

bool EditorApp::SelectionBounds(glm::vec3& outCenter, float& outRadius) {
	if (!m_world || m_selected == UINT32_MAX ||
		m_selected >= m_world->scene.hierarchy.size())
		return false;
	// Union AABB over the selected subtree's mesh nodes (same union the
	// gizmo highlight draws in editor_gizmo.cpp).
	glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
	bool has = false;
	std::vector<uint32_t> stack{ m_selected };
	while (!stack.empty()) {
		const uint32_t n = stack.back();
		stack.pop_back();
		if (n >= m_world->scene.hierarchy.size())
			continue;
		for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling)
			stack.push_back(uint32_t(c));
		auto mit = m_world->scene.meshForNode.find(n);
		if (mit == m_world->scene.meshForNode.end())
			continue;
		smvk::StaticMesh* mesh = m_world->GetMesh(mit->second);
		if (!mesh)
			continue;
		const glm::mat4& model = m_world->scene.globalTransform[n];
		for (int k = 0; k < 8; ++k) {
			const glm::vec3 corner(
				(k & 1) ? mesh->geometry.aabbMax.x : mesh->geometry.aabbMin.x,
				(k & 2) ? mesh->geometry.aabbMax.y : mesh->geometry.aabbMin.y,
				(k & 4) ? mesh->geometry.aabbMax.z : mesh->geometry.aabbMin.z);
			const glm::vec3 w = glm::vec3(model * glm::vec4(corner, 1.0f));
			mn = glm::min(mn, w);
			mx = glm::max(mx, w);
			has = true;
		}
	}
	if (!has) {
		// No mesh under the selection: pivot on the node origin.
		outCenter = glm::vec3(m_world->scene.globalTransform[m_selected][3]);
		outRadius = 0.5f;
		return true;
	}
	outCenter = (mn + mx) * 0.5f;
	outRadius = glm::length(mx - mn) * 0.5f;
	return true;
}

void EditorApp::FrameSelection() {
	glm::vec3 center{ 0.0f };
	float radius = 0.0f;
	SetupDefaultCameras();
	EditorCam& cam = ActiveCam();
	if (SelectionBounds(center, radius)) {
		cam.target = center;
		if (cam.type == EditorCam::Type::Orthographic)
			cam.orthoHalfH = std::max(radius * 1.2f, 0.5f);
		else
			cam.dist = std::max(radius * 2.2f, 1.0f);
	}
	else {
		FrameAll();
	}
}

bool EditorApp::SelectionPivotCenter(glm::vec3& outCenter) {
	// The MULTI-SELECT pivot: the combined world AABB center over
	// EVERY selected node's subtree meshes (Maya-style — the local
	// pivot the gizmo transforms about). Single selection = the
	// primary's subtree bounds (SelectionBounds semantics); nothing
	// selected = false.
	if (!m_world || m_selected == UINT32_MAX
		|| m_selected >= m_world->scene.hierarchy.size())
		return false;
	if (m_selection.size() <= 1) {
		glm::vec3 c;
		float r = 0.0f;
		if (SelectionBounds(c, r)) {
			outCenter = c;
			return true;
		}
		return false;
	}
	glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
	bool has = false;
	std::unordered_set<uint32_t> seen;
	for (uint32_t root : m_selection) {
		if (root >= m_world->scene.hierarchy.size()
			|| !seen.insert(root).second)
			continue;
		std::vector<uint32_t> stack{ root };
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			if (n >= m_world->scene.hierarchy.size()
				|| !seen.insert(n).second)
				continue;
			for (int c = m_world->scene.hierarchy[n].firstChild;
				c != -1;
				c = m_world->scene.hierarchy[c].nextSibling)
				stack.push_back(uint32_t(c));
			auto mit = m_world->scene.meshForNode.find(n);
			if (mit == m_world->scene.meshForNode.end())
				continue;
			smvk::StaticMesh* mesh = m_world->GetMesh(mit->second);
			if (!mesh)
				continue;
			const glm::mat4& model =
				m_world->scene.globalTransform[n];
			for (int k = 0; k < 8; ++k) {
				const glm::vec3 corner(
					(k & 1) ? mesh->geometry.aabbMax.x
					: mesh->geometry.aabbMin.x,
					(k & 2) ? mesh->geometry.aabbMax.y
					: mesh->geometry.aabbMin.y,
					(k & 4) ? mesh->geometry.aabbMax.z
					: mesh->geometry.aabbMin.z);
				const glm::vec3 w =
					glm::vec3(model * glm::vec4(corner, 1.0f));
				mn = glm::min(mn, w);
				mx = glm::max(mx, w);
				has = true;
			}
		}
	}
	if (!has)
		return false;
	outCenter = (mn + mx) * 0.5f;
	return true;
}

void EditorApp::FrameAll() {
	if (!m_world)
		return;
	const ::Aabb bounds = m_world->ComputeSceneBounds();
	if (bounds.mx.x < bounds.mn.x)
		return;
	SetupDefaultCameras();
	EditorCam& cam = ActiveCam();
	cam.target = (bounds.mn + bounds.mx) * 0.5f;
	if (cam.type == EditorCam::Type::Orthographic)
		cam.orthoHalfH = std::max(
			glm::length(bounds.mx - bounds.mn) * 0.5f * 1.2f, 0.5f);
	else
		cam.dist = std::max(
			glm::length(bounds.mx - bounds.mn) * 0.5f * 2.2f, 1.0f);
}

void EditorApp::DrawFrame() {
	// Frame-arena + dt live in the HOST loop (App::MainLoop resets the
	// arena + advances the clock every frame — see app.cpp).
	// Frame-time histogram hook (View menu / F12; zero cost while
	// disabled): the single record per editor frame.
	smvk::profiler::RecordFrameTime(double(delta_time_s) * 1000.0);
	Update();
	// Dev crash-repro vehicle (TOURMALINE_EDITOR_OPEN_AT=
	// "<frame>:<file>[;<frame>:<file>...]"): opens scene files MID-LOOP at
	// the given frame indices — the exact open-DIALOG timing (the open
	// completes during a frame). Multiple entries
	// drive sequences (e.g. a plain open first, then a tour open over a
	// loaded scene — the user-crash shape). The --gltf CLI path opens
	// at Initialize time instead (before any present), so driver bugs
	// that need "presents happened before the open" never fire there.
	{
		struct OpenAt {
			int frame = -1;
			std::string path;
			bool done = false;
		};
		static std::vector<OpenAt> sOpens = []() {
			std::vector<OpenAt> out;
			const char* e = std::getenv("TOURMALINE_EDITOR_OPEN_AT");
			if (!e || !*e)
				return out;
			std::string spec(e);
			size_t pos = 0;
			while (pos < spec.size()) {
				const size_t semi = spec.find(';', pos);
				const std::string one = spec.substr(pos,
					semi == std::string::npos ? std::string::npos : semi - pos);
				pos = (semi == std::string::npos) ? spec.size() : semi + 1;
				const size_t colon = one.find(':');
				if (colon == std::string::npos || colon == 0)
					continue;
				OpenAt o;
				o.frame = std::atoi(one.substr(0, colon).c_str());
				o.path = one.substr(colon + 1);
				if (o.frame >= 0 && !o.path.empty())
					out.push_back(std::move(o));
			}
			return out;
		}();
		for (OpenAt& o : sOpens) {
			if (o.done || o.frame < 0 || int(m_frameCounter) != o.frame)
				continue;
			o.done = true;
			std::cerr << "[OPEN_AT] frame " << o.frame << ": opening '"
				<< o.path << "' (mid-loop, open-dialog timing)\n";
			std::fflush(stderr);
			OpenAnySceneFile(o.path);
			std::cerr << "[OPEN_AT] open returned\n";
			std::fflush(stderr);
		}
	}
	// Dev selection pin (TOURMALINE_EDITOR_SELECT=<node>|list, one-shot at
	// the first frame a world exists): headless box-vs-mesh diagnostics
	// (see the [SELBOX] trace in RecordGizmoPass) need a selection
	// without input. "list" prints every draw item's node + global
	// basis (the rotated-frame hunt). Absent = off.
	{
		static const char* selRaw = std::getenv("TOURMALINE_EDITOR_SELECT");
		static const bool selList = selRaw
			&& std::strcmp(selRaw, "list") == 0;
		static const int selNode = selRaw ? std::atoi(selRaw) : -1;
		static bool selDone = false;
		if (!selDone && m_world && selRaw) {
			if (selList) {
				selDone = true;
				for (const auto& item : m_world->drawItems) {
					const glm::mat4& g =
						m_world->scene.globalTransform[item.node];
					std::fprintf(stderr,
						"[SELLIST] node=%u parent=%d basis "
						"x=(%.3f,%.3f,%.3f) y=(%.3f,%.3f,%.3f) "
						"z=(%.3f,%.3f,%.3f)\n",
						item.node,
						m_world->scene.hierarchy[item.node].parent,
						g[0][0], g[0][1], g[0][2],
						g[1][0], g[1][1], g[1][2],
						g[2][0], g[2][1], g[2][2]);
				}
				std::fflush(stderr);
			}
			else if (selNode >= 0
				&& selNode < int(m_world->scene.hierarchy.size())) {
				selDone = true;
				m_selected = uint32_t(selNode);
				std::cerr << "[SELECT] dev pin: node " << selNode
					<< " selected (" << m_world->scene.hierarchy.size()
					<< " nodes)\n";
				std::fflush(stderr);
			}
		}
	}
	// Dev crash-repro vehicle #2 (TOURMALINE_EDITOR_OPEN_AT2="<frame>:
	// <file>"): same as OPEN_AT but fires AFTER the deferred block, so
	// the pending live-view launch consumes one frame LATER — the exact
	// dialog shape from the user's crash log (one editor frame renders
	// with the new world between the open and the launch).
	{
		struct OpenAt2 {
			int frame = -1;
			std::string path;
			bool done = false;
		};
		static std::vector<OpenAt2> sOpens = []() {
			std::vector<OpenAt2> out;
			const char* e = std::getenv("TOURMALINE_EDITOR_OPEN_AT2");
			if (!e || !*e)
				return out;
			const std::string spec(e);
			const size_t colon = spec.find(':');
			if (colon == std::string::npos || colon == 0)
				return out;
			OpenAt2 o;
			o.frame = std::atoi(spec.substr(0, colon).c_str());
			o.path = spec.substr(colon + 1);
			if (o.frame >= 0 && !o.path.empty())
				out.push_back(std::move(o));
			return out;
		}();
		for (OpenAt2& o : sOpens) {
			if (o.done || o.frame < 0 || int(m_frameCounter) != o.frame)
				continue;
			o.done = true;
			std::cerr << "[OPEN_AT2] frame " << o.frame << ": opening '"
				<< o.path << "' (post-deferred — launch lags one frame)\n";
			std::fflush(stderr);
			OpenAnySceneFile(o.path);
			std::cerr << "[OPEN_AT2] open returned\n";
			std::fflush(stderr);
		}
	}
	// Dev crash-repro vehicle #3 (TOURMALINE_EDITOR_AUTO_QUICKBAKE=<frame>):
	// fires CookCubemapsNow() (the cubemap-only quick bake, GiBaker
	// skipped) MID-LOOP at the given frame — the exact
	// "Update cubemaps now" button timing for headless repros (pairs
	// with OPEN_AT/OPEN_AT2 to have a scene loaded first).
	{
		static int sFrame = []() {
			const char* e = std::getenv("TOURMALINE_EDITOR_AUTO_QUICKBAKE");
			return (e && *e) ? std::atoi(e) : -1;
		}();
		static bool sDone = false;
		if (!sDone && sFrame >= 0 && int(m_frameCounter) == sFrame) {
			sDone = true;
			std::cerr << "[AUTO_QUICKBAKE] frame " << sFrame
				<< ": cooking cubemaps\n";
			std::fflush(stderr);
			CookCubemapsNow();
			std::cerr << "[AUTO_QUICKBAKE] posted\n";
			std::fflush(stderr);
		}
	}
	// ---- frame sync: acquire -> editor passes -> present (the host pump
	// shape, driven editor-side for the custom frame flow). ----
	auto fb = m_swapchain->AcquireFramebuffer();
	if (fb.imageIndex >= 0)
		RenderFrame(fb);
	m_swapchain->EndAndPresentFramebuffer(fb);
	// Fibers pump (main thread): deliver job completions (cook/bake
	// finish logic runs here, after the frame is presented).
	m_fibers.Pump();
	PollCookProcess();
	PollBakeProcess();
}

void EditorApp::Log(const std::string& s) {
	m_consoleLines.push_back(s);
	if (m_consoleLines.size() > 400)
		m_consoleLines.erase(m_consoleLines.begin());
	std::cerr << "[Editor] " << s << "\n";
}

} // namespace editor
