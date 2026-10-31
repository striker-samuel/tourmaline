// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Project abstraction (Unity/Unreal style): a project folder owns
// everything the editor needs for one body of work —
//   <root>/project.tourproj   marker (name + guid + version)
//   <root>/content/            sources (models/<stem>/*.gltf, scenes/*.tour)
//   <root>/paks/               cooked outputs (<stem>.pak)
//   <root>/gi/                 loose bake files (written by the cooker next
//                             to the out pak; resolved by the loaders)
//   <root>/.tourmaline/        OWNED artifact cache (the Unity Library/
//                             equivalent: cache.db + assets.db +
//                             pak/<klass>.pak, hidden on Windows).
//                             No global disk: without a
//                             project the cache is off.
// Opening a project repoints the artifact cache, the cook defaults and
// the artifact cache at it. Importing an asset copies it into the
// project, adopts its GUID and cooks show the result with the full
// render pipeline — no manual staging, no global cache.

#include "editor_app.h"

#include <artifact_cache/artifact_kind.h>
#include <artifact_cache/assetdb.h>

#include <Engine/Asset/tour.h>
#include <Engine/Utils/file_watcher.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

namespace editor {
namespace {
constexpr const char* kProjMarker = "project.tourproj";
// v2: the required `guid=` identity row (no v1 reader — OpenProject
// rejects guid-less markers with the recreate/re-init instructions).
constexpr const char* kProjVersion = "2";
// CookSettings' baked-in default: still meaning "user never chose".
constexpr const char* kLegacyOutPak = "content/paks/edited.pak";

std::string Normalized(const std::string& p) {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path abs = fs::absolute(fs::path(p), ec);
	if (ec)
		abs = fs::path(p);
	return abs.lexically_normal().generic_string();
}

bool HasModelExtension(const std::string& path) {
	auto lowerTail = [&](size_t n) {
		if (path.size() < n)
			return std::string();
		std::string t = path.substr(path.size() - n);
		for (char& c : t)
			c = char(std::tolower((unsigned char)c));
		return t;
	};
	return lowerTail(4) == ".glb" || lowerTail(5) == ".gltf";
}

} // namespace

std::string EditorApp::ProjectArtifactDir() const {	if (m_projectRoot.empty())
		return {};
	return (std::filesystem::path(m_projectRoot) / ".tourmaline")
		.generic_string();
}

bool EditorApp::InProjectDir(const std::string& absPath) const {
	if (m_projectRoot.empty() || absPath.empty())
		return false;
	namespace fs = std::filesystem;
	std::error_code ec;
	const std::string norm = fs::absolute(fs::path(absPath), ec)
		.lexically_normal().generic_string();
	if (ec)
		return false;
	std::string root = fs::path(m_projectRoot).lexically_normal()
		.generic_string();
	if (!root.empty() && root.back() != '/')
		root += '/';
	return norm.size() > root.size()
		&& norm.compare(0, root.size(), root) == 0;
}

std::string EditorApp::ProjectRelativePath(
	const std::string& absPath) const {
	if (!InProjectDir(absPath))
		return {};
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path rel = fs::relative(fs::path(absPath),
		fs::path(m_projectRoot), ec);
	if (ec || rel.empty())
		return {};
	std::string posix = rel.generic_string();
	// Belt-and-suspenders (InProjectDir already guarantees
	// containment): never emit an escaping relative path.
	if (posix == ".." || posix.compare(0, 3, "../") == 0
		|| posix.find("/../") != std::string::npos)
		return {};
	return posix;
}

std::string EditorApp::ProjectCookOutPak(
	const std::string& sceneStem) const {
	if (m_projectRoot.empty() || sceneStem.empty())
		return {};
	return (std::filesystem::path(m_projectRoot) / "paks"
		/ (sceneStem + ".pak")).generic_string();
}

bool EditorApp::IsProjectDir(const std::string& dir) {
	if (dir.empty())
		return false;
	std::error_code ec;
	return std::filesystem::is_regular_file(
		std::filesystem::path(dir) / kProjMarker, ec);
}

std::string EditorApp::ReadProjectName(const std::string& dir) {
	std::ifstream in(
		std::filesystem::path(dir) / kProjMarker, std::ios::binary);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		if (line.substr(0, eq) == "name")
			return line.substr(eq + 1);
	}
	return {};
}

std::string EditorApp::ReadProjectGuid(const std::string& dir) {
	// The persistent identity row (the pipeline's Project::Open agrees
	// on the same parse — the marker format is shared contract).
	std::ifstream in(
		std::filesystem::path(dir) / kProjMarker, std::ios::binary);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		if (line.substr(0, eq) == "guid")
			return line.substr(eq + 1);
	}
	return {};
}

void EditorApp::PushRecentProject(const std::string& dir) {
	// Most-recent-first, deduped, capped (the startup dialog lists it).
	auto it = std::find(m_recentProjects.begin(), m_recentProjects.end(),
		dir);
	if (it != m_recentProjects.end())
		m_recentProjects.erase(it);
	m_recentProjects.insert(m_recentProjects.begin(), dir);
	if (m_recentProjects.size() > 8)
		m_recentProjects.resize(8);
}

bool EditorApp::OpenProject(const std::string& dir) {
	const std::string root = Normalized(dir);
	if (!IsProjectDir(root)) {
		Log("Not a project (no " + std::string(kProjMarker) + "): "
			+ root);
		return false;
	}
	// The identity contract (current-format policy — the pipeline's
	// Project::Open enforces the same row): no valid guid, no project.
	// Actionable, never a silent name-derived fallback.
	{
		const std::string guid = ReadProjectGuid(root);
		if (guid.empty()
				|| !smvk::cache::IsValidAssetGuid(guid)) {
			Log("Project '" + root + "' has no valid identity"
				" (guid= row missing or malformed in "
				+ std::string(kProjMarker) + ") — the current"
				" project format requires one. Recreate the"
				" project (or run 'Project.exe init' on it once)"
				" to mint its identity.");
			return false;
		}
		m_projectGuid = guid;
	}
	PushRecentProject(root);
	if (root == m_projectRoot) {
		Log("Project already open: " + m_projectName);
		return true;
	}
	// A project switch drops the live scene (its live view dies with
	// it — CloseScene suspends + retires safely; the tour autosave
	// contract in CloseScene keeps edits safe).
	CloseScene();
	m_projectRoot = root;
	m_projectName = ReadProjectName(root);
	if (m_projectName.empty())
		m_projectName = std::filesystem::path(root).filename().string();
	// Per-scene cook outputs live in the project now; drop any stale
	// default so the next open/import re-derives into project paks/.
	m_cook.outPak.clear();
	m_cookOutPakBuf[0] = '\0';
	// The project OWNS its artifact cache: reopen cache.db + the
	// asset directory there (a missing or unwritable dir degrades to
	// cache-off — the cache only accelerates processing).
	{
		// One-time migration: the owned dir used to be <root>/scratch.
		// Move it aside when <root>/artifacts does not exist yet so
		// existing projects keep their registry + payloads.
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path oldDir = fs::path(root) / "scratch";
		const fs::path newDir = fs::path(root) / "artifacts";
		if (fs::is_directory(oldDir, ec) && !fs::exists(newDir, ec)) {
			fs::rename(oldDir, newDir, ec);
			if (!ec)
				Log("Project: migrated 'scratch/' -> 'artifacts/'.");
			else
				Log("Project: cannot migrate 'scratch/' -> "
					"'artifacts/': " + ec.message());
		}
	}
	if (!OpenCacheDb())
		Log("Project '" + m_projectName + "' open, but its artifact "
			"cache is unusable — cache disabled for this session.");
	// Source-asset directory over the project (Explorer-side
	// moves/renames heal through the sidecars on rescan).
	m_assetDb.Close();
	if (m_store) {
		std::string dberr;
		if (m_assetDb.Open(m_store->Root(), m_projectRoot, dberr)) {
			smvk::cache::RescanStats stats;
		if (m_assetDb.RescanProject(stats, dberr))
			Log("Asset directory: "
				+ std::to_string(stats.adopted) + " adopted, "
				+ std::to_string(stats.moved) + " re-pointed, "
				+ std::to_string(stats.derived) + " derived, "
				+ std::to_string(stats.missing) + " missing.");
			else
				Log("Asset directory rescan failed: " + dberr);
		}
		else {
			Log("Asset directory unavailable: " + dberr);
		}
	}
	SavePrefs();
	UpdateWindowTitle();
	ArmProjectWatcher();
	Log("Project open: '" + m_projectName + "' (" + m_projectRoot
		+ ") — artifact\ cache\ and\ cooks\ live\ here\ now.");
	return true;
}

bool EditorApp::NewProjectOrOpen(const std::string& dir) {
	const std::string root = Normalized(dir);
	namespace fs = std::filesystem;
	std::error_code ec;
	if (IsProjectDir(root))
		return OpenProject(root); // already a project: just open it
	// Scaffold the Unity-style tree (.tourmaline/ itself is created by
	// EnsureRegistryRoot when the cache opens).
	fs::create_directories(fs::path(root) / "content" / "models", ec);
	fs::create_directories(fs::path(root) / "content" / "scenes", ec);
	fs::create_directories(fs::path(root) / "paks", ec);
	fs::create_directories(fs::path(root) / "gi", ec);
	if (ec) {
		Log("Project: cannot scaffold '" + root + "': "
			+ ec.message());
		return false;
	}
	{
		std::ofstream out(fs::path(root) / kProjMarker,
			std::ios::binary | std::ios::trunc);
		if (!out) {
			Log("Project: cannot write " + std::string(kProjMarker)
				+ " in '" + root + "'.");
			return false;
		}
		// The identity is minted ONCE here (creation); every later
		// open just reads it (OpenProject's rejection is the
		// actionable gate for pre-identity markers).
		const std::string guid = smvk::tour::GenerateUuid();
		out << "# Tourmaline project (name + identity + format version)\n";
		out << "name=" << fs::path(root).filename().string() << "\n";
		out << "guid=" << guid << "\n";
		out << "version=" << kProjVersion << "\n";
	}
	return OpenProject(root);
}

void EditorApp::PrimeProjectCookDefaults() {
	// Per-scene outputs land in the project when the user hasn't
	// customized them (empty, or the baked-in legacy default). Both
	// the dialog-less cook (m_cook.outPak) and the cook dialog buffer
	// agree afterwards.
	if (m_projectRoot.empty() || m_gltfPath.empty())
		return;
	if (!m_cook.outPak.empty() && m_cook.outPak != kLegacyOutPak)
		return; // user (or --out-pak) chose: keep it
	const std::string stem =
		std::filesystem::path(m_gltfPath).stem().string();
	const std::string outPak = ProjectCookOutPak(stem);
	if (outPak.empty())
		return;
	m_cook.outPak = outPak;
	std::snprintf(m_cookOutPakBuf, sizeof(m_cookOutPakBuf), "%s",
		outPak.c_str());
}

void EditorApp::AdoptProjectAsset(const std::string& assetPath,
	const std::string& importer) {
	if (m_projectRoot.empty() || !m_assetDb.IsOpen()
		|| assetPath.empty())
		return;
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::is_regular_file(assetPath, ec))
		return;
	// A copied-along sidecar keeps its GUID (moves preserve identity);
	// otherwise a fresh one is issued and written.
	std::string guid, err;
	if (smvk::cache::ReadAssetMeta(assetPath, guid, err)) {
		if (!m_assetDb.AdoptFile(assetPath, guid, importer, err))
			Log("Asset directory: cannot adopt '" + assetPath + "': "
				+ err);
		return;
	}
	if (err != "no .meta")
		Log("Asset directory: " + err + " — issuing a fresh GUID.");
	guid = smvk::tour::GenerateUuid();
	if (!smvk::cache::WriteAssetMeta(assetPath, guid, importer, err)) {
		Log("Asset directory: cannot write .meta for '" + assetPath
			+ "': " + err);
		return;
	}
	if (!m_assetDb.AdoptFile(assetPath, guid, importer, err))
		Log("Asset directory: cannot adopt '" + assetPath + "': "
			+ err);
	else
		Log("Asset directory: new GUID " + guid + " for '"
			+ assetPath + "'.");
}

std::string EditorApp::ProjectAssetGuid(const std::string& assetPath) {
	AdoptProjectAsset(assetPath);
	if (m_projectRoot.empty() || !m_assetDb.IsOpen()
		|| assetPath.empty())
		return {};
	std::string guid;
	if (!m_assetDb.GuidForPath(assetPath, guid))
		return {};
	return guid;
}

std::string EditorApp::ImportAssetFile(const std::string& srcPath) {
	if (m_projectRoot.empty()) {
		Log("Import: open or create a project first (File > New / Open "
			"Project...) — imports live inside the project.");
		return {};
	}
	if (!HasModelExtension(srcPath)) {
		Log("Import: only .gltf/.glb sources in this version: "
			+ srcPath);
		return {};
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::is_regular_file(srcPath, ec)) {
		Log("Import: source not found: " + srcPath);
		return {};
	}
	const fs::path src(srcPath);
	const std::string stem = src.stem().string();
	const fs::path destDir =
		fs::path(m_projectRoot) / "content" / "models" / stem;
	fs::create_directories(destDir, ec);
	if (ec) {
		Log("Import: cannot create '" + destDir.generic_string()
			+ "': " + ec.message());
		return {};
	}
	// DEPENDENCY CLOSURE (structure-preserving): parse the source
	// once here and copy every external buffer + image URI at its
	// URI-relative path under the destination — the model's relative
	// links keep resolving IN the project (subfolder textures
	// included; the old flat sibling copy missed them and the links
	// broke, with no way for the path editor to relink a source that
	// never came along). `.ktx2` sidecars ride their images (same
	// dir, same stem — the auto-ktx convention), the source's own
	// .meta keeps its GUID. Unreferenced files stay behind (no
	// folder dump); stale .tour files never come (they pin the OLD
	// source path). Same-dir import adopts in place.
	const fs::path srcDir = src.parent_path();
	uint64_t copiedBytes = 0;
	size_t copiedFiles = 0;
	auto copyIn = [&](const fs::path& from, const fs::path& to)
		-> bool {
		std::error_code cec;
		if (fs::equivalent(from, to, cec) && !cec)
			return true; // same file (safety net)
		fs::create_directories(to.parent_path(), cec);
		if (cec) {
			Log("Import: cannot create '"
				+ to.parent_path().generic_string() + "': "
				+ cec.message());
			return false;
		}
		fs::copy_file(from, to, fs::copy_options::overwrite_existing,
			cec);
		if (cec) {
			Log("Import: copy failed '" + from.generic_string()
				+ "': " + cec.message());
			return false;
		}
		++copiedFiles;
		std::error_code sec;
		copiedBytes += fs::file_size(from, sec);
		return true;
	};
	if (fs::equivalent(srcDir, destDir, ec) && !ec) {
		Log("Import: source already inside the project — adopting '"
			+ srcPath + "' in place.");
	}
	else {
		// The model file first (dependencies may point back at it).
		if (!copyIn(src, destDir / src.filename()))
			return {};
		// External dependencies: buffers + images with URIs.
		std::vector<std::string> uris;
		{
			tinygltf::Model deps;
			if (!LoadModelFromAsciiFile(deps, srcPath.c_str())) {
				Log("Import: source parse failed — copied the model "
					"file only (dependencies may dangle; the Path "
					"editor can relink).");
			}
			else {
				for (const auto& b : deps.buffers)
					if (!b.uri.empty() && b.uri.rfind("data:", 0) != 0)
						uris.push_back(b.uri);
				for (const auto& im : deps.images)
					if (!im.uri.empty() && im.uri.rfind("data:", 0) != 0)
						uris.push_back(im.uri);
			}
		}
		size_t missing = 0, flattened = 0;
		for (const std::string& uri : uris) {
			const fs::path rel(uri);
			const fs::path from = (srcDir / rel).lexically_normal();
			std::error_code fec;
			if (!fs::is_regular_file(from, fec)) {
				++missing;
				Log("Import: dependency missing at the source: '"
					+ uri + "' (" + from.generic_string() + ").");
				continue;
			}
			fs::path to = (destDir / rel).lexically_normal();
			// Escaping URIs ("../x") would land outside the project:
			// flatten into the asset dir — the link stays broken;
			// the Path editor relinks those.
			if (!InProjectDir(to.generic_string())) {
				to = destDir / from.filename();
				++flattened;
			}
			if (!copyIn(from, to))
				return {};
			// `.ktx2` sidecar (same dir, same stem): best-effort ride.
			fs::path side = from;
			side.replace_extension(".ktx2");
			if (fs::is_regular_file(side, fec)) {
				fs::path sideTo = to;
				sideTo.replace_extension(".ktx2");
				(void)copyIn(side, sideTo);
			}
		}
		// The source's own .meta sidecar (GUID identity) rides along.
		std::error_code mec;
		const fs::path srcMeta = srcDir
			/ (src.filename().string() + ".meta");
		if (fs::is_regular_file(srcMeta, mec))
			(void)copyIn(srcMeta,
				destDir / (src.filename().string() + ".meta"));
		char buf[160];
		std::snprintf(buf, sizeof(buf), "%zu file(s), %.1f MiB",
			copiedFiles, double(copiedBytes) / (1024.0 * 1024.0));
		Log("Import: '" + src.filename().string() + "' + dependency "
			"closure -> '" + destDir.generic_string() + "' (" + buf
			+ (missing ? std::string(", ") + std::to_string(missing)
				+ " missing" : "")
			+ (flattened ? std::string(", ")
				+ std::to_string(flattened)
				+ " flattened (../)" : "")
			+ ").");
	}
	const fs::path dest = destDir / src.filename();
	// The copied-along .meta (if any) keeps the asset's GUID; a fresh
	// one is issued otherwise — the file is addressable by UUID from
	// here on (tours reference it via -guid).
	AdoptProjectAsset(dest.generic_string());
	return dest.generic_string();
}

std::string EditorApp::CreateTextureAsset(const std::string& srcPath,
	const std::string& dir) {
	// TEXTURE-ASSET creation (see the header): copy the source image in,
	// write <stem>.texture + identity (.meta sidecar + assetdb row), select.
	if (m_projectRoot.empty()) {
		Log("Create texture: open or create a project first (File > New / "
			"Open Project...).");
		return {};
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path src(srcPath);
	if (!fs::is_regular_file(src, ec)) {
		Log("Create texture: source not found: " + srcPath);
		return {};
	}
	std::string ext = src.extension().string();
	for (char& c : ext)
		c = char(std::tolower((unsigned char)c));
	if (ext != ".png" && ext != ".jpg" && ext != ".jpeg"
		&& ext != ".tga" && ext != ".bmp" && ext != ".ktx2") {
		Log("Create texture: unsupported source '" + ext
			+ "' (want png/jpg/jpeg/tga/bmp/ktx2): " + srcPath);
		return {};
	}
	// The browsed dir (fallback: <root>/content — the browser heals
	// m_assetsDir on the next draw).
	std::string destDir = dir;
	if (destDir.empty() || !IsProjectDir(destDir))
		destDir = (fs::path(m_projectRoot) / "content").generic_string();
	fs::create_directories(destDir, ec);
	if (ec) {
		Log("Create texture: cannot create '" + destDir + "': "
			+ ec.message());
		return {};
	}
	// Copy the source in (skip when already there).
	fs::path srcIn = fs::path(destDir) / src.filename();
	if (!fs::equivalent(src, srcIn, ec)) {
		fs::copy_file(src, srcIn, fs::copy_options::overwrite_existing, ec);
		if (ec) {
			Log("Create texture: copy failed '" + srcPath + "': "
				+ ec.message());
			return {};
		}
	}
	// Existing .texture for the stem: select it (delete it to re-create
	// — the import dedupes by the .texture path, so a re-create is the
	// settings-reset path).
	const std::string stem = src.filename().stem().string();
	fs::path texPath = fs::path(destDir) / (stem + ".texture");
	if (fs::is_regular_file(texPath, ec)) {
		m_assetsSel = texPath.generic_string();
		Log("Create texture: '" + stem + ".texture' already exists — "
			"selected (delete it to re-create).");
		return texPath.generic_string();
	}
	// Defaults: sRGB color, rgba8, full chain, linear/repeat (edit them
	// in the Inspector; a KTX2 source reads colorSpace/format/mipLevels
	// read-only from its header).
	smvk::assets::TextureAsset t{};
	t.guid = smvk::tour::GenerateUuid();
	t.name = stem;
	t.source = src.filename().string();
	std::string err;
	if (!smvk::assets::SaveTextureAsset(texPath, t, err)) {
		Log("Create texture: cannot write '" + texPath.generic_string()
			+ "': " + err);
		return {};
	}
	// Identity: the guid rides the file (inline) AND the sidecar (the
	// assetdb's adoption path) — one guid, both views.
	{
		std::string merr;
		if (!smvk::cache::WriteAssetMeta(texPath.generic_string(),
				t.guid, "texture", merr))
			Log("Create texture: cannot write .meta for '"
				+ texPath.generic_string() + "': " + merr);
	}
	AdoptProjectAsset(texPath.generic_string(), "texture");
	m_assetsSel = texPath.generic_string();
	Log("Create texture: '" + texPath.filename().string() + "' from '"
		+ src.filename().string() + "' (edit the import settings in the "
		"Inspector; materials reference it as tex.<slot>="
		+ stem + ".texture).");
	return texPath.generic_string();
}

void EditorApp::OnInputEvent(const input::Event& e) {
	// OS drag-and-drop (Windows Explorer -> the asset browser): collect
	// the FileDrop batch + the drop point (window coords; the LAST file's
	// point is the release point). The normalized event owns its path
	// string (the bridge copies it out of the native event); keep the copy
	// anyway — DrawAssetsPanel routes the batch next frame against its
	// rect (the only drop target — see HandleDroppedFiles).
	if (e.type == input::EventType::FileDrop && !e.text.empty()) {
		m_pendingDropX = e.x;
		m_pendingDropY = e.y;
		m_pendingDropFiles.push_back(e.text);
	}
}

void EditorApp::HandleDroppedFiles(const std::vector<std::string>& files) {
	// Drop routing (the asset browser's drop target): images become
	// .texture assets in the browsed dir; .gltf/.glb run the full import
	// (copy + derive + open). Everything else drops with a log — never
	// silently.
	if (m_projectRoot.empty()) {
		Log("Drop: open a project first (File > New / Open Project...).");
		return;
	}
	if (files.empty())
		return;
	int imported = 0;
	int unsupported = 0;
	for (const std::string& p : files) {
		std::string ext =
			std::filesystem::path(p).extension().string();
		for (char& c : ext)
			c = char(std::tolower((unsigned char)c));
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg"
			|| ext == ".tga" || ext == ".bmp" || ext == ".ktx2") {
			if (!CreateTextureAsset(p, m_assetsDir).empty())
				++imported;
		}
		else if (ext == ".gltf" || ext == ".glb") {
			if (ImportAsset(p))
				++imported;
		}
		else {
			++unsupported;
			Log("Drop: unsupported format '" + p + "' (want png/jpg/jpeg/"
				"tga/bmp/ktx2/gltf/glb).");
		}
	}
	if (imported > 0)
		Log("Drop: imported " + std::to_string(imported) + " file"
			+ (imported > 1 ? "s" : "") + ".");
}

// THE MESH IMPORTER drive (editor side): glTF is IMPORT-ONLY — parse +
// derive the artifact family beside the source when (and only when) the
// artifacts are missing/stale (MeshArtifactStale: the .geometry absent,
// the manifest corrupt, or the source newer), then hand back the
// .geometry path the session references. "" = failure (logged). This is
// the SINGLE place a .gltf turns into a session: opens (direct or
// tour-recorded), drops and the project import all land here.
std::string EditorApp::ImportMeshArtifactFor(const std::string& gltfPath) {
	namespace fs = std::filesystem;
	if (gltfPath.empty())
		return {};
	const fs::path geometry =
		fs::path(gltfPath).replace_extension(".geometry");
	// The settings FIRST: the stale gate compares the recorded import
	// fingerprint against THIS config (a settings/revision drift
	// re-derives — see ImportFingerprintFor/MeshArtifactStale).
	smvk::assets::DeriveConfig cfg{};
	if (const char* e = std::getenv("TOURMALINE_EDITOR_MESHLETS"))
		cfg.meshlets = std::atoi(e) != 0;
	if (const char* e = std::getenv("TOURMALINE_EDITOR_LODS"))
		cfg.lods = std::atoi(e) != 0;
	// Far-shading derivation (default OFF — see DeriveConfig::shaderLods):
	// opt-in only; the ShaderLodPolicy default keeps vertex-lit off.
	if (const char* e = std::getenv("TOURMALINE_EDITOR_SHADERLODS"))
		cfg.shaderLods = std::atoi(e) != 0;
	if (!smvk::assets::MeshArtifactStale(gltfPath, cfg)) {
		Log("Import: artifact current — opening '" + geometry.generic_string()
			+ "' (no re-import).");
		return geometry.generic_string();
	}
	// NO explicit guid: the derivation consumes the .gltf.meta BESIDE
	// the source as the identity ledger (a re-import over changed
	// content CARRIES the family + subasset identities; a first import
	// mints the deterministic from-zero identity — see asset_export.h).
	// The project DB still adopts the file for its own bookkeeping
	// (ProjectAssetGuid elsewhere); the ARTIFACT identity is the
	// ledger's.
	const std::string guid;
	smvk::assets::AssetManifest manifest;
	smvk::assets::DeriveStats stats;
	std::string derr;
	Log("Import: deriving the mesh artifact from '"
		+ fs::path(gltfPath).filename().generic_string() + "'...");
	if (!smvk::assets::ImportMeshSource(gltfPath, guid, nullptr, cfg,
			manifest, &stats, &derr)) {
		Log("Import: mesh importer failed: " + derr
			+ " — the scene does NOT open from the glTF (glTF is "
			"import-only; fix the source and re-import).");
		return {};
	}
	Log("Import: derived assets ("
		+ std::to_string(stats.geometryBlobs) + " geometry, "
		+ std::to_string(stats.materials) + " materials, "
		+ std::to_string(stats.anims) + " anims, "
		+ std::to_string(stats.instances) + " instances"
		+ (stats.newGuids ? ", " + std::to_string(stats.newGuids)
			+ " new GUID(s)" : "")
		+ (stats.keptGuids ? ", " + std::to_string(stats.keptGuids)
			+ " kept GUID(s)" : "") + ").");
	// Funnel registration (the artifact DB is the pipeline entry
	// point): every manifest entry becomes a (klass, uuid) identity
	// row — no payload yet (derivations live loose beside the source;
	// payloads stage on cook). Skipped when the cache is off (project-
	// less sessions); identity still resolves through the manifest.
	if (m_cacheDb.IsOpen() && !m_projectRoot.empty()) {
		const auto kindOf =
			[](smvk::assets::AssetKind k) -> const char* {
			switch (k) {
			case smvk::assets::AssetKind::Geometry:
				return smvk::cache::kKindMesh;
			case smvk::assets::AssetKind::Material:
				return smvk::cache::kKindMaterial;
			case smvk::assets::AssetKind::Anim:
				return smvk::cache::kKindAnimation;
			}
			return "";
		};
		const fs::path gltfDir = fs::path(gltfPath).parent_path();
		const std::string root = fs::path(m_projectRoot)
			.lexically_normal().generic_string();
		size_t registered = 0;
		for (const smvk::assets::AssetEntry& e : manifest.entries) {
			const char* klass = kindOf(e.kind);
			if (!klass || !*klass || e.guid.empty() || e.file.empty())
				continue;
			std::error_code ec2;
			std::string uri = fs::absolute(gltfDir / e.file, ec2)
				.lexically_normal().generic_string();
			if (!ec2 && uri.size() > root.size()
				&& uri.compare(0, root.size(), root) == 0
				&& uri[root.size()] == '/')
				uri = uri.substr(root.size() + 1);
			// Staleness key: the manifest's source digest when the
			// derivation carries one, else the entry GUID (stable
			// across reimports either way — equality is all Has
			// needs).
			const std::string hash = manifest.sourceDigest.empty()
				? e.guid : manifest.sourceDigest;
			std::string rerr;
			if (m_cacheDb.Register(klass, e.guid, uri, hash, "", rerr))
				++registered;
			else
				Log("Import: artifact registration skipped for '"
					+ e.file + "': " + rerr);
		}
		Log("Import: registered " + std::to_string(registered)
			+ " derived artifact(s) in the artifact DB.");
	}
	return geometry.generic_string();
}

bool EditorApp::ImportAsset(const std::string& srcPath) {
	if (m_cookRunning) {
		Log("Import: a cook is already running — wait for it to finish.");
		return false;
	}
	// Copy + adopt (no scene switch inside): the asset lands in the
	// project with its UUID, then opens below.
	const std::string dest = ImportAssetFile(srcPath);
	if (dest.empty())
		return false;
	// THE MESH IMPORTER (Unity-style preprocessed assets): the glTF ->
	// <stem>.geometry (the data blob) + <stem>.mat<N>_*.material +
	// <stem>.anim<N_>*.anim + the <stem>.gltf.meta manifest (stable GUIDs)
	// — cameras, materials, lights, geometry, anims: ALL of it rides the
	// artifacts; the session (and the tour it saves) references the
	// .geometry, and the glTF NEVER opens as a scene (import-only, see
	// ImportMeshArtifactFor). A failed import FAILS here — no glTF
	// session fallback.
	const std::string artifact = ImportMeshArtifactFor(dest);
	if (artifact.empty()) {
		Log("Import: could not derive the mesh artifact — aborting "
			"(the copied glTF stays for a fixed re-import).");
		return false;
	}
	if (!OpenScene(artifact)) {
		Log("Import: could not open the derived artifact.");
		return false;
	}
	const std::string stem =
		std::filesystem::path(dest).stem().string();
	// Cook outputs follow the project (PrimeProjectCookDefaults also
	// runs on open, but the import states it explicitly).
	m_cook.outPak = ProjectCookOutPak(stem);
	std::snprintf(m_cookOutPakBuf, sizeof(m_cookOutPakBuf), "%s",
		m_cook.outPak.c_str());
	// Importing ends here: the asset is open with its ENGINE
	// representation ready (GPU world, streamed textures, transcode
	// cache). Cooking the product .pak is the separate next step
	// (Ctrl+Shift+C) — as is Bake GI (on demand).
	Log("Import: '" + stem + "' ready in the editor (cook: Ctrl+Shift+C "
		"-> " + m_cook.outPak + ").");
	return true;
}

// ---- the project-scoped watcher (item 10's editor leg) ---------------
// Arm: one FileWatcher over the project root. The policy is
// ProjectDefault PLUS the editor's own output trees (artifacts/ +
// gi/ — the cooker's bake files land there; watching them would feed
// our own writes back). TOURMALINE_EDITOR_WATCH=0 disables (the
// manual Rescan + re-import paths stand).
void EditorApp::ArmProjectWatcher() {
	m_projectWatcherEnabled = true;
	if (const char* e = std::getenv("TOURMALINE_EDITOR_WATCH"))
		m_projectWatcherEnabled = std::atoi(e) != 0;
	if (!m_projectWatcherEnabled || m_projectRoot.empty()) {
		m_projectWatcher.reset();
		return;
	}
	auto watcher = std::make_unique<smvk::FileWatcher>();
	smvk::FileWatcher::Policy policy =
		smvk::FileWatcher::Policy::ProjectDefault();
	// The editor's own output trees (never watch what we write):
	// artifacts/ (the owned bake dir) + gi/ (the cooker's loose bake
	// files) — feedback loops die here.
	for (const char* d : { "artifacts", "gi" })
		policy.ignoredDirNames.push_back(d);
	std::string err;
	if (!watcher->Start(policy, err)
			|| !watcher->AddDirectory(m_projectRoot, err)) {
		Log("Project watcher: not armed (" + err + ") — the manual "
			"Rescan + re-import paths stand.");
		m_projectWatcher.reset();
		return;
	}
	m_projectWatcher = std::move(watcher);
	Log("Project watcher armed (sources + .meta + .tour; generated "
		"outputs invisible).");
}

// Consume: drain SETTLED changes + route them through the EXISTING
// selective funnels — the frame-boundary call (cheap when nothing
// moved; never a project scan). The routing is per-extension:
//   .gltf/.glb -> ImportMeshArtifactFor (the stale gate re-derives
//                 ONLY when the source changed; the identity ledger
//                 preserves every GUID across the reimport)
//   .meta      -> the asset-db rescan (the identity ledger moved)
//   .tour      -> a LOG ONLY: an external edit never silently
//                 overwrites unsaved editor work (the checklist's
//                 contract) — reload the scene to apply it
//   textures / recipes / anything else -> the log (their live
//                 refresh legs are the item-16 follow-ups)
void EditorApp::ConsumeWatchedProjectChanges() {
	if (!m_projectWatcher || !m_projectWatcher->Running())
		return;
	const std::vector<smvk::FileWatcher::Change> changes =
		m_projectWatcher->Poll(150);
	if (changes.empty())
		return;
	for (const smvk::FileWatcher::Change& c : changes) {
		const std::string name =
			std::filesystem::path(c.path).filename().generic_string();
		const std::string ext =
			std::filesystem::path(c.path).extension().generic_string();
		// A removed source keeps its rows (the registry heals on the
		// next rescan; a log line is enough).
		if (c.event == smvk::FileWatcher::Event::Removed) {
			Log("Watcher: '" + name + "' removed — the next Rescan "
				"reconciles the registry.");
			continue;
		}
		// Identity sidecars (incl. the .gltf.meta ledger): rescan.
		if (ext == ".meta") {
			smvk::cache::RescanStats stats;
			std::string rerr;
			if (m_assetDb.RescanProject(stats, rerr))
				Log("Watcher: identity sidecar '" + name + "' — "
					"rescanned (" + std::to_string(stats.adopted)
					+ " adopted, " + std::to_string(stats.moved)
					+ " re-pointed).");
			else
				Log("Watcher: rescan failed (" + rerr + ").");
			continue;
		}
		// glTF sources: the selective re-import funnel (stale-gated,
		// identity-preserving — ImportMeshArtifactFor's whole
		// contract). The LIVE session keeps rendering the loaded
		// generation; re-open the scene to see the fresh one.
		if (name.size() >= 5
				&& (name.substr(name.size() - 5) == ".gltf"
					|| name.substr(name.size() - 4) == ".glb")) {
			Log("Watcher: glTF source '" + name + "' changed — "
				"re-importing (identity preserved; re-open the "
				"scene to see the fresh geometry).");
			(void)ImportMeshArtifactFor(c.path);
			continue;
		}
		// Authored scenes: NEVER auto-replay (an external edit must
		// not silently overwrite unsaved editor work).
		if (ext == ".tour" || ext == ".tourrecipe") {
			Log("Watcher: '" + name + "' changed on disk — reload it "
				"to apply (external edits never overwrite unsaved "
				"editor work).");
			continue;
		}
		Log("Watcher: '" + name + "' changed on disk (the "
			"development refresh for this kind lands with the "
			"live-update follow-ups).");
	}
}

} // namespace editor
