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
#include <Engine/Renderer/mesh.h>
#include <gltf_import/gltf.h>
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
	return LoadGltfIntoWorld(path);
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

bool EditorApp::Initialize() {
	if (!Super::Initialize())
		return false;
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

	// Scratch disk (editor artifact cache): open the registry before
	// any scene work; a missing/unwritable root degrades to
	// cache-off (the editor still runs — the cache is never a
	// precondition).
	OpenCacheDb();


	m_imgui = std::make_unique<smvk::ImGuiOverlay>();
	smvk::ImGuiOverlayDesc imguiDesc{};
	if (!m_imgui->Init(*m_context, *m_swapchain, window, imguiDesc)) {
		std::cerr << "[Editor] ImGui init failed, continuing without UI\n";
		m_imgui.reset();
	}
	else {
		imguiEventHook_ = &smvk::ImGuiOverlay::ProcessEvent;
		// Opaque menu/window surfaces: the dark theme leaves these
		// slightly translucent (the 3D viewport bleeds through menus).
		// ONLY the top-level surfaces change — every other entry keeps
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
	}

	// Default editor cameras (Perspective / Top ortho / Side ortho).
	SetupDefaultCameras();

	// Tourmaline Editor branding: window title + generated logo icon.
	ApplyTourmalineBranding();

	LoadPrefs(); // load editor prefs (window sizes, panel widths, etc.)

	if (!m_pendingGltf.empty()) {
		const std::string p = m_pendingGltf;
		m_pendingGltf.clear();
		if (!OpenScene(p))
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
	PollCookProcess();
	PollRunProcess();
	PollPublishProcess();
	m_imgui.reset();
	m_shadowAtlas.Destroy(*m_rendergraph);
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
	m_shadowProgram = {};
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
			// prefs value must not degenerate the layout — a
			// sub-tab-height dock, for instance, made the console
			// tab look dead).
			if (section == "outliner" && key == "width")
				m_outlinerW = std::clamp(std::stof(val), 200.0f, 720.0f);
			else if (section == "inspector" && key == "width")
				m_inspectorW = std::clamp(std::stof(val), 200.0f, 720.0f);
			else if (section == "bottomDock" && key == "height")
				m_bottomDockH = std::clamp(std::stof(val), 120.0f, 600.0f);
			else if (section == "scratch" && key == "dir")
				m_scratchRoot = val;
			else if (section == "wysiwyg" && key == "preview")
				m_previewAfterCook = (val == "1" || val == "true");
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
	if (!m_scratchRoot.empty())
		out << "[scratch]\ndir=" << m_scratchRoot << "\n";
	out << "[wysiwyg]\npreview=" << (m_previewAfterCook ? 1 : 0) << "\n";
}

bool EditorApp::OpenCacheDb() {
	m_cacheDb.Close();
	m_store.reset();
	// Prefs override first, machine default second (F:\ when present).
	std::string root = m_scratchRoot;
	if (root.empty())
		root = smvk::cache::DefaultScratchRoot();
	root = smvk::cache::EnsureScratchRoot(root);
	if (root.empty()) {
		m_scratchRoot.clear();
		Log("Scratch disk unavailable — artifact cache disabled "
			"(set one via File > Scratch disk...).");
		return false;
	}
	m_scratchRoot = root;
	std::error_code ec;
	const std::filesystem::path dbPath =
		std::filesystem::path(root) / "cache.db";
	std::string err;
	if (!m_cacheDb.Open(dbPath.generic_string(), err)) {
		Log("Scratch cache db open failed: " + err);
		return false;
	}
	std::snprintf(m_scratchBuf, sizeof(m_scratchBuf), "%s",
		m_scratchRoot.c_str());
	m_store = std::make_unique<smvk::cache::ArtifactStore>(
		m_scratchRoot, m_cacheDb);
	std::string cerr;
	const uint64_t count = m_cacheDb.Count(cerr);
	Log("Scratch disk: " + root + " (cache.db ready, "
		+ std::to_string(count) + " artifact(s) registered)");
	// Editor VFS: register the scratch class paks (the cooked-resource
	// overlay the display path resolves through). ONLY tour + scratch
	// mount here — never the release content tree (editor -> release
	// is one-way; reading content/paks back would feed stale cooked
	// content into authoring). The scene class mounts on preview
	// (MountScratchSceneBackend); these are the resource classes.
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
	m_editSourceGltf.clear();
	m_previewPakPath.clear();
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
	m_refPhCluster.clear();
	m_envByNode.clear();
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
	// glTF to import — an empty world whose geometry comes entirely
	// from references. tourOverride pins the reference lookup to the
	// opened tour file (otherwise <stem>.tour next to the source,
	// which does not exist here).
	std::string loadPath = path;
	tinygltf::Model model;
	bool haveModel = !path.empty(); // grows: tour opens parse the bound source
	const bool directOpen = !path.empty(); // direct glTF open vs tour open
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
	// Fresh edit context: any prior cooked preview relationship ends.
	m_editSourceGltf.clear();
	m_previewPakPath.clear();
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
			std::make_unique<smvk::TextureStreamer>(*m_context, streamCfg);
	}
	m_world = std::make_unique<smvk::World>();
	smvk::Cluster& cluster = m_world->LoadCluster(*m_context);
	// THE DEFINITION BUILD: the world assembles from the tour doc.
	// Reference #0 is the source glTF itself (a ROOT reference — no
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
			rootRef.name = std::filesystem::path(loadPath).stem().string();
			if (haveDoc && !refDoc.references.empty()
				&& refDoc.references[0].root)
				refDoc.references[0] = rootRef;
			else
				refDoc.references.insert(refDoc.references.begin(),
					rootRef);
		}
		if (!refDoc.references.empty()) {
			const size_t applied = ApplySceneReferences(cluster, refDoc,
				std::filesystem::path(loadPath).parent_path()
					.generic_string(),
				m_context.get(), m_textureStreamer.get(),
				nullptr /* host files — cook-parity resolution */,
				smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0,
				&m_refPhCluster, &m_refIssues, &m_refResolved);
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
	// bind above just filled them for THIS world — clearing would wipe
	// the panel's reporting. They reset at load start/close instead.)
	m_defaultMatWorld = UINT32_MAX;
	m_overrideRules.clear();
	m_nextOverrideId = 1;
	m_envByNode.clear(); // env nodes restored by LoadTour (editorSet)
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
	// placeholder by design) — mapped to UINT32_MAX so the parallel
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
		// Temp-doc reload (reference Reload): the snapshot — not the
		// on-disk file — is the document. No autosave offer either:
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
				+ ") — falling back to the tour file.");
			LoadTour();
			CheckAutosaveOffer();
		}
	}
	else if (!tourOverride.empty()) {
		// Opened definition tour: OpenTourFile replays its own doc
		// right after this build — nothing loads here (a second load
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
	// kept, ids stable — the per-frame visibility filters stop draws/
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
	char buf[512];
	std::snprintf(buf, sizeof(buf), "Loaded '%s' (nodes=%zu meshes=%zu materials=%zu)",
		path.c_str(), m_world->scene.hierarchy.size(),
		m_world->meshLocations.size(), m_world->materialLocations.size());
	Log(buf);
	return true;
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
	// Cooked .pak display core: same path as App's SceneRenderer
	// (PakReader + LoadClusterFromPak + Resolve; the renderer renders a
	// SCENE, index 0 by default — --pak parity). Pak textures upload
	// fully at load, so the glTF texture streamer is dropped. Picking
	// falls back to AABB (no glTF model kept); tour replay is skipped
	// (tours are authored against the source glTF, not the cooked
	// pak). The image + parsed reader stay members (never re-parsed
	// per frame: Open decompresses).
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
	// Plain pak opens carry no edit-source relationship (only the
	// WYSIWYG preview flow restores it after the open).
	m_editSourceGltf.clear();
	m_previewPakPath.clear();
	m_world = std::make_unique<smvk::World>();
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
	m_envByNode.clear(); // pak loads replay no tour: default IBL stands
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
	return true;
}

void EditorApp::MountScratchSceneBackend() {
	// Registers the scratch scene pak image as a PakBackend on the
	// editor VFS (replacing the previous "scratch-scene"). ONLY tour +
	// scratch ever mount here — the release content tree (content/)
	// with its paks is a FORWARD output and is never read back.
	m_editorVfs.Unmount("scratch-scene");
	if (!m_pakImage || m_pakImage->empty())
		return;
	std::string err;
	auto backend = smvk::PakBackend::Open(
		std::shared_ptr<const std::vector<uint8_t>>(m_pakImage),
		"scratch-scene", err);
	if (!backend) {
		Log("Editor VFS: scratch scene registration failed: " + err);
		return;
	}
	m_editorVfs.Mount(backend, 10, "scratch-scene");
	Log("Editor VFS: scratch scene registered (textures served from the scratch copy).");
}

void EditorApp::PreviewCookedResult(const std::string& pakPath) {
	// WYSIWYG: display the COOKED scene — the scratch copy of the cook,
	// NEVER the release pak (editor -> release is one-way forwards;
	// reading content/paks back would feed stale cooked content into
	// authoring). The pak bytes stage into the artifact store (class
	// "scene", keyed by the definition fingerprint), and the preview
	// loads scene 0 from the scratch image — the renderer renders a
	// SCENE, the pak is its blob backing (--pak parity).
	const std::string source = m_gltfHostPath;
	const bool fromEdit = !source.empty() && !HasPakExtension(source);
	// 1. Stage the cooked pak into the scratch (cache bookkeeping).
	std::string stem = std::filesystem::path(pakPath).stem().string();
	std::string serr;
	bool staged = false;
	if (m_store && !m_cookDefHash.empty()) {
		std::ifstream f(pakPath, std::ios::binary | std::ios::ate);
		if (f) {
			const auto end = f.tellg();
			f.seekg(0);
			auto image = std::make_shared<std::vector<uint8_t>>(
				static_cast<size_t>(end), 0);
			if (f.read(reinterpret_cast<char*>(image->data()),
					static_cast<std::streamsize>(image->size()))) {
				if (m_store->Put("scene", stem,
						std::span<const uint8_t>(*image), m_cookDefHash,
						"{\"pak\":\"" + pakPath + "\"}", serr)
					&& m_store->Flush(serr)) {
					staged = true;
					Log("Scratch: cooked scene staged + flushed ("
						+ std::to_string(image->size()) + " B, uri '"
						+ stem + "').");
				}
			}
		}
	}
	// 2. Display from the SCRATCH copy (tour + scratch only — no
	// release readback).
	std::vector<uint8_t> scratchImage;
	if (!m_store || !m_store->ReadPakImage("scene", scratchImage, serr)) {
		Log("WYSIWYG preview needs the scratch cache (File > Scratch "
			"disk...): " + serr);
		return;
	}
	auto image = std::make_shared<std::vector<uint8_t>>(
		std::move(scratchImage));
	// The GI sidecars (sglm/cvols) are loose files baked next to the
	// release pak by THIS cook — current outputs, not stale readback;
	// displayPath carries the stem for their lookup.
	if (!LoadPakFromImage(image, pakPath, 0))
		return;
	MountScratchSceneBackend();
	if (fromEdit) {
		m_editSourceGltf = source;
		m_previewPakPath = pakPath;
		Log("WYSIWYG: viewing the cooked scene (scratch copy) — "
			"File > Back to edit source (Ctrl+Alt+E) to continue "
			"editing.");
	}
	else {
		Log("WYSIWYG: cooked scene staged"
			+ std::string(staged ? "" : " (definition hash missing)")
			+ "; displaying the scratch copy.");
	}
}

void EditorApp::ReturnToEditSource() {
	if (m_editSourceGltf.empty())
		return;
	const std::string source = m_editSourceGltf;
	// LoadGltfIntoWorld clears the preview state; the tour replay
	// restores the full edit stack (selection/gizmos/history).
	if (!OpenScene(source)) {
		Log("Back to edit source failed: " + source);
		return;
	}
	Log("Back to editing: " + source);
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
// fill, color-coded by priority) — draw lives in DrawCvolPreviewOverlay
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
	// pool indices — equal only by creation-order coincidence. Resolving
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
	// tour file is current — every edit saves it — so edits replay back).
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
	// Map group node name+child-index back: simpler robust approach — walk
	// the WORLD tree: a node with a mesh whose parent has the same mesh
	// count pattern... Instead do a parallel recursion mirroring the
	// importer: world cluster root (node 0) children == glTF roots.
	// The importer attaches glTF roots under the cluster root via
	// importNodeRecursive(gltf, parent=clusterRoot?) — actually under the
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
		// Prim nodes carry meshes (skipped here — paired below via their
		// group). The artificial cluster root is unnamed — skip it too so
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
	// angled perspective-like views — only the Projection combo changes
	// the type. Ortho zoom scales the half-height.
	SetupDefaultCameras();
	// Flat-UV panes own the wheel + MMB (pane zoom/pan in UpdateUvPaneInput):
	// the 3D orbit camera never moves while a flat pane is active.
	if (IsFlatUvViewMode(ActiveViewMode()))
		return;
	// Locked look-through: orbit navigation is parked while the active
	// viewport follows a live camera node (move the node via gizmo or
	// the transform inspector — the view tracks it). A lost node falls
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
	Update();
	auto fb = m_swapchain->AcquireFramebuffer();
	if (fb.imageIndex >= 0)
		RenderFrame(fb);
	m_swapchain->EndAndPresentFramebuffer(fb);
	PollCookProcess();
	PollRunProcess();
	PollPublishProcess();
}

void EditorApp::Log(const std::string& s) {
	m_consoleLines.push_back(s);
	if (m_consoleLines.size() > 400)
		m_consoleLines.erase(m_consoleLines.begin());
	std::cerr << "[Editor] " << s << "\n";
}

} // namespace editor
