// Project abstraction (Unity/Unreal style): a project folder owns
// everything the editor needs for one body of work —
//   <root>/project.tourproj   marker (name + version)
//   <root>/content/            sources (models/<stem>/*.gltf, scenes/*.tour)
//   <root>/paks/               cooked outputs (<stem>.pak)
//   <root>/gi/                 baker sidecars (written by the cooker next
//                             to the out pak; resolved by the loaders)
//   <root>/scratch/            OWNED artifact-cache scratch (the Unity
//                             Library/ equivalent: cache.db + artifacts/)
// Opening a project repoints the scratch, the cook defaults and the
// hosted view's VFS at it. Importing an asset copies it into the
// project, cooks it, and the hosted view shows the result with the
// full render pipeline — no manual staging, no global cache.

#include "editor_app.h"

#include <cctype>
#include <filesystem>
#include <fstream>

namespace editor {
namespace {
constexpr const char* kProjMarker = "project.tourproj";
constexpr const char* kProjVersion = "1";
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

void EditorApp::SetProjectSmoke(const std::string& dir,
	const std::string& asset) {
	m_projectSmoke = true;
	m_projectSmokeDir = dir;
	m_projectSmokeAsset = asset;
}

std::string EditorApp::ProjectScratchDir() const {	if (m_projectRoot.empty())
		return {};
	return (std::filesystem::path(m_projectRoot) / "scratch")
		.generic_string();
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

bool EditorApp::OpenProject(const std::string& dir) {
	const std::string root = Normalized(dir);
	if (!IsProjectDir(root)) {
		Log("Not a project (no " + std::string(kProjMarker) + "): "
			+ root);
		return false;
	}
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
	m_wysiwygRefreshPending = false;
	// Per-scene cook outputs live in the project now; drop any stale
	// default so the next open/import re-derives into project paks/.
	m_cook.outPak.clear();
	m_cookOutPakBuf[0] = '\0';
	m_cookContentDirBuf[0] = '\0';
	// The project OWNS its scratch: reopen the cache there (a missing
	// or unwritable scratch degrades to cache-off, like the global).
	if (!OpenCacheDb())
		Log("Project '" + m_projectName + "' open, but its scratch is "
			"unusable — cache disabled for this session.");
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
	Log("Project open: '" + m_projectName + "' (" + m_projectRoot
		+ ") — scratch, cooks and the hosted view live here now.");
	return true;
}

bool EditorApp::NewProjectOrOpen(const std::string& dir) {
	const std::string root = Normalized(dir);
	namespace fs = std::filesystem;
	std::error_code ec;
	if (IsProjectDir(root))
		return OpenProject(root); // already a project: just open it
	// Scaffold the Unity-style tree (scratch/ itself is created by
	// EnsureScratchRoot when the cache opens).
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
		out << "# Tourmaline project (name + format version)\n";
		out << "name=" << fs::path(root).filename().string() << "\n";
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
	if (smvk::vfs::HasFlintbasePrefix(assetPath))
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

bool EditorApp::ImportAsset(const std::string& srcPath) {
	if (m_projectRoot.empty()) {
		Log("Import: open or create a project first (File > New / Open "
			"Project...) — imports live inside the project.");
		return false;
	}
	if (m_cookRunning) {
		Log("Import: a cook is already running — wait for it to finish.");
		return false;
	}
	if (!HasModelExtension(srcPath)) {
		Log("Import: only .gltf/.glb sources in this version: "
			+ srcPath);
		return false;
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::is_regular_file(srcPath, ec)) {
		Log("Import: source not found: " + srcPath);
		return false;
	}
	const fs::path src(srcPath);
	const std::string stem = src.stem().string();
	const fs::path destDir =
		fs::path(m_projectRoot) / "content" / "models" / stem;
	fs::create_directories(destDir, ec);
	if (ec) {
		Log("Import: cannot create '" + destDir.generic_string()
			+ "': " + ec.message());
		return false;
	}
	// glTF companions (buffers, images, transcoded .ktx2 sidecars)
	// live next to the source by convention: copy the whole sibling
	// set so relative URIs keep resolving. Stale .tour files do NOT
	// come along (they pin the OLD source path; fresh tours are
	// authored in-project). Same-dir import adopts in place.
	const fs::path srcDir = src.parent_path();
	uint64_t copiedBytes = 0;
	size_t copiedFiles = 0;
	if (fs::equivalent(srcDir, destDir, ec) && !ec) {
		Log("Import: source already inside the project — adopting '"
			+ srcPath + "' in place.");
	}
	else {
		for (const auto& entry : fs::directory_iterator(srcDir, ec)) {
			if (ec)
				break;
			if (!entry.is_regular_file(ec) || ec)
				continue;
			const fs::path& p = entry.path();
			std::string ext = p.extension().string();
			for (char& c : ext)
				c = char(std::tolower((unsigned char)c));
			if (ext == ".tour")
				continue; // stale source pin; re-authored in-project
			fs::copy_file(p, destDir / p.filename(),
				fs::copy_options::overwrite_existing, ec);
			if (ec) {
				Log("Import: copy failed '" + p.generic_string()
					+ "': " + ec.message());
				return false;
			}
			++copiedFiles;
			copiedBytes += entry.file_size(ec);
		}
		if (ec) {
			Log("Import: cannot list '" + srcDir.generic_string()
				+ "': " + ec.message());
			return false;
		}
		char buf[128];
		std::snprintf(buf, sizeof(buf), "%zu file(s), %.1f MiB",
			copiedFiles, double(copiedBytes) / (1024.0 * 1024.0));
		Log("Import: '" + src.filename().string() + "' + companions "
			"-> '" + destDir.generic_string() + "' (" + buf + ").");
	}
	const fs::path dest = destDir / src.filename();
	// The copied-along .meta (if any) keeps the asset's GUID.
	AdoptProjectAsset(dest.generic_string());
	if (!OpenScene(dest.generic_string())) {
		Log("Import: could not open the copied asset.");
		return false;
	}
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

} // namespace editor
