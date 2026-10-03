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
#include <cstring>
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
#include <Engine/Asset/pak.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/GI/cubemap_volume.h>
#include <Engine/Profile/profile.h>
#include <Engine/Renderer/mesh.h>
#include <gltf_import/gltf.h>
#include <stb_image_write.h> // hosted-target diagnostic dump (PNG)
#include <Engine/core.h>

#include <imgui.h>

namespace editor {

bool EditorApp::HasPakExtension(const std::string& path) {
	if (path.size() < 4)
		return false;
	const std::string ext = path.substr(path.size() - 4);
	return ext == ".pak" || ext == ".PAK" || ext == ".Pak";
}

namespace {
// Splits "file.pak:N" into path + scene index (drive-letter colons never
// qualify: the suffix must be all digits and the base must end in .pak).
std::pair<std::string, uint32_t> SplitPakIndex(const std::string& path) {
	const size_t colon = path.find_last_of(':');
	if (colon == std::string::npos || colon + 1 >= path.size())
		return { path, 0 };
	for (size_t i = colon + 1; i < path.size(); ++i)
		if (path[i] < '0' || path[i] > '9')
			return { path, 0 };
	const std::string base = path.substr(0, colon);
	if (base.size() < 4)
		return { path, 0 };
	const std::string ext = base.substr(base.size() - 4);
	if (ext != ".pak" && ext != ".PAK")
		return { path, 0 };
	return { base, static_cast<uint32_t>(std::stoul(path.substr(colon + 1))) };
}
} // namespace

bool EditorApp::OpenScene(const std::string& path) {
	// Queued when called before Initialize (CLI path); otherwise loads now.
	// Dispatches cooked .pak vs source .gltf by extension.
	if (!m_context) {
		m_pendingGltf = path;
		return true;
	}
	const auto [pakBase, pakIndex] = SplitPakIndex(path);
	if (HasPakExtension(pakBase))
		return LoadPakIntoWorld(pakBase, pakIndex);
	if (HasPakExtension(path))
		return LoadPakIntoWorld(path);
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
	if (path.size() >= 5) {
		const std::string ext = path.substr(path.size() - 5);
		if (ext == ".tour" || ext == ".TOUR")
			return OpenTourFile(path);
	}
	return OpenScene(path);
}

bool EditorApp::OpenPak(const std::string& pakPath) {
	if (!m_context) {
		m_pendingGltf = pakPath;
		return true;
	}
	const auto [pakBase, pakIndex] = SplitPakIndex(pakPath);
	return LoadPakIntoWorld(HasPakExtension(pakBase) ? pakBase : pakPath,
		HasPakExtension(pakBase) ? pakIndex : 0);
}

EditorApp::EditorApp()
	: m_guidResolver(*this) {
}

bool EditorApp::Initialize() {
	if (!Super::Initialize())
		return false;
	// Dev quad-layout switch (VULKANTOY_EDITOR_VP_QUAD=1): start in the
	// four-viewport layout so headless smoke runs exercise the hosted
	// MULTIVIEW path (every additional viewport drives its own hosted
	// view; see the live-view drive in DrawFrame). The V toggle keeps
	// working after.
	if (const char* quad = std::getenv("VULKANTOY_EDITOR_VP_QUAD")) {
		if (std::atoi(quad) != 0) {
			m_vpLayout = 1;
			std::cerr << "[Editor] quad viewport layout (dev env) — "
				"hosted multiview engaged\n";
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
	// Without a project the cache is OFF (no scratch disk — cooks and
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
	if (!m_wysiwygSmoke && m_projectSmokeDir.empty())
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
		if (!OpenScene(p))
			std::cerr << "[Editor] failed to load '" << p << "'\n";
	}

	// Headless project flow (--project-smoke <dir> + --gltf <asset>):
	// create/open the project, import the asset (engine representation
	// ready), then COOK it explicitly (product step, full settings) and
	// run the regular wysiwyg-smoke stages over the result (the cook is
	// already running, so stage 0 just waits for it).
	if (!m_projectSmokeDir.empty() && !m_projectSmokeAsset.empty()) {
		const std::string d = m_projectSmokeDir;
		const std::string a = m_projectSmokeAsset;
		m_projectSmokeDir.clear();
		m_projectSmokeAsset.clear();
		if (!NewProjectOrOpen(d) || !ImportAsset(a)) {
			std::cerr << "[ProjectSmoke] setup failed (see log)\n";
		}
		else {
			CookToPak();
			m_wysiwygSmoke = true;
		}
	}

	Log("Tourmaline Editor ready. File > Open scene... to load a scene.");
	return true;
}

void EditorApp::Shutdown() {
	// Flush the working tour to the project .tour on exit (per-edit
	// saves went to the %APPDATA% autosave) â€” same contract as
	// CloseScene, so closing the editor never loses edits.
	if (m_world && !TourTarget().empty())
		SaveTour();
	SavePrefs(); // save editor prefs on close
	// Hosted WYSIWYG renderer FIRST: its GPU resources (pipelines,
	// targets, its own rendergraph world) release through the SHARED
	// context â€” must happen before the editor tears its own state.
	if (m_wysiwyg)
		StopWysiwyg();
	WaitForDeviceIdle();
	m_textureStreamer.reset();
	PollCookProcess();
	PollBakeProcess();
	PollPublishProcess();
	m_imgui.reset();
	m_depth = smvk::DepthTarget{};
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
			// [wysiwyg] preview key retired: the hosted view is the
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
	// artifacts/ dir (cache.db + assets.db + per-class paks). Without
	// a project the cache stays off — imports/cooks still work (the
	// cache only accelerates processing, never gates it).
	if (m_projectRoot.empty()) {
		Log("Artifact cache off — no project open (File > New / Open "
			"Project...).");
		return false;
	}
	std::string root = ProjectArtifactDir();
	root = smvk::cache::EnsureScratchRoot(root);
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
	// Editor VFS: register the scratch class paks (the cooked-resource
	// overlay the display path resolves through). ONLY tour + scratch
	// mount here â€” never the release content tree (editor -> release
	// is one-way; reading content/paks back would feed stale cooked
	// content into authoring). The scene class is consumed by the
	// HOSTED WorldRenderer (its own VFS, App --pak parity), not by
	// this overlay; these are the resource classes.
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
			std::string("scratch-") + klass, berr);
		if (!backend)
			continue; // stale/corrupt class pak: cache, disposable
		const std::string name = std::string("scratch-") + klass;
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
	// Mid-frame safe: the hosted live view dies with its scene — the
	// world moves to the retire stash (alive for the borrowed pointer)
	// and the actual stop runs at the next frame boundary.
	SuspendWysiwygForWorldSwap();
	m_world.reset();
	m_gltfPath.clear();
	m_gltfHostPath.clear();
	m_pakPath.clear();
	m_pakImage.reset();
	m_pakReader.reset();
	m_pakScenes.clear();
	m_pakSceneIndex = 0;
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
	m_pathRemaps.clear();
	m_refIssues.clear();
	m_refResolved.clear();
	m_defaultMatWorld = UINT32_MAX;
	ClearSelection();
	ClearHistory();
	m_tourDirty = false;
	Log("Scene closed.");
}

bool EditorApp::LoadGltfIntoWorld(const std::string& path,
	const std::string& tourOverride) {
	// Empty path = sourceless tour (DEPRECATED source mechanism): no
	// glTF to import â€” an empty world whose geometry comes entirely
	// from references. tourOverride pins the reference lookup to the
	// opened tour file (otherwise <stem>.tour next to the source,
	// which does not exist here).
	std::string loadPath = path;
	tinygltf::Model model;
	bool haveModel = !path.empty(); // grows: tour opens parse the bound source
	const bool directOpen = !path.empty(); // direct glTF open vs tour open
	// Texture-cache key scope for this import (GUID-namespaced when the
	// model lives in a project): identically-NAMED textures across models
	// stop thrashing the one (klass, uri) row. Nested reference imports
	// inherit this scope — consistent across sessions. Cleared at the end;
	// early returns are harmless (the next import re-scopes first).
	if (m_texCacheAdapter)
		m_texCacheAdapter->SetKeyScope(ProjectAssetGuid(loadPath));
	if (haveModel) {
		// Scheme paths keep their //flintbase/ identity in m_gltfPath
		// but load (and fingerprint) from the resolved storage file.
		if (smvk::vfs::HasFlintbasePrefix(path)) {
			loadPath = smvk::FlintbaseHostPath(path);
			if (loadPath.empty()) {
				Log("Failed to resolve " + std::string(smvk::vfs::Normalize(path)));
				return false;
			}
		}
		if (!LoadModelFromAsciiFile(model, loadPath.c_str())) {
			Log("Failed to load glTF: " + loadPath);
			return false;
		}
	}
	WaitForDeviceIdle();
	// Fresh streamer per load: entries view the old cluster's stashed
	// payload bytes, so drop them before the world (and its clusters)
	// is replaced. GPU is idle, so teardown is safe.
	m_textureStreamer.reset();
	m_texHandleBySlot.clear();
	m_bindlessBySlot.clear();
	// The world is about to be replaced: suspend the live view (the
	// old world retires at the frame boundary after the hosted stop)
	// — the pending refresh below rebinds onto the NEW world.
	SuspendWysiwygForWorldSwap();
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
		m_textureStreamer =
			smvk::mem::MakeProcess<smvk::TextureStreamer>(*m_context, streamCfg);
	}
	m_world = smvk::mem::MakeProcess<smvk::World>();
	smvk::Cluster& cluster = m_world->LoadCluster(*m_context);
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
			rootRef.guid = ProjectAssetGuid(loadPath);
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
			// — the hosted WYSIWYG view matches the App, and the F3/F5
			// views work on live worlds). Configs mirror the cooker
			// defaults (meshlets 64/124, LOD ratios 60/30/12%) and ride
			// into every nested reference (ApplySceneReferences forwards
			// them). Kill-switches for giant imports
			// (VULKANTOY_EDITOR_MESHLETS=0 / VULKANTOY_EDITOR_LODS=0).
			smvk::MeshOptConfig importMeshOpt{}; // weld/cache/overdraw/fetch on
			importMeshOpt.generateLods = true;   // sibling blobs <base>/lodN
			smvk::MeshletConfig importMeshlets{};
			importMeshlets.enabled = true;
			if (const char* e = std::getenv("VULKANTOY_EDITOR_MESHLETS")) {
				if (std::atoi(e) == 0)
					importMeshlets.enabled = false;
			}
			if (const char* e = std::getenv("VULKANTOY_EDITOR_LODS")) {
				if (std::atoi(e) == 0)
					importMeshOpt.generateLods = false;
			}
			const size_t applied = ApplySceneReferences(cluster, refDoc,
				std::filesystem::path(loadPath).parent_path()
					.generic_string(),
				m_context.get(), m_textureStreamer.get(),
				nullptr /* host files — cook-parity resolution */,
				importMeshOpt, importMeshlets, 0,
			&m_refPhCluster, &m_refIssues, &m_refResolved,
			/*autoKtx=*/true, /*fileNodes=*/nullptr,
			/*artifactCache=*/m_texCacheAdapter.get(),
			/*guidResolver=*/&m_guidResolver);
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
	// Tour-opened scenes: parse the bound source for CPU picking
	// (direct opens parsed up front; the definition resolved the
	// host path above).
	if (!haveModel && !loadPath.empty()) {
		if (LoadModelFromAsciiFile(model, loadPath.c_str()))
			haveModel = true;
	}
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
	m_vfsRoot.clear(); // re-resolved lazily by the VFS tab
	m_vfsSel.clear();
	m_gltfModel = std::move(model);
	m_hasGltfModel = haveModel; // sourceless: no model, AABB picking
	m_pakPath.clear();
	m_pakImage.reset();
	m_pakReader.reset();
	m_pakScenes.clear();
	m_pakSceneIndex = 0;
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
	m_pathRemaps.clear(); // fresh world: relinks reload from the tour
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
	Log("Indexing nodes for picking...");
	RebuildNodeIndex();
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
	ApplyPathRemaps(); // path editor relinks: fill still-missing slots
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
	// Bake-asset GI bindings for the hosted live view (fresh world —
	// the clusters carry no Gi registry yet; the hosted rebind below
	// reads them).
	RefreshLiveViewGiBindings();
	// The hosted view drives the viewport for this scene from its
	// source (full App pipeline, no cook needed): the pending suspend
	// above rebinds it IN PLACE at the next frame boundary — a fresh
	// launch only when no live view exists yet.
	return true;
}

std::string EditorApp::GuidResolverHook::PathForGuid(const std::string& guid) {
	// Closed db (no project / cache off) answers "": the reference falls
	// back to its recorded -source path — exactly the pre-GUID behavior.
	std::string path;
	if (!app.m_assetDb.IsOpen() || !app.m_assetDb.PathForGuid(guid, path))
		return {};
	return path;
}

bool EditorApp::LoadPakIntoWorld(const std::string& path,
	uint32_t sceneIndex) {
	// Host-file pak open (manual inspection). The bytes are read ONCE
	// into a shared image; the display core below never touches the
	// file again.
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		Log("Failed to open pak: " + path);
		return false;
	}
	const auto end = f.tellg();
	if (end == std::ifstream::pos_type(-1) || end == std::streampos(0)) {
		Log("Pak is empty or unreadable: " + path);
		return false;
	}
	f.seekg(0);
	auto image = std::make_shared<std::vector<uint8_t>>(
		static_cast<size_t>(end), 0);
	if (!f.read(reinterpret_cast<char*>(image->data()),
			static_cast<std::streamsize>(image->size()))) {
		Log("Pak read failed: " + path);
		return false;
	}
	return LoadPakFromImage(std::move(image), path, sceneIndex);
}

bool EditorApp::LoadPakFromImage(
	std::shared_ptr<std::vector<uint8_t>> image,
	const std::string& displayPath, uint32_t sceneIndex) {
	// Cooked .pak display core: same path as App's main.cpp host
	// (PakReader + LoadClusterFromPak + Resolve; a SCENE renders, index
	// 0 by default — --pak parity). Pak textures upload fully at load,
	// so the glTF texture streamer is dropped. Picking falls back to
	// AABB (no glTF model kept); tour replay is skipped (tours are
	// authored against the source glTF, not the cooked pak). The image
	// + parsed reader stay members (never re-parsed per frame: Open
	// decompresses).
	m_pakImage = std::move(image);
	m_pakReader.reset();
	m_pakScenes.clear();
	{
		std::string pakErr;
		m_pakReader = smvk::PakReader::Open(
			std::span<const uint8_t>(*m_pakImage), pakErr);
		if (!m_pakReader) {
			Log("Pak parse failed (" + pakErr + ")");
			m_pakImage.reset();
			return false;
		}
	}
	const size_t entryCount = m_pakReader->EntryCount();
	{
		std::string lerr;
		m_pakScenes = smvk::ListPakScenes(*m_pakReader, &lerr);
		if (m_pakScenes.empty()) {
			Log("Pak has no loadable scenes" +
				std::string(lerr.empty() ? "" : (": " + lerr)));
			m_pakImage.reset();
			m_pakReader.reset();
			return false;
		}
		if (sceneIndex >= m_pakScenes.size()) {
			Log("Pak scene index " + std::to_string(sceneIndex) +
				" out of range (" + std::to_string(m_pakScenes.size()) +
				" scenes)");
			m_pakImage.reset();
			m_pakReader.reset();
			m_pakScenes.clear();
			return false;
		}
		for (size_t i = 0; i < m_pakScenes.size(); ++i)
			Log("Pak scene[" + std::to_string(i) + "] '" +
				m_pakScenes[i].name + "' nodes=" +
				std::to_string(m_pakScenes[i].nodes));
	}
	m_pakSceneIndex = sceneIndex;
	Log("Pak parsed: " + std::to_string(entryCount) + " entries, loading "
		"scene[" + std::to_string(sceneIndex) + "] '" +
		m_pakScenes[sceneIndex].name + "'...");
	WaitForDeviceIdle();
	m_textureStreamer.reset(); // pak path owns its textures; no streaming
	m_texHandleBySlot.clear();
	m_bindlessBySlot.clear();
	// World replacement incoming: suspend the live view (the old
	// world retires safely at the frame boundary; the pending refresh
	// rebinds onto this pak's world).
	SuspendWysiwygForWorldSwap();
	m_world = smvk::mem::MakeProcess<smvk::World>();
	try {
		m_world->LoadClusterFromPak(*m_context, *m_pakReader, sceneIndex);
	}
	catch (const std::exception& e) {
		Log(std::string("Pak load failed: ") + e.what());
		m_world.reset();
		return false;
	}
	m_context->FlushUploads();
	Log("Resolving world...");
	m_world->ResolveAndFixup_clusters();
	// Slot maps for the viewport upload remap (pak textures are
	// full-resident; no streamer, but the pool->bindless check still
	// applies and is logged here).
	RebuildTexHandleMap();

	m_pakPath = displayPath;
	// Keep the scene identity pointed at the pak so TourPath()/cook panel
	// stay coherent, but mark it as cooked: no glTF picking, no tour.
	m_gltfPath = displayPath;
	m_gltfHostPath = displayPath;
	m_tourPath = TourPath();
	m_vfsRoot.clear();
	m_vfsSel.clear();
	m_hasGltfModel = false;
	m_gltfModel = tinygltf::Model{};
	m_primRef.clear();
	ClearTexPreview();
	m_selected = UINT32_MAX;
	ClearHistory();
	m_terrainGroups.clear(); // fresh world: stale group grids die with it
	m_deletedNodes.clear(); // fresh world: deletes die with it
	m_images.clear(); // fresh world: image defs die with it
	m_nextImageId = 1;
	m_pathRemaps.clear(); // pak scenes carry no glTF image list
	m_refIssues.clear();
	m_refResolved.clear(); // fresh world: resolved sources die with it
	m_defaultMatWorld = UINT32_MAX;
	m_overrideRules.clear(); // pak scenes are read-only: no wide rules
	m_nextOverrideId = 1;
	// Pak loads replay no tour: the scene keeps its cooked/default look.
	m_envCurrent = UINT32_MAX;
	m_materialFocus = UINT32_MAX;
	m_materialTrail.clear();
	m_imageFocus = UINT32_MAX;
	ClearSelection();
	ResetMeshletDebug();
	m_clusterBase = UINT32_MAX;
	if (m_world->clusters.size() == 1 &&
		m_world->scene.hierarchy.size() ==
		1 + m_world->clusters[0]->scene.hierarchy.size())
		m_clusterBase = 1;
	RebuildNodeIndex(); // no-op without a glTF model (clears stale refs)
	ClearSglmLayers(); // dropped before the sibling-atlas probe below

	// Baked lightmap atlas next to the pak: content/gi/<stem>.sglm.
	TryLoadSiblingSglm(displayPath);
	// Convex GI volume sidecars (probes-only pivot preview data).
	TryLoadCvolumes(displayPath);
	RebuildCookedLights(); // census + seeds the viewport sun from the pak
	EnsureDefaultMaterial(); // editor fallback material (editor://default)

	const ::Aabb bounds = m_world->ComputeSceneBounds();
	if (bounds.mx.x >= bounds.mn.x) {
		const glm::vec3 center = (bounds.mn + bounds.mx) * 0.5f;
		const float radius = glm::length(bounds.mx - bounds.mn) * 0.5f;
		SetupDefaultCameras();
		EditorCam& fitCam = m_editorCams[m_viewports[0].camIdx];
		fitCam.target = center;
		fitCam.dist = std::max(radius * 2.2f, 1.0f);
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf),
		"Loaded pak '%s' scene[%u] (entries=%zu nodes=%zu meshes=%zu materials=%zu)%s",
		displayPath.c_str(), sceneIndex, entryCount,
		m_world->scene.hierarchy.size(),
		m_world->meshLocations.size(), m_world->materialLocations.size(),
		m_hasSglm ? " + sglm atlas" : " (no sglm atlas)");
	Log(buf);
	Log("Note: pak is read-only in the editor (edits/tour apply to the source glTF).");
	// The hosted view drives the viewport for this pak too (App
	// --pak parity inspection): the pending suspend above rebinds it
	// in place at the next frame boundary.
	return true;
}

bool EditorApp::LaunchWysiwyg() {
	// LIVE VIEW: the hosted renderer renders THE EDITOR'S OWN WORLD —
	// the live engine representation (imports preprocessed, GUID
	// references bound, edits applied). The world is BORROWED
	// (SetHostedBorrowedWorld): the editor keeps owning + mutating it,
	// edits show up per frame; the hosted renderer never imports
	// anything. Cooks are the separate PRODUCT step — they never feed
	// the viewport.
	if (!m_world) {
		Log("Live view: no world to host — open a scene first.");
		return false;
	}
	// Frame-boundary contract: creating the hosted swapchain and
	// (re)binding descriptors while editor frames are in flight
	// faults. Callers run between frames; idle anyway so a direct
	// call can never wedge the shared context.
	WaitForDeviceIdle();
	StopWysiwyg(); // fresh instance per launch (world + extent change)
	// Viewport-sized target: matches the common case 1:1; rect drift is
	// handled by the DrawFrame extent watcher (stable drift relaunches
	// at the new extent — see m_wysiwygResizeWant).
	const ViewportRect& vp = m_viewports[0].rect;
	uint32_t w = std::max(uint32_t(vp.w), 64u);
	uint32_t h = std::max(uint32_t(vp.h), 64u);
	m_wysiwygExtent = smvk::Extent2D{ w, h };
	// Scene id = the open scene's stem (SceneIdForHost; GI sidecar names
	// resolve gi/<stem>.* through the hosted content VFS: project root
	// first, bundled content under). The string lives in a member; the
	// renderer keeps its own copy.
	m_wysiwygSceneId = SceneIdForHost();
	if (m_wysiwygSceneId.empty())
		m_wysiwygSceneId = "scene";
	// One call: boot off this context, borrow the editor's world (it
	// keeps owning + mutating it — edits show up per frame; the hosted
	// renderer never imports anything), full framegraph + hosted
	// invariants, initialized. Cooks are the separate PRODUCT step —
	// they never feed the viewport.
	try {
		m_wysiwyg = smvk::mem::MakeProcess<smvk::WorldRenderer>(*this, *m_world,
			m_wysiwygExtent, m_wysiwygSceneId, m_projectRoot);
	}
	catch (const std::exception& e) {
		Log(std::string("Live view: launch threw: ") + e.what());
		StopWysiwyg();
		return false;
	}
	// Compose plumbing: bindless slot over the hosted target + the
	// compose program.
	m_wysiwygBindless = m_context->CreateBindlessImage(
		m_wysiwyg->HostedTargetView(),
		smvk::ImageLayout::ShaderReadOnlyOptimal);
	if (m_wysiwygBindless.empty()) {
		Log("Live view: hosted target bindless registration failed.");
		StopWysiwyg();
		return false;
	}
	m_wysiwygBindlessIndex = m_context->GetBindlessIndex(m_wysiwygBindless);
	m_wysiwygSamplerIndex = m_context->GetOrCreateSampler(
		smvk::SamplerCreateInfo{
			.magFilter = smvk::Filter::Linear,
			.minFilter = smvk::Filter::Linear,
			.mipmapMode = smvk::MipmapMode::Linear,
			.addressU = smvk::AddressMode::ClampToEdge,
			.addressV = smvk::AddressMode::ClampToEdge,
			.addressW = smvk::AddressMode::ClampToEdge,
			.maxAnisotropy = 1.0f });
	if (!EnsureWysiwygProgram()) {
		Log("Live view: compose shaders unavailable "
			"(engine_resources/shaders/wysiwyg.*.spv missing?).");
		StopWysiwyg();
		return false;
	}
	Log("Live view: hosted WorldRenderer rendering the editor's world "
		"'" + m_wysiwygSceneId + "' (full App pipeline, live; edits "
		"show per frame - world swaps rebind in place).");
	return true;
}

void EditorApp::StopWysiwyg() {
	if (!m_wysiwyg)
		return;
	// Retire in-flight editor frames first: their compose passes still
	// reference the bindless slot + hosted target being torn down here.
	WaitForDeviceIdle();
	// Release the hosted instance's GPU resources while the shared
	// context is alive (its Shutdown releases Holders; ~App then
	// RELEASES the borrowed context AND the borrowed world — the
	// editor keeps ownership of both).
	try {
		m_wysiwyg->HostedShutdown();
	}
	catch (const std::exception& e) {
		Log(std::string("Live view: shutdown threw: ") + e.what());
	}
	m_wysiwyg.reset();
	m_wysiwygBindless = {};
	m_wysiwygBindlessIndex = UINT32_MAX;
	Log("Live view: hosted renderer stopped.");
}

void EditorApp::SuspendWysiwygForWorldSwap() {
	// Mid-frame world-swap safety (scene opens/closes fire inside
	// RenderFrame): the OLD world moves to the retire stash — it stays
	// ALIVE for the hosted renderer's borrowed pointer — and the actual
	// swap is deferred to the frame boundary (rebinding GPU-side state
	// mid-frame faults on the already-recorded compose pass). Flags the
	// pending swap REGARDLESS of the live view's state: DrawFrame's
	// consumption rebinds an existing hosted renderer IN PLACE, LAUNCHES
	// the first one when no live view exists yet, and STOPS it when the
	// scene closed — then retires the stash after.
	m_wysiwygSuspendPending = true;
	m_worldRetire = std::move(m_world);
}

std::string EditorApp::SceneIdForHost() const {
	// Hosted-view scene id (the open scene's stem; the pak stem wins for
	// pak opens — the borrowed world IS the scene). Shared by the launch
	// and the in-place rebind (the GI sidecar loaders resolve
	// gi/<id>.* through the project-aware VFS).
	namespace fs = std::filesystem;
	if (!m_pakPath.empty())
		return fs::path(m_pakPath).stem().string();
	if (!m_gltfHostPath.empty())
		return fs::path(m_gltfHostPath).stem().string();
	if (!m_gltfPath.empty())
		return fs::path(m_gltfPath).stem().string();
	return std::string("scene");
}

bool EditorApp::TryLoadSiblingSglm(const std::string& pakPath) {
	m_hasSglm = false;
	m_sglm = smvk::gi::SgLightmapAtlas{};
	m_sglmPath.clear();
	// Candidate stems: the pak registry's lightmap binding first, then
	// the pak file stem (registry-less paks / sidecar convention).
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
	namespace fs = std::filesystem;
	const fs::path pak(pakPath);
	const std::string stem = pak.stem().string();
	if (!stem.empty() &&
		std::find(stems.begin(), stems.end(), stem) == stems.end())
		stems.push_back(stem);
	std::vector<std::string> candidates;
	for (const std::string& s : stems) {
		candidates.push_back(
			(pak.parent_path() / ".." / "gi" / (s + ".sglm")).lexically_normal().string());
		candidates.push_back(
			(fs::path("content") / "gi" / (s + ".sglm")).string());
		candidates.push_back(
			(pak.parent_path() / (s + ".sglm")).string());
	}
	for (const std::string& c : candidates) {
		std::error_code ec;
		if (!fs::is_regular_file(c, ec))
			continue;
		if (LoadSglm(c))
			return true;
	}
	m_sglmStatus = "no sibling atlas found for '" + pak.stem().string() +
		"' (tried " + std::to_string(candidates.size()) + " paths)";
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
// gi/<stem>.cvmanifest + .cvol sidecars next to the opened pak and
// builds each volume's hull: the bake AABB (gridMin..gridMin + dims*
// spacing) clipped by the volume's plane set (3D Sutherland-Hodgman).
// The hull feeds the LIGHTING-mode view option (wireframe / translucent
// fill, color-coded by priority) â€” draw lives in DrawCvolPreviewOverlay
// (editor_viewports.cpp).
// ---------------------------------------------------------------------------
bool EditorApp::TryLoadCvolumes(const std::string& pakPath) {
	m_cvolPreviews.clear();
	namespace fs = std::filesystem;
	const fs::path pak(pakPath);
	const std::string stem = pak.stem().string();
	if (stem.empty())
		return false;
	// Manifest candidates (same sibling convention as the sglm probe).
	std::vector<fs::path> manifests;
	manifests.push_back(
		(pak.parent_path() / ".." / "gi" / (stem + ".cvmanifest"))
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
	// One .cvol vfs path per line ('#' comments skipped). Lines are
	// relative to the CONTENT ROOT (the manifest's parent's parent).
	std::ifstream in(manifestPath);
	if (!in)
		return false;
	const fs::path contentRoot = manifestPath.parent_path().parent_path();
	std::string line;
	int loaded = 0;
	while (std::getline(in, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		fs::path volPath = fs::path(line);
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
	// Pak scenes are full-resident with no streamer: nothing to switch.
	if (!m_hasGltfModel || m_gltfPath.empty() || HasPakExtension(m_gltfPath)) {
		Log("Texture mode: only glTF scenes can switch (pak is full-resident).");
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

void EditorApp::RebuildNodeIndex() {
	m_primRef.clear();
	m_clusterRootWorld = UINT32_MAX;
	if (!m_hasGltfModel || !m_world)
		return;
	// The importer creates, per glTF node with a mesh, one prim child node
	// per primitive in order (see importNodeRecursive): group nodes carry the
	// glTF node name, prim children are unnamed identity nodes. Re-walk the
	// glTF node tree in the same preorder to recover (mesh, prim) refs.
	struct Frame { int gltf; uint32_t world; };
	// Map group node name+child-index back: simpler robust approach â€” walk
	// the WORLD tree: a node with a mesh whose parent has the same mesh
	// count pattern... Instead do a parallel recursion mirroring the
	// importer: world cluster root (node 0) children == glTF roots.
	// The importer attaches glTF roots under the cluster root via
	// importNodeRecursive(gltf, parent=clusterRoot?) â€” actually under the
	// world-resolved tree the cluster root child order matches glTF root
	// order. Prim children of each group node match primitive order.
	// Walk: for each world node that has a mesh, its parent group maps to a
	// glTF node; we resolve by matching names in preorder.
	std::vector<int> gltfPreorder;
	std::function<void(int)> walk = [&](int n) {
		gltfPreorder.push_back(n);
		for (int c : m_gltfModel.nodes[n].children)
			walk(c);
	};
	// glTF roots: default scene or unreferenced nodes (mirror GltfToCluster).
	std::vector<int> roots;
	if (m_gltfModel.defaultScene >= 0 &&
		m_gltfModel.defaultScene < (int)m_gltfModel.scenes.size())
		roots = m_gltfModel.scenes[m_gltfModel.defaultScene].nodes;
	else if (!m_gltfModel.scenes.empty())
		roots = m_gltfModel.scenes.front().nodes;
	else {
		std::vector<bool> ref(m_gltfModel.nodes.size(), false);
		for (const auto& n : m_gltfModel.nodes)
			for (int c : n.children)
				if (c >= 0 && c < (int)m_gltfModel.nodes.size())
					ref[c] = true;
		for (int i = 0; i < (int)m_gltfModel.nodes.size(); ++i)
			if (!ref[i])
				roots.push_back(i);
	}
	for (int r : roots)
		walk(r);

	// World group nodes in preorder (skip world root 0 + cluster root).
	// Reference-subtree nodes are SKIPPED: they come from other sources
	// (no master-gltf counterpart), so the preorder pairing stays exact.
	std::vector<uint32_t> refSubtree;
	for (uint32_t ph : m_refPlaceholders) {
		std::vector<uint32_t> stack{ ph };
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			if (n >= m_world->scene.hierarchy.size())
				continue;
			if (std::find(refSubtree.begin(), refSubtree.end(), n) !=
				refSubtree.end())
				continue;
			refSubtree.push_back(n);
			for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
				c = m_world->scene.hierarchy[c].nextSibling)
				stack.push_back(uint32_t(c));
		}
	}
	auto inRefSubtree = [&](uint32_t n) {
		return std::find(refSubtree.begin(), refSubtree.end(), n) !=
			refSubtree.end();
	};
	std::vector<uint32_t> worldGroups;
	std::function<void(uint32_t)> wwalk = [&](uint32_t n) {
		if (inRefSubtree(n))
			return; // referenced scene: keep the pairing master-only
		worldGroups.push_back(n);
		for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling)
			wwalk((uint32_t)c);
	};
	// Find the cluster-root child of world root (first child).
	if (!m_world->scene.hierarchy.empty()) {
		for (int c = m_world->scene.hierarchy[0].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling) {
			if (m_clusterRootWorld == UINT32_MAX)
				m_clusterRootWorld = uint32_t(c);
			wwalk((uint32_t)c);
		}
	}
	// Pair group nodes with glTF nodes by name in preorder (names may repeat;
	// order is what the importer guarantees).
	size_t gi = 0;
	for (uint32_t wn : worldGroups) {
		if (gi >= gltfPreorder.size())
			break;
		// Prim nodes carry meshes (skipped here â€” paired below via their
		// group). The artificial cluster root is unnamed â€” skip it too so
		// group nodes stay 1:1 with glTF nodes in preorder.
		if (m_world->scene.meshForNode.contains(wn))
			continue;
		if (!m_world->scene.nameForNode.contains(wn))
			continue;
		int gn = gltfPreorder[gi++];
		const tinygltf::Node& gnode = m_gltfModel.nodes[gn];
		if (gnode.mesh < 0)
			continue;
		// Prim children of this group, in sibling order, map to primitives.
		int primIdx = 0;
		for (int c = m_world->scene.hierarchy[wn].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling) {
			if (!m_world->scene.meshForNode.contains((uint32_t)c))
				continue;
			m_primRef[(uint32_t)c] = PrimRef{ gnode.mesh, primIdx++ };
		}
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
	UpdateDeltaTime();
	// Frame-time histogram hook (View menu / F12; zero cost while
	// disabled — the hosted renderer's own hook is gated off when
	// hosted so this is the single record per editor frame).
	smvk::profiler::RecordFrameTime(double(delta_time_s) * 1000.0);
	Update();
	// Deferred live-view world swaps, consumed HERE between frames and
	// idle — never inside the editor's open frame: the OLD world stays
	// alive in the retire stash for the in-flight frames, then
	//  - a NEW world: IN-PLACE rebind (the hosted renderer keeps every
	//    GPU resource; only the world-derived state rebuilds) — or the
	//    first launch when no live view exists yet;
	//  - no world (scene closed): the live view dies with its scene.
	if (m_wysiwygSuspendPending) {
		m_wysiwygSuspendPending = false;
		if (m_world) {
			if (m_wysiwyg) {
				// In-place rebind onto the newly opened world (scene
				// open / reimport / pak switch): scene id + project root
				// first (the GI sidecar loaders resolve gi/<id>.* through
				// the project-aware VFS), then the swap + rebuild. The old
				// world retires right after (its in-flight frames are done
				// — the rebind waited for device idle).
				m_wysiwyg->SetSceneId(SceneIdForHost());
				m_wysiwyg->SetProjectRoot(m_projectRoot);
				m_wysiwyg->RebindWorld(m_world.get());
				Log("Live view: rebound in place onto the new world "
					"(framegraph kept).");
			}
			else {
				// First open (no live view yet): launch — viewport rects
				// first so the target sizes off viewport 0 instead of a
				// degenerate 64x64 (RenderFrame recomputes them anyway —
				// pure math, safe to run here).
				{
					const auto vkExt = m_swapchain->GetExtent();
					ComputeViewportRects(smvk::Extent2D{ vkExt.width,
						vkExt.height });
				}
				LaunchWysiwyg();
			}
			m_worldRetire.reset();
		}
		else {
			StopWysiwyg(); // scene closed: the live view dies with it
			m_worldRetire.reset();
		}
	}
	// HOSTED EXTENT DRIFT (aspect fix): the hosted target extent is a
	// launch-time snapshot — the compose blit stretches on rect drift
	// (layout switch, window resize, splitter drag). "Fresh instance
	// per launch (world + extent change)" is the designed answer, wired
	// here: relaunch on a STABLE drift only (3 identical wanted extents)
	// so transient resize flicker never churns and a live splitter drag
	// stretches as today until it settles (then one relaunch, 1:1 again).
	// Rects are last frame's here (RenderFrame recomputes them after);
	// the debounce rides that 1-frame lag fine.
	if (m_wysiwyg && m_world) {
		const ViewportRect& vp0 = m_viewports[0].rect;
		const smvk::Extent2D want{
			std::max(uint32_t(vp0.w), 64u),
			std::max(uint32_t(vp0.h), 64u) };
		if (want.width == m_wysiwygExtent.width
			&& want.height == m_wysiwygExtent.height) {
			m_wysiwygResizeWant = { 0, 0 };
			m_wysiwygResizeStable = 0;
		}
		else {
			if (want.width == m_wysiwygResizeWant.width
				&& want.height == m_wysiwygResizeWant.height)
				++m_wysiwygResizeStable;
			else {
				m_wysiwygResizeWant = want;
				m_wysiwygResizeStable = 1;
			}
			if (m_wysiwygResizeStable >= 3) {
				const smvk::Extent2D old = m_wysiwygExtent;
				// Relaunch (StopWysiwyg + fresh instance at the new
				// extent — the world is borrowed again, edits keep
				// flowing; refresh schedules reset to defaults).
				if (LaunchWysiwyg())
					Log("Live view: extent drift "
						+ std::to_string(old.width) + "x"
						+ std::to_string(old.height) + " -> "
						+ std::to_string(want.width) + "x"
						+ std::to_string(want.height)
						+ " (relaunched; compose 1:1 again).");
				m_wysiwygResizeWant = { 0, 0 };
				m_wysiwygResizeStable = 0;
			}
		}
	}
	// HOSTED WYSIWYG drive: run the WorldRenderer frame between our own
	// frames, wrapped in device-idle boundaries. Both renderers share
	// ONE Vulkan context (volk globals forbid a second instance), so
	// their per-frame engine-global slots must never be in flight
	// together: idle -> hosted submit -> idle -> the editor's own frame.
	if (m_wysiwyg) {
		const bool trace = std::getenv("VULKANTOY_HOSTED_TRACE") != nullptr;
		if (trace) { std::fprintf(stderr, "[DRIVE] idle-pre\n"); std::fflush(stderr); }
		WaitForDeviceIdle();
		// CAMERA DRIVE: viewport 0's camera drives the hosted MAIN view —
		// the compose blits it into panel 0, so the panel's camera chip and
		// its content must agree (the ACTIVE camera drove it before:
		// hovering any other panel desynced panel 0 from its own camera
		// while that camera rendered nowhere). Each additional viewport
		// keeps driving its OWN hosted view below (viewport i+1 -> view i)
		// with its own camera. Top/Side ortho cameras drive a TRUE
		// orthographic lens (orthoHalfH; the old fov-approximation
		// dolly-distorted the live view). Locked look-through (bound scene
		// cameras) stays an editor-pass feature for now.
		{
			const EditorCam& cam = m_editorCams[m_viewports[0].camIdx];
			m_wysiwyg->DriveExternalCamera(EditorCamEye(cam), cam.target,
				cam.fovYDeg, cam.nearClip, cam.farClip,
				(cam.type == EditorCam::Type::Orthographic)
					? std::max(cam.orthoHalfH, 0.01f) : 0.0f);
		}
		// GRID DRIVE: the hosted renderer draws the construction grid
		// itself (depth-tested against each view's own depth — the host
		// has no depth to test against, which is why the overlay grid
		// drew over everything). Same settings as the editor's grid.
		m_wysiwyg->DriveGrid(m_showGrid, m_gridSize, m_gridExtent);
		// MULTIVIEW drive: every ADDITIONAL editor viewport drives its
		// own hosted view (viewport i+1 -> renderer view i): the view
		// count follows the viewport layout (SetViewCount no-ops while
		// unchanged; a layout change recreates the per-view targets
		// here at the idle boundary), extents track the viewport rects
		// and each viewport's OWN camera renders its view — ortho
		// cameras with a TRUE orthographic lens (same as the main
		// drive). The views carry per-view tic/toc refresh schedules
		// (see the live-view panel); off-phase views keep their last
		// image and compose anyway.
		{
			const int extraViewports = ViewportCount() - 1;
			const uint32_t viewCount = (uint32_t)std::clamp(
				extraViewports, 0, (int)smvk::WorldRenderer::kMaxSecondaryViews);
			m_wysiwyg->SetViewCount(viewCount);
			for (uint32_t i = 0; i < viewCount; ++i) {
				const EditorViewport& vp = m_viewports[i + 1];
				m_wysiwyg->SetViewEnabled(i, true);
				m_wysiwyg->SetViewExtent(i, smvk::Extent2D{
					std::max(uint32_t(vp.rect.w), 64u),
					std::max(uint32_t(vp.rect.h), 64u) });
				const EditorCam& cam = m_editorCams[vp.camIdx];
				m_wysiwyg->DriveView(i, EditorCamEye(cam), cam.target,
					cam.fovYDeg, cam.nearClip, cam.farClip,
					(cam.type == EditorCam::Type::Orthographic)
						? std::max(cam.orthoHalfH, 0.01f) : 0.0f);
			}
		}
		// DEBUG-VIEW DRIVE: the editor's view modes hook the hosted
		// frame with APP PARITY (the F-keys match the standalone App's
		// DebugView shortcuts 1:1). Mapped 1:1 where the frame has the
		// view; the editor-only flat/baked views map onto their closest
		// frame views (flat unwraps -> the UV checkers, baked -> flat
		// irradiance).
		{
			int giDebug = 0;
			smvk::DebugView dv = smvk::DebugView::Lit;
			switch (ActiveViewMode()) {
			case ViewMode::Unlit: dv = smvk::DebugView::Unlit; break;
			case ViewMode::Lit: dv = smvk::DebugView::Lit; break;
			case ViewMode::Uv0:
			case ViewMode::FlatUv0: dv = smvk::DebugView::Uv0; break;
			case ViewMode::Uv1:
			case ViewMode::FlatUv1: dv = smvk::DebugView::Uv1; break;
			case ViewMode::VertexColors:
				dv = smvk::DebugView::VertexColors; break;
			case ViewMode::Meshlets: dv = smvk::DebugView::Meshlets; break;
			case ViewMode::Cascades: dv = smvk::DebugView::Cascades; break;
			case ViewMode::Lods: dv = smvk::DebugView::Lods; break;
			case ViewMode::Pom: dv = smvk::DebugView::Pom; break;
			case ViewMode::Baked:
				dv = smvk::DebugView::Lit;
				giDebug = 1; // flat baked irradiance
				break;
			}
			m_wysiwyg->SetDebugView(dv);
			m_wysiwyg->SetGiDebug(giDebug);
		}
		// WIREFRAME DRIVE (P / Z — App parity): the frame's wireframe
		// is Line polygon mode over the Fill depth prepass, so hidden
		// lines are removed (lines never draw over the objects in
		// front — the prepass depth occludes them).
		m_wysiwyg->SetWireframe(m_wireframeAll);
		if (trace) { std::fprintf(stderr, "[DRIVE] hosted frame\n"); std::fflush(stderr); }
		m_wysiwyg->HostedDrawFrame();
		if (trace) { std::fprintf(stderr, "[DRIVE] idle-post\n"); std::fflush(stderr); }
		WaitForDeviceIdle();
		if (trace) { std::fprintf(stderr, "[DRIVE] done\n"); std::fflush(stderr); }
		// Diagnostic dump (VULKANTOY_WYSIWYG_DUMP=<frame>): read the
		// hosted target back at the device-idle boundary (its layout
		// is SHADER_READ here) and write a PNG + pixel stats —
		// "is the hosted renderer actually drawing the mesh" becomes
		// answerable headlessly. Two shots (frame N and N+11) so
		// before/after comparisons (camera moves, view switches) come
		// out of ONE run. Ships in all builds (env-gated dev tool).
		static const int kDumpFrame = []() {
			const char* e = std::getenv("VULKANTOY_WYSIWYG_DUMP");
			return e ? std::atoi(e) : -1;
		}();
		static int dumpShots = 0;
		const int dumpAt = (dumpShots == 0) ? kDumpFrame
			: (dumpShots == 1 ? kDumpFrame + 11 : -1);
		if (kDumpFrame >= 0 && dumpAt >= 0
			&& int(m_frameCounter) == dumpAt
			&& m_wysiwygExtent.width > 0 && m_wysiwygExtent.height > 0) {
			++dumpShots;
			const size_t bytes = size_t(m_wysiwygExtent.width)
				* m_wysiwygExtent.height * 4;
			std::vector<uint8_t> px(bytes);
			if (m_context->DownloadDataFromImage(
					m_wysiwyg->HostedTargetImage(),
					smvk::ImageLayout::ShaderReadOnlyOptimal,
					px.data(), px.size())) {
				// BGRA -> RGBA for stb + stats.
				uint64_t sum[3] = { 0, 0, 0 };
				size_t nonBlack = 0;
				for (size_t i = 0; i + 3 < px.size(); i += 4) {
					std::swap(px[i], px[i + 2]); // B,R
					sum[0] += px[i]; sum[1] += px[i + 1];
					sum[2] += px[i + 2];
					if (px[i] > 8 || px[i + 1] > 8 || px[i + 2] > 8)
						++nonBlack;
				}
				const size_t n = px.size() / 4;
				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"hosted-target dump #%d: %ux%u mean=(%u,%u,%u) "
					"non-black %.1f%% -> wysiwyg_hosted.png",
					dumpShots,
					m_wysiwygExtent.width, m_wysiwygExtent.height,
					unsigned(sum[0] / n), unsigned(sum[1] / n),
					unsigned(sum[2] / n),
					100.0 * double(nonBlack) / double(n));
				Log(buf);
				stbi_write_png(
					dumpShots == 1 ? "wysiwyg_hosted.png"
						: "wysiwyg_hosted2.png",
					int(m_wysiwygExtent.width),
					int(m_wysiwygExtent.height), 4, px.data(),
					int(m_wysiwygExtent.width) * 4);
			}
			else {
				Log("hosted-target dump: readback failed.");
			}
			// MULTIVIEW: dump each hosted view's own image at the same
			// shots (wysiwyg_viewN.png + stats + the view's refresh state)
			// — per-view content and the tic/toc phase split become
			// answerable headlessly.
			for (uint32_t i = 0; i < m_wysiwyg->GetViewCount(); ++i) {
				const smvk::Extent2D ext = m_wysiwyg->ViewOutExtent(i);
				const smvk::ImageHandle img = m_wysiwyg->ViewOutImage(i);
				if (ext.width == 0 || ext.height == 0 || img.empty())
					continue;
				const size_t vbytes = size_t(ext.width) * ext.height * 4;
				std::vector<uint8_t> vpx(vbytes);
				if (!m_context->DownloadDataFromImage(
						img, smvk::ImageLayout::ShaderReadOnlyOptimal,
						vpx.data(), vpx.size())) {
					Log("hosted-view dump: readback failed.");
					continue;
				}
				uint64_t vsum[3] = { 0, 0, 0 };
				size_t vNonBlack = 0;
				for (size_t p = 0; p + 3 < vpx.size(); p += 4) {
					std::swap(vpx[p], vpx[p + 2]); // B,R
					vsum[0] += vpx[p]; vsum[1] += vpx[p + 1];
					vsum[2] += vpx[p + 2];
					if (vpx[p] > 8 || vpx[p + 1] > 8 || vpx[p + 2] > 8)
						++vNonBlack;
				}
				const size_t vn = vpx.size() / 4;
				char vbuf[256];
				std::snprintf(vbuf, sizeof(vbuf),
					"hosted-view %u dump #%d: %ux%u mean=(%u,%u,%u) "
					"non-black %.1f%% refresh=%s%s -> wysiwyg_view%u.png",
					i + 1u, dumpShots,
					ext.width, ext.height,
					unsigned(vsum[0] / vn), unsigned(vsum[1] / vn),
					unsigned(vsum[2] / vn),
					100.0 * double(vNonBlack) / double(vn),
					smvk::ViewRefreshName(m_wysiwyg->GetViewRefresh(i)),
					m_wysiwyg->ViewRenderedThisFrame(i)
						? " (rendered this frame)" : " (held image)",
					i + 1u);
				Log(vbuf);
				char png[64];
				std::snprintf(png, sizeof(png), "wysiwyg_view%u%s.png",
					i + 1u, dumpShots == 1 ? "" : "b");
				stbi_write_png(png, int(ext.width), int(ext.height), 4,
					vpx.data(), int(ext.width) * 4);
			}
			// Lit restore for the SECOND shot (smoke runs only): shot 1
			// stays whatever mode was active (the Unlit+wire reference
			// in the smoke flow — the control), shot 2 renders Lit with
			// the scene state as of shot 1 + 11 frames (the swung scene
			// sun in the smoke flow — the scene-env proof). Firing here
			// (right after shot 1 is captured) keeps the two shots
			// deterministically apart regardless of stage timing; manual
			// dump runs never touch the view mode.
			if (dumpShots == 1 && m_wysiwygSmoke) {
				ActiveViewMode() = ViewMode::Lit;
				m_wireframeAll = false;
				std::cerr << "[SMOKE] editor view mode -> Lit, wireframe "
					"off — the live view must follow\n";
			}
		}
	}
	if (std::getenv("VULKANTOY_HOSTED_TRACE") != nullptr) {
		std::fprintf(stderr, "[DRIVE] ed-acquire\n"); std::fflush(stderr);
	}
	auto fb = m_swapchain->AcquireFramebuffer();
	if (std::getenv("VULKANTOY_HOSTED_TRACE") != nullptr) {
		std::fprintf(stderr, "[DRIVE] ed-render begin img=%d\n", (int)fb.imageIndex); std::fflush(stderr);
	}
	if (fb.imageIndex >= 0)
		RenderFrame(fb);
	if (std::getenv("VULKANTOY_HOSTED_TRACE") != nullptr) {
		std::fprintf(stderr, "[DRIVE] ed-present begin\n"); std::fflush(stderr);
	}
	m_swapchain->EndAndPresentFramebuffer(fb);
	if (std::getenv("VULKANTOY_HOSTED_TRACE") != nullptr) {
		std::fprintf(stderr, "[DRIVE] ed-present done\n"); std::fflush(stderr);
	}
	PollCookProcess();
	PollBakeProcess();
	PollPublishProcess();
	DriveWysiwygSmoke();
}

void EditorApp::SetWysiwygSmoke() {
	m_wysiwygSmoke = true;
}

void EditorApp::DriveWysiwygSmoke() {
	// Dev crash-repro (--wysiwyg-smoke): cook the loaded scene minimally,
	// let the auto-preview launch, render frames, cook as the product
	// step under the live view, render, then exit. The project flow
	// (--project-smoke) additionally covers on-demand Bake GI between
	// preview and the in-place sidecar rebind. Every stage logs so a
	// crash position is identifiable from stderr.
	if (!m_wysiwygSmoke)
		return;
	switch (m_smokeStage) {
	case 0: // the live hosted view must be up from the open (no cook!)
		if (!m_world) {
			std::cerr << "[SMOKE] FAILED: no world (see log)\n";
			RequestClose();
			return;
		}
		if (m_wysiwyg) {
			std::cerr << "[SMOKE] stage 0 -> 1: live view up "
				"(hosted renderer on the editor's world)\n";
			m_smokeStage = 1;
			m_smokeFrames = 0;
		}
		return;
	case 1: // render under the live view; then the PRODUCT step
		// Camera-drive proof (frame 12): rotate the ACTIVE editor
		// camera — the hosted live view must follow (dump-armed runs
		// capture a post-rotation frame; see VULKANTOY_WYSIWYG_DUMP).
		if (m_smokeFrames == 12 && !m_editorCams.empty()) {
			EditorCam& cam =
				m_editorCams[m_viewports[m_activeVp].camIdx];
			cam.yaw += 0.9f;
			cam.pitch = std::clamp(cam.pitch - 0.35f, -1.5f, 1.5f);
			cam.dist = std::max(cam.dist * 0.15f, 0.05f); // close-up
			std::cerr << "[SMOKE] editor camera rotated+zoomed — "
				"the live view must follow\n";
			// Scene-env proof: swing the authored sun IN THE SCENE
			// (CurrentEnv is scene-backed; ResolveSceneEnv refreshes
			// the rendered copy) — the hosted live view's sun
			// (directional light + shadows + skydome disc) must follow.
			CurrentEnv().sunAzimuth += 90.0f;
			ResolveSceneEnv();
			std::cerr << "[SMOKE] scene sun azimuth +90 — "
				"the live view must follow\n";
		}
		// Debug-view-drive proof (frame 16): the editor's Unlit mode
		// must hook the hosted frame (same camera as the rotated
		// reference run — a differing dump isolates the view switch).
		// Wireframe proof rides the same shot: P toggles the hidden-
		// line wireframe (Line mode over the Fill prepass — lines
		// never draw over the objects in front).
		if (m_smokeFrames == 16) {
			ActiveViewMode() = ViewMode::Unlit;
			m_wireframeAll = !m_wireframeAll;
			std::cerr << "[SMOKE] editor view mode -> Unlit + wireframe "
				"on — the live view must follow\n";
		}
		if (++m_smokeFrames > 20) {
			if (m_cookRunning)
				return;
		if (m_projectSmoke) {
			// Project flow additionally covers on-demand Bake GI: bake,
			// wait it out, then the in-place sidecar rebind below.
			std::cerr << "[SMOKE] stage 1 -> 1b: Bake GI (on demand)\n";
			BakeGI();
			m_smokeStage = 4;
			m_smokeFrames = 0;
		}
		else {
			// Plain flow: cook as the PRODUCT step under the live
			// view (the cook never touches the viewport — the view
			// must stay live across it).
			std::cerr << "[SMOKE] stage 1 -> 1c: cook (product step "
				"under the live view)\n";
			m_cook.lightmap = false;
			m_cook.cubemap = false;
			m_cook.lods = false;
			m_cook.meshlets = false;
			m_cook.giCvol = false;
			m_cook.giWorld = false;
			CookToPak();
			m_smokeStage = 3;
			m_smokeFrames = 0;
		}
	}
	return;
case 3: // cook done: the live view must have survived it (no rebind —
		// the cook never changed the world; the in-place rebind makes a
		// post-cook relaunch unnecessary by construction).
	if (m_cookRunning)
		return;
	if (++m_smokeFrames > 3) {
		if (!m_wysiwyg) {
			std::cerr << "[SMOKE] FAILED: live view died across the "
				"cook (see log)\n";
			RequestClose();
			return;
		}
		std::cerr << "[SMOKE] stage 1c -> 2: live view survived the cook "
			"(no rebind needed)\n";
		m_smokeStage = 2;
		m_smokeFrames = 0;
	}
	return;
case 4: // on-demand bake running; then the in-place sidecar rebind
	if (!m_bakeRunning) {
		if (m_giBakedHash.empty()) {
			std::cerr << "[SMOKE] FAILED: Bake GI did not complete "
				"(see log)\n";
			RequestClose();
			return;
		}
		// Same path as the bake-done UI flow (editor_cook.cpp): the
		// sidecars refresh IN PLACE — no renderer relaunch exists.
		std::cerr << "[SMOKE] stage 1b -> 2: in-place sidecar rebind\n";
		if (m_wysiwyg && m_world) {
			m_wysiwyg->SetSceneId(SceneIdForHost());
			m_wysiwyg->SetProjectRoot(m_projectRoot);
			m_wysiwyg->RebindWorld(m_world.get());
		}
		m_smokeStage = 2;
		m_smokeFrames = 0;
	}
	else if (++m_smokeFrames > 3600) {
		std::cerr << "[SMOKE] FAILED: Bake GI timed out (see log)\n";
		RequestClose();
	}
	return;
	case 2: // render after the product step (cook/bake + the sidecar
		// rebind in the project flow), then exit
		if (++m_smokeFrames > 20) {
		if (!m_wysiwyg) {
			std::cerr << "[SMOKE] FAILED: live view died across the "
				"sidecar rebind (see log)\n";
			RequestClose();
			return;
		}
		std::cerr << "[SMOKE] stage 2 done: exiting cleanly\n";
		RequestClose();
	}
	return;
	}
}

void EditorApp::Log(const std::string& s) {
	m_consoleLines.push_back(s);
	if (m_consoleLines.size() > 400)
		m_consoleLines.erase(m_consoleLines.begin());
	std::cerr << "[Editor] " << s << "\n";
}

} // namespace editor
