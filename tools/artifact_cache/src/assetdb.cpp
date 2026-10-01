// Source-asset identity database implementation: GUID sidecars +
// the assets.db directory (SQLiteCpp RAII, same pimpl/WAL pattern as
// CacheDb). See assetdb.h for the layering contract.

#include <artifact_cache/assetdb.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <SQLiteCpp/SQLiteCpp.h>

namespace fs = std::filesystem;

namespace smvk::cache {

namespace {

bool IsHexLower(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Normalized absolute generic path (no existence requirement).
std::string Normalized(const std::string& p) {
	std::error_code ec;
	fs::path abs = fs::absolute(fs::path(p), ec);
	if (ec)
		abs = fs::path(p);
	return abs.lexically_normal().generic_string();
}

// Project-relative posix for in-project files; absolute generic
// otherwise (works, but not portable — the caller prefers project
// files).
std::string ToStoredPath(const std::string& projectRoot,
	const std::string& absPath) {
	if (!projectRoot.empty()
		&& absPath.size() > projectRoot.size()
		&& absPath.compare(0, projectRoot.size(), projectRoot) == 0
		&& absPath[projectRoot.size()] == '/')
		return absPath.substr(projectRoot.size() + 1);
	return absPath;
}

std::string FromStoredPath(const std::string& projectRoot,
	const std::string& stored) {
	if (stored.empty())
		return {};
	if (!stored.empty() && stored[0] == '/')
		return stored; // stored absolute (posix or drive form)
	// Drive-absolute ("F:/...") passes through; relative resolves.
	if (stored.size() > 2 && stored[1] == ':')
		return stored;
	if (projectRoot.empty())
		return stored;
	return projectRoot + "/" + stored;
}

int64_t FileMtime(const fs::path& p) {
	std::error_code ec;
	const auto ft = fs::last_write_time(p, ec);
	if (ec)
		return 0;
	return std::chrono::duration_cast<std::chrono::seconds>(
		ft.time_since_epoch()).count();
}

// Manifest-registered derived assets (glTF derivation; see
// gltf_import/asset_export.h): the <stem>.gltf.meta sidecar carries the
// glTF's own guid PLUS its derived-asset table — "mesh=", "material.N=",
// "anim.N=" lines with "file guid name" values. Parsed here (inline —
// artifact_cache never depends on gltf_import) so the rescan registers
// the derived files into the same table (kind mesh/material/anim).
struct ManifestEntry {
	std::string kind; // "mesh" | "material" | "anim"
	std::string file; // beside the gltf
	std::string guid;
};
void ParseManifestSidecar(const fs::path& metaPath,
	std::vector<ManifestEntry>& out) {
	out.clear();
	std::ifstream in(metaPath, std::ios::binary);
	if (!in)
		return;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = line.substr(0, eq);
		const std::string val = line.substr(eq + 1);
		const char* kind = nullptr;
		if (key == "mesh")
			kind = "mesh";
		else if (key.rfind("material.", 0) == 0)
			kind = "material";
		else if (key.rfind("anim.", 0) == 0)
			kind = "anim";
		if (!kind)
			continue; // guid/source/stem/unknown: not a derived entry
		// "file guid name..." — the name tail (spaces) is display-only.
		std::istringstream is(val);
		ManifestEntry e;
		e.kind = kind;
		if (!(is >> e.file >> e.guid) || !IsValidAssetGuid(e.guid))
			continue;
		out.push_back(std::move(e));
	}
}

// The sidecar's kind= line ("" when the sidecar predates the field):
// bake outputs declare "gi"/"cubemap" here (see
// Engine/Asset/bake_asset.h); source-asset sidecars carry none and keep
// the legacy "model" kind at adoption.
std::string BakeSidecarKind(const std::string& assetPath) {
	std::ifstream in(MetaPathFor(assetPath), std::ios::binary);
	if (!in)
		return {};
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		if (line.substr(0, eq) == "kind")
			return line.substr(eq + 1);
	}
	return {};
}

} // namespace

bool IsValidAssetGuid(const std::string& guid) {
	if (guid.size() != 32)
		return false;
	for (char c : guid)
		if (!IsHexLower(c))
			return false;
	return true;
}

std::string MetaPathFor(const std::string& assetPath) {
	return assetPath + ".meta";
}

bool ReadAssetMeta(const std::string& assetPath, std::string& outGuid,
	std::string& err) {
	outGuid.clear();
	const std::string meta = MetaPathFor(assetPath);
	std::error_code ec;
	if (!fs::is_regular_file(meta, ec)) {
		err = "no .meta";
		return false;
	}
	std::ifstream in(meta, std::ios::binary);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		if (line.substr(0, eq) == "guid") {
			outGuid = line.substr(eq + 1);
			break;
		}
	}
	if (!IsValidAssetGuid(outGuid)) {
		err = "invalid guid '" + outGuid + "' in " + meta;
		outGuid.clear();
		return false;
	}
	return true;
}

bool WriteAssetMeta(const std::string& assetPath, const std::string& guid,
	const std::string& importer, std::string& err) {
	if (!IsValidAssetGuid(guid)) {
		err = "refusing to write invalid guid";
		return false;
	}
	std::ofstream out(MetaPathFor(assetPath),
		std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write " + MetaPathFor(assetPath);
		return false;
	}
	out << "# Tourmaline asset meta (GUID identifies this file — keep "
		"the sidecar with it on move/rename)\n";
	out << "guid=" << guid << "\n";
	out << "importer=" << (importer.empty() ? "model" : importer)
		<< "\n";
	out << "version=1\n";
	return !!out;
}

// ---------------------------------------------------------------------------
// AssetDatabase
// ---------------------------------------------------------------------------

struct AssetDatabase::Impl {
	std::unique_ptr<SQLite::Database> db;
};

namespace {

bool InitAssetSchema(SQLite::Database& db, std::string& err) {
	try {
		SQLite::Statement pragma1(db, "PRAGMA journal_mode=WAL");
		pragma1.executeStep();
		SQLite::Statement pragma2(db, "PRAGMA synchronous=NORMAL");
		pragma2.executeStep();
		const int version = db.exec("PRAGMA user_version");
		if (version < 1) {
			db.exec(
				"CREATE TABLE IF NOT EXISTS assets ("
				"  guid TEXT NOT NULL,"
				"  path TEXT NOT NULL,"
				"  kind TEXT NOT NULL DEFAULT '',"
				"  size INTEGER NOT NULL,"
				"  mtime INTEGER NOT NULL,"
				"  PRIMARY KEY (guid)"
				")");
			db.exec("CREATE INDEX IF NOT EXISTS idx_assets_path "
				"ON assets (path)");
			db.exec("PRAGMA user_version = 1");
		}
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

} // namespace

AssetDatabase::AssetDatabase() = default;

AssetDatabase::~AssetDatabase() {
	Close();
}

bool AssetDatabase::Open(const std::string& scratchRoot,
	const std::string& projectRoot, std::string& err) {
	Close();
	if (scratchRoot.empty() || projectRoot.empty()) {
		err = "asset db needs a scratch root and a project root";
		return false;
	}
	const std::string dbPath =
		(fs::path(scratchRoot) / "assets.db").generic_string();
	std::error_code ec;
	fs::create_directories(fs::path(dbPath).parent_path(), ec);
	try {
		m_impl = new Impl();
		m_impl->db = std::make_unique<SQLite::Database>(dbPath,
			SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE, 3000);
		if (!InitAssetSchema(*m_impl->db, err)) {
			Close();
			return false;
		}
		m_dbPath = dbPath;
		m_projectRoot = Normalized(projectRoot);
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		Close();
		return false;
	}
}

void AssetDatabase::Close() {
	delete m_impl;
	m_impl = nullptr;
	m_dbPath.clear();
	m_projectRoot.clear();
}

bool AssetDatabase::AdoptFile(const std::string& assetPath,
	const std::string& guid, const std::string& kind, std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "asset db not open";
		return false;
	}
	if (!IsValidAssetGuid(guid)) {
		err = "adopting with invalid guid";
		return false;
	}
	std::error_code ec;
	if (!fs::is_regular_file(assetPath, ec)) {
		err = "not a file: " + assetPath;
		return false;
	}
	const std::string abs = Normalized(assetPath);
	const uint64_t size = fs::file_size(assetPath, ec);
	if (ec) {
		err = "cannot stat: " + assetPath;
		return false;
	}
	const int64_t mtime = FileMtime(assetPath);
	try {
		SQLite::Statement q(*m_impl->db,
			"INSERT OR REPLACE INTO assets "
			"(guid, path, kind, size, mtime) VALUES (?, ?, ?, ?, ?)");
		q.bind(1, guid);
		q.bind(2, ToStoredPath(m_projectRoot, abs));
		q.bind(3, kind);
		q.bind(4, static_cast<int64_t>(size));
		q.bind(5, mtime);
		q.exec();
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

bool AssetDatabase::GuidForPath(const std::string& assetPath,
	std::string& outGuid) {
	outGuid.clear();
	if (!m_impl || !m_impl->db)
		return false;
	const std::string abs = Normalized(assetPath);
	try {
		// Prefer the project-relative row; fall back to an absolute
		// row (adopted before the project moved).
		for (int attempt = 0; attempt < 2; ++attempt) {
			const std::string key = (attempt == 0)
				? ToStoredPath(m_projectRoot, abs)
				: abs;
			SQLite::Statement q(*m_impl->db,
				"SELECT guid FROM assets WHERE path = ? LIMIT 1");
			q.bind(1, key);
			if (q.executeStep()) {
				outGuid = q.getColumn(0).getString();
				return IsValidAssetGuid(outGuid);
			}
		}
	}
	catch (const SQLite::Exception&) {
	}
	outGuid.clear();
	return false;
}

bool AssetDatabase::PathForGuid(const std::string& guid,
	std::string& outAbsPath) {
	outAbsPath.clear();
	if (!m_impl || !m_impl->db || !IsValidAssetGuid(guid))
		return false;
	try {
		SQLite::Statement q(*m_impl->db,
			"SELECT path FROM assets WHERE guid = ? LIMIT 1");
		q.bind(1, guid);
		if (!q.executeStep())
			return false;
		const std::string resolved =
			FromStoredPath(m_projectRoot, q.getColumn(0).getString());
		std::error_code ec;
		if (!fs::is_regular_file(resolved, ec))
			return false; // moved/deleted: rescan first, then retry
		outAbsPath = Normalized(resolved);
		return true;
	}
	catch (const SQLite::Exception&) {
	}
	outAbsPath.clear();
	return false;
}

bool AssetDatabase::RescanProject(RescanStats& stats, std::string& err) {
	stats = RescanStats{};
	if (!m_impl || !m_impl->db) {
		err = "asset db not open";
		return false;
	}
	// Snapshot known GUIDs so moves (same guid, new path) update the
	// row instead of duplicating it.
	std::unordered_map<std::string, std::string> known; // guid -> path
	try {
		SQLite::Statement all(*m_impl->db,
			"SELECT guid, path FROM assets");
		while (all.executeStep()) {
			known[all.getColumn(0).getString()] =
				all.getColumn(1).getString();
		}
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
	// Walk the project tree; every sidecar-carrying file (re)registers.
	std::error_code ec;
	for (fs::recursive_directory_iterator it(m_projectRoot, ec), end;
		!ec && it != end; it.increment(ec)) {
		if (ec)
			break;
		const fs::path& p = it->path();
		std::error_code fec;
		if (!it->is_regular_file(fec) || fec)
			continue;
		if (p.extension() == ".meta")
			continue; // sidecars register through their asset
		if (p.filename() == "project.tourproj")
			continue;
		// Skip the owned caches (identity is project content, not
		// scratch/pak outputs). gi/ is NOT skipped: bake outputs carry
		// .meta sidecars there (see Engine/Asset/bake_asset.h) and adopt
		// like any other sidecar-carrying file — files WITHOUT sidecars
		// never reach adoption (see below), so debug PNGs/charts stay out.
		// (scratch/ tolerated: pre-rename projects.)
		const std::string rel = ToStoredPath(m_projectRoot,
			Normalized(p.generic_string()));
		if (rel.compare(0, 8, "scratch/") == 0
			|| rel.compare(0, 9, "artifacts/") == 0
			|| rel.compare(0, 5, "paks/") == 0)
			continue;
		std::string guid, merr;
		if (!ReadAssetMeta(p.generic_string(), guid, merr))
			continue; // no (valid) sidecar: adopted on import/open
		// Sidecar-declared kind wins (bake outputs: "gi"/"cubemap");
		// source sidecars predate the field and keep "model".
		std::string kind = BakeSidecarKind(p.generic_string());
		if (kind.empty())
			kind = "model";
		const auto kt = known.find(guid);
		if (kt == known.end()) {
			std::string aerr;
			if (AdoptFile(p.generic_string(), guid, kind, aerr))
				++stats.adopted;
			known[guid] = rel;
		}
		else if (kt->second != rel) {
			std::string aerr;
			if (AdoptFile(p.generic_string(), guid, kind, aerr))
				++stats.moved;
			kt->second = rel;
		}
		// DERIVED ASSETS (the gltf.meta manifest): the sidecar's
		// mesh/material/anim entries register into the same table (no
		// per-file sidecars — identity is manifest-driven). Kinds
		// "mesh"/"material"/"anim"; moves with the gltf heal here the
		// same way (GUID-stable re-registration).
		{
			std::vector<ManifestEntry> entries;
			ParseManifestSidecar(p, entries);
			for (const ManifestEntry& e : entries) {
				const fs::path derived =
					p.parent_path() / e.file;
				std::error_code dec;
				if (!fs::is_regular_file(derived, dec))
					continue;
				const std::string derivedRel =
					ToStoredPath(m_projectRoot,
						Normalized(derived.generic_string()));
				const auto dt = known.find(e.guid);
				bool registered = false;
				if (dt == known.end()) {
					std::string aerr;
					registered = AdoptFile(derived.generic_string(),
						e.guid, e.kind, aerr);
					if (registered) {
						known[e.guid] = derivedRel;
						++stats.derived;
					}
				}
				else if (dt->second != derivedRel) {
					std::string aerr;
					registered = AdoptFile(derived.generic_string(),
						e.guid, e.kind, aerr);
					if (registered) {
						dt->second = derivedRel;
						++stats.derived;
					}
				}
			}
		}
	}
	if (ec) {
		err = ec.message();
		return false;
	}
	// Count rows whose file is gone (kept — a retarget, not a delete).
	try {
		SQLite::Statement all(*m_impl->db,
			"SELECT path FROM assets");
		while (all.executeStep()) {
			const std::string resolved = FromStoredPath(m_projectRoot,
				all.getColumn(0).getString());
			std::error_code fec;
			if (!fs::is_regular_file(resolved, fec))
				++stats.missing;
		}
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
	return true;
}

bool AssetDatabase::All(std::vector<AssetRecord>& out,
	std::string& err) {
	out.clear();
	if (!m_impl || !m_impl->db) {
		err = "asset db not open";
		return false;
	}
	try {
		SQLite::Statement q(*m_impl->db,
			"SELECT guid, path, kind, size, mtime FROM assets "
			"ORDER BY path");
		while (q.executeStep()) {
			AssetRecord r;
			r.guid = q.getColumn(0).getString();
			r.path = q.getColumn(1).getString();
			r.kind = q.getColumn(2).getString();
			r.size = q.getColumn(3).getInt64();
			r.mtime = q.getColumn(4).getInt64();
			out.push_back(std::move(r));
		}
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

std::string AssetDatabase::ResolvePath(
	const std::string& storedPath) const {
	if (storedPath.empty())
		return {};
	return FromStoredPath(m_projectRoot, storedPath);
}

} // namespace smvk::cache
