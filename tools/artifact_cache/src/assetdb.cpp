// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Source-asset identity database implementation: GUID sidecars +
// the assets.db directory (SQLiteCpp RAII, same pimpl/WAL pattern as
// CacheDb). See assetdb.h for the layering contract.

#include <artifact_cache/assetdb.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
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
		// "geometry" is the post-10/6 rename of the derived data blob
		// (".mesh" -> ".geometry"); BOTH keys are the same asset class and
		// must register under the manifest guid — the parser skipping the
		// renamed key made the DB adopt the blob under a FRESH guid while
		// tours/AddSceneReference recorded the MANIFEST guid, so the
		// guid-first reference resolution never matched (Add to scene
		// broken, references skipped after a reimport).
		if (key == "mesh" || key == "geometry")
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
// Engine/Asset/bake_asset.h); cook assets declare "gicook"/"lmcook"
// through their importer (see Engine/Asset/cook_asset.h); source-asset
// sidecars carry importer=model and keep the legacy "model" kind at
// adoption. The importer fallback keeps cook assets collection-addressable
// through rescans (WriteAssetMeta only writes importer=, never kind=).
std::string BakeSidecarKind(const std::string& assetPath) {
	std::ifstream in(MetaPathFor(assetPath), std::ios::binary);
	if (!in)
		return {};
	std::string kind, importer;
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
			kind = line.substr(eq + 1);
		else if (line.substr(0, eq) == "importer")
			importer = line.substr(eq + 1);
	}
	if (!kind.empty())
		return kind;
	// Cook-asset importers double as kinds; everything else predates the
	// field ("model", "bake", ...) — "bake" is the bake-asset writer's
	// importer tag on files that always carry an explicit kind=, so it
	// never reaches here with meaning; pass it through harmlessly.
	if (importer == "gicook" || importer == "lmcook")
		return importer;
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

// Full-sidecar write with explicit plug rows (the anonymous namespace
// below defines it; forward-declared here for the preserve-rewrite).
namespace {
bool WriteAssetMetaWithPlugs(const std::string& assetPath,
	const std::string& guid, const std::string& importer,
	const std::vector<AssetPlug>& plugs, std::string& err);
} // namespace

bool WriteAssetMeta(const std::string& assetPath, const std::string& guid,
	const std::string& importer, std::string& err) {
	// Preserve authored INPUT PLUG rows (see ReadAssetPlugs): this rewrite
	// is the adoption/re-adoption path (the canonical guid/importer header),
	// and dropping the plugs would silently discard authored import config.
	// Defined after the plug helpers below (file-order declaration).
	return WriteAssetMetaWithPlugs(assetPath, guid, importer,
		ReadAssetPlugs(assetPath), err);
}

// ---------------------------------------------------------------------------
// Input plugs (see the header's block): index-paired
// "plug.<kind>.<n>.old/new" rows. Pairing is by index within a kind; the
// rows are written in (kind, index) order.
// ---------------------------------------------------------------------------
namespace {
// Parses one plug key "plug.<kind>.<n>.<half>"; false when not a plug key.
bool ParsePlugKey(const std::string& key, std::string& kind, size_t& idx,
	std::string& half) {
	if (key.rfind("plug.", 0) != 0)
		return false;
	const size_t k1 = key.find('.', 5); // after "plug."
	if (k1 == std::string::npos)
		return false;
	kind = key.substr(5, k1 - 5);
	const size_t k2 = key.rfind('.');
	if (k2 <= k1)
		return false;
	const std::string idxStr = key.substr(k1 + 1, k2 - k1 - 1);
	half = key.substr(k2 + 1);
	if (kind.empty() || idxStr.empty()
		|| idxStr.find_first_not_of("0123456789") != std::string::npos
		|| (half != "old" && half != "new"))
		return false;
	idx = std::stoull(idxStr);
	return true;
}

// Plug rows in (kind, index) order (grouping by kind keeps N stable per
// kind, so hand-edited interleavings rewrite canonically).
void WritePlugRows(std::ostream& out,
	const std::vector<AssetPlug>& plugs) {
	std::map<std::string, std::vector<const AssetPlug*>> byKind;
	for (const AssetPlug& p : plugs) {
		if (p.kind.empty() || p.oldUri.empty() || p.newPath.empty())
			continue; // incomplete row: dropped (never silently half-written)
		byKind[p.kind].push_back(&p);
	}
	for (const auto& [kind, rows] : byKind) {
		for (size_t i = 0; i < rows.size(); ++i) {
			out << "plug." << kind << "." << i << ".old="
				<< rows[i]->oldUri << "\n";
			out << "plug." << kind << "." << i << ".new="
				<< rows[i]->newPath << "\n";
		}
	}
}

// Full sidecar write with EXPLICIT plug rows (the shared tail of
// WriteAssetMeta's preserve-rewrite and WriteAssetPlugs' replace-rewrite).
bool WriteAssetMetaWithPlugs(const std::string& assetPath,
	const std::string& guid, const std::string& importer,
	const std::vector<AssetPlug>& plugs, std::string& err) {
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
	WritePlugRows(out, plugs);
	return !!out;
}
} // namespace

std::vector<AssetPlug> ReadAssetPlugs(const std::string& assetPath) {
	std::vector<AssetPlug> out;
	std::ifstream in(MetaPathFor(assetPath), std::ios::binary);
	if (!in)
		return out;
	// (kind, index) -> {old, new} halves; paired at the end.
	std::map<std::pair<std::string, size_t>,
		std::pair<std::string, std::string>> halves;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string kind, half;
		size_t idx = 0;
		if (!ParsePlugKey(line.substr(0, eq), kind, idx, half))
			continue;
		auto& pair = halves[{ kind, idx }];
		if (half == "old")
			pair.first = line.substr(eq + 1);
		else
			pair.second = line.substr(eq + 1);
	}
	for (const auto& [key, pair] : halves) {
		if (pair.first.empty() || pair.second.empty())
			continue; // unpaired (hand edit): skip
		AssetPlug p;
		p.kind = key.first;
		p.oldUri = pair.first;
		p.newPath = pair.second;
		out.push_back(std::move(p));
	}
	return out;
}

bool WriteAssetPlugs(const std::string& assetPath, const std::string& kind,
	const std::vector<AssetPlug>& plugs, std::string& err) {
	if (kind.empty()) {
		err = "refusing to write plug rows without a kind";
		return false;
	}
	for (const AssetPlug& p : plugs)
		if (!p.oldUri.empty() && p.newPath.empty()) {
			err = "refusing to write a half plug row (old without new)";
			return false;
		}
	// Identity gate: the sidecar must carry a valid guid (adopt the asset
	// first) — plug rows without identity would orphan on the next rewrite.
	std::string guid;
	if (!ReadAssetMeta(assetPath, guid, err))
		return false;
	std::string importer;
	{
		std::ifstream in(MetaPathFor(assetPath), std::ios::binary);
		std::string line;
		while (in && std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			if (line.substr(0, eq) == "importer")
				importer = line.substr(eq + 1);
		}
	}
	// Keep every OTHER kind's rows verbatim; REPLACE this kind's set with
	// the given rows (empty = clear the kind).
	std::vector<AssetPlug> combined;
	for (const AssetPlug& p : ReadAssetPlugs(assetPath))
		if (p.kind != kind)
			combined.push_back(p);
	for (const AssetPlug& p : plugs)
		if (!p.oldUri.empty() && !p.newPath.empty()) {
			AssetPlug stamped = p;
			stamped.kind = kind; // caller passes kind-scoped rows
			combined.push_back(std::move(stamped));
		}
	return WriteAssetMetaWithPlugs(assetPath, guid,
		importer.empty() ? "model" : importer, combined, err);
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
		}
		if (version < 2) {
			// The persistent CONTENT-ID registry (the generated
			// project.h's stable identities — one and the same
			// database, the owner's call; see assetdb.h's block).
			// Tombstones keep their rows: the id+symbol stay reserved,
			// active=0 drops them from the generated catalogs.
			db.exec(
				"CREATE TABLE IF NOT EXISTS content_ids ("
				"  guid TEXT NOT NULL PRIMARY KEY,"
				"  id INTEGER NOT NULL UNIQUE,"
				"  symbol TEXT NOT NULL,"
				"  kind TEXT NOT NULL,"
				"  path TEXT NOT NULL DEFAULT '',"
				"  active INTEGER NOT NULL DEFAULT 1"
				")");
			db.exec("CREATE INDEX IF NOT EXISTS idx_content_ids_kind "
				"ON content_ids (kind)");
		}
		db.exec("PRAGMA user_version = 2");
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

bool AssetDatabase::Open(const std::string& registryRoot,
	const std::string& projectRoot, std::string& err) {
	Close();
	if (registryRoot.empty() || projectRoot.empty()) {
		err = "asset db needs a registry root and a project root";
		return false;
	}
	const std::string dbPath =
		(fs::path(registryRoot) / "assets.db").generic_string();
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
		// CONDITIONAL upsert: the row is written ONLY when the recorded
		// metadata actually changes. The ids generation runs on every
		// build and adopts every participating asset — an unconditional
		// INSERT OR REPLACE churned the whole table each time (SQLite
		// writes + WAL growth for byte-identical rows). ON CONFLICT DO
		// UPDATE ... WHERE <changed> skips the write entirely when the
		// path/kind/size/mtime all stand.
		SQLite::Statement q(*m_impl->db,
			"INSERT INTO assets (guid, path, kind, size, mtime) "
			"VALUES (?, ?, ?, ?, ?) "
			"ON CONFLICT(guid) DO UPDATE SET "
			"  path = excluded.path, kind = excluded.kind, "
			"  size = excluded.size, mtime = excluded.mtime "
			"WHERE path <> excluded.path OR kind <> excluded.kind "
			"   OR size <> excluded.size OR mtime <> excluded.mtime");
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
		// (scratch/ + artifacts/ tolerated: pre-schema-rename projects,
		// ignored in place — caches rebuild under .tourmaline/.)
		const std::string rel = ToStoredPath(m_projectRoot,
			Normalized(p.generic_string()));
		if (rel.compare(0, 8, "scratch/") == 0
			|| rel.compare(0, 9, "artifacts/") == 0
			|| rel.compare(0, 12, ".tourmaline/") == 0
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
			// THE SIDECAR, not the asset itself: this call passed the
			// walked .gltf straight through, so the parser read glTF
			// JSON as a manifest - zero entries ever parsed, zero
			// derived guids ever registered, and the importer's own
			// adoption minted FRESH guids instead (manifest guid vs DB
			// guid mismatch: tours/AddSceneReference record the manifest
			// guid, the loader resolves through the DB - never matched,
			// references skipped, Add to scene broken).
			ParseManifestSidecar(MetaPathFor(p.generic_string()), entries);
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

// ---- the content-id registry ---------------------------------------------

namespace {

// The kind's id range (the generated header's namespaces; see
// assetdb.h). Allocation walks to the range's next free slot and NEVER
// reuses (tombstones hold theirs forever).
bool IdRangeForKind(const std::string& kind, uint64_t& base,
	uint64_t& last) {
	if (kind == "scene") {
		base = 0x1000;
	}
	else if (kind == "texture") {
		base = 0x2000;
	}
	else if (kind == "mesh") {
		base = 0x3000;
	}
	else
		return false;
	last = base + 0xFFF;
	return true;
}

// A valid C++ identifier from the path's stem, in three passes:
//   (a) sanitize: [A-Za-z0-9_], invalid chars fold to '_', a leading
//       digit takes an '_' prefix;
//   (b) RESERVED normalization: any identifier CONTAINING '__' is
//       reserved ([lex.name]) — every '__' folds to '_u_' until none
//       remain (__stone -> _u_stone, ___x -> _u_u_x); a LEADING '_'
//       followed by an uppercase letter is reserved too — an 'u'
//       inserts after the underscore (_Stone -> _uStone);
//   (c) C++20 KEYWORDS (the COMPLETE set, incl. the digraph operators
//       and the charN_t/co_* tokens the first cut missed — compl.png
//       and bitand.png must never emit textures::compl) take an '_'
//       prefix.
// The stem at FIRST registration only — renames later keep the
// assigned symbol.
std::string SymbolFromPath(const std::string& projectRelPath) {
	static const char* kKeywords[] = {
		"alignas", "alignof", "and", "and_eq", "asm", "auto",
		"bitand", "bitor", "bool", "break", "case", "catch", "char",
		"char8_t", "char16_t", "char32_t", "class", "compl", "concept",
		"const", "consteval", "constexpr", "constinit", "const_cast",
		"continue", "co_await", "co_return", "co_yield", "decltype",
		"default", "delete", "do", "double", "dynamic_cast", "else",
		"enum", "explicit", "export", "extern", "false", "float", "for",
		"friend", "goto", "if", "inline", "int", "long", "mutable",
		"namespace", "new", "noexcept", "not", "not_eq", "nullptr",
		"operator", "or", "or_eq", "private", "protected", "public",
		"register", "reinterpret_cast", "requires", "return", "short",
		"signed", "sizeof", "static", "static_assert", "static_cast",
		"struct", "switch", "template", "this", "thread_local",
		"throw", "true", "try", "typedef", "typeid", "typename",
		"union", "unsigned", "using", "virtual", "void", "volatile",
		"wchar_t", "while", "xor", "xor_eq",
	};
	std::string stem = fs::path(projectRelPath).stem().generic_string();
	std::string sym;
	for (char c : stem) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9') || c == '_';
		sym += ok ? c : '_';
	}
	if (sym.empty())
		sym = "_";
	if (sym[0] >= '0' && sym[0] <= '9')
		sym = "_" + sym;
	// (b) reserved '__' anywhere: fold to '_u_' until none remain (the
	// fold itself can never create a new '__').
	for (size_t at = sym.find("__"); at != std::string::npos;
			at = sym.find("__"))
		sym.replace(at, 2, "_u_");
	// (b) reserved leading '_' + uppercase: insert 'u' after the '_'.
	if (sym.size() >= 2 && sym[0] == '_'
			&& sym[1] >= 'A' && sym[1] <= 'Z')
		sym.insert(1, "u");
	// (c) keywords take an '_' prefix.
	for (const char* kw : kKeywords)
		if (sym == kw) {
			sym = "_" + sym;
			break;
		}
	return sym;
}

} // namespace

bool AssetDatabase::EnsureContentId(const std::string& guid,
	const std::string& kind, const std::string& projectRelPath,
	uint64_t& outId, std::string& outSymbol, std::string& err) {
	outId = 0;
	outSymbol.clear();
	if (!m_impl || !m_impl->db) {
		err = "asset db not open";
		return false;
	}
	if (!IsValidAssetGuid(guid)) {
		err = "content id needs a valid asset guid";
		return false;
	}
	uint64_t base = 0, last = 0;
	if (!IdRangeForKind(kind, base, last)) {
		err = "unknown content kind '" + kind + "' (scene|texture|mesh)";
		return false;
	}
	try {
		// Existing row: return its id+symbol verbatim (a tombstone
		// re-activates; the symbol NEVER re-derives — renames keep it).
		// The stored KIND must match the request: one GUID identifying
		// two kinds is an IDENTITY INVARIANT violation (a scene and a
		// mesh sharing a GUID) — it fails loudly, never silently
		// cross-namespaces.
		{
			SQLite::Statement q(*m_impl->db,
				"SELECT id, symbol, kind, path, active "
				"FROM content_ids WHERE guid = ?");
			q.bind(1, guid);
			if (q.executeStep()) {
				if (q.getColumn(2).getString() != kind) {
					err = "guid " + guid + " is registered as a '"
						+ q.getColumn(2).getString()
						+ "' but requested as a '" + kind
						+ "' — an identity inconsistency (the asset"
							" and its sidecar disagree); fix the"
							" sidecar, never the registry";
					return false;
				}
				outId = uint64_t(q.getColumn(0).getInt64());
				outSymbol = q.getColumn(1).getString();
				const std::string storedPath =
					q.getColumn(3).getString();
				const bool active = q.getColumn(4).getInt() != 0;
				if (!active) {
					SQLite::Statement u(*m_impl->db,
						"UPDATE content_ids SET active = 1, path = ? "
						"WHERE guid = ?");
					u.bind(1, projectRelPath);
					u.bind(2, guid);
					u.exec();
				}
				else if (storedPath != projectRelPath) {
					// path refresh (informational only)
					SQLite::Statement u(*m_impl->db,
						"UPDATE content_ids SET path = ? WHERE guid = ?");
					u.bind(1, projectRelPath);
					u.bind(2, guid);
					u.exec();
				}
				return true;
			}
		}
		// Fresh allocation: the kind range's next free id (MAX+1 —
		// tombstones hold theirs, so no reuse ever).
		uint64_t next = base;
		{
			SQLite::Statement q(*m_impl->db,
				"SELECT MAX(id) FROM content_ids WHERE id >= ? AND id <= ?");
			q.bind(1, static_cast<int64_t>(base));
			q.bind(2, static_cast<int64_t>(last));
			if (q.executeStep() && !q.isColumnNull(0))
				next = uint64_t(q.getColumn(0).getInt64()) + 1;
		}
		if (next > last) {
			err = "the '" + kind + "' id range is exhausted (delete"
				" tombstones you truly no longer need)";
			return false;
		}
		// The symbol: the path stem, collision-suffixed within the kind
		// (stable once assigned — the suffix stays through renames).
		const std::string want = SymbolFromPath(projectRelPath);
		std::string symbol = want;
		for (uint32_t attempt = 2;; ++attempt) {
			SQLite::Statement q(*m_impl->db,
				"SELECT 1 FROM content_ids WHERE kind = ? AND symbol = ?");
			q.bind(1, kind);
			q.bind(2, symbol);
			if (!q.executeStep())
				break; // free
			if (attempt > 99) {
				err = "symbol collision exhaustion for '" + want + "'";
				return false;
			}
			symbol = want + "_" + std::to_string(attempt);
		}
		SQLite::Statement q(*m_impl->db,
			"INSERT INTO content_ids (guid, id, symbol, kind, path,"
			" active) VALUES (?, ?, ?, ?, ?, 1)");
		q.bind(1, guid);
		q.bind(2, static_cast<int64_t>(next));
		q.bind(3, symbol);
		q.bind(4, kind);
		q.bind(5, projectRelPath);
		q.exec();
		outId = next;
		outSymbol = symbol;
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

bool AssetDatabase::ListContentIds(std::vector<ContentIdRecord>& out,
	std::string& err) {
	out.clear();
	if (!m_impl || !m_impl->db) {
		err = "asset db not open";
		return false;
	}
	try {
		SQLite::Statement q(*m_impl->db,
			"SELECT guid, id, symbol, kind, path, active "
			"FROM content_ids ORDER BY id");
		while (q.executeStep()) {
			ContentIdRecord r;
			r.guid = q.getColumn(0).getString();
			r.id = uint64_t(q.getColumn(1).getInt64());
			r.symbol = q.getColumn(2).getString();
			r.kind = q.getColumn(3).getString();
			r.path = q.getColumn(4).getString();
			r.active = q.getColumn(5).getInt() != 0;
			out.push_back(std::move(r));
		}
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

bool AssetDatabase::SetContentIdActive(const std::string& guid,
	bool active, std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "asset db not open";
		return false;
	}
	try {
		SQLite::Statement q(*m_impl->db,
			"UPDATE content_ids SET active = ? WHERE guid = ?");
		q.bind(1, active ? 1 : 0);
		q.bind(2, guid);
		q.exec();
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

} // namespace smvk::cache
