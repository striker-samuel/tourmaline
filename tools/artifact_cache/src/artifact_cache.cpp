// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor artifact cache implementation: registry-root handling + the
// SQLite registry (SQLiteCpp RAII over the vendored sqlite3
// amalgamation). See artifact_cache.h for the cache contract.

#include <artifact_cache/artifact_cache.h>
#include <artifact_cache/artifact_kind.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <SQLiteCpp/SQLiteCpp.h>

#include <Engine/Asset/pak.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace smvk::cache {

// ---------------------------------------------------------------------------
// Registry root
// ---------------------------------------------------------------------------

std::string ExistingRegistryRootFor(const std::string& contentDir) {
	if (contentDir.empty())
		return {};
	std::error_code ec;
	const fs::path root =
		(fs::path(contentDir) / ".tourmaline").lexically_normal();
	if (!fs::is_regular_file(root / "cache.db", ec) || ec)
		return {};
	return root.generic_string();
}

std::string EnsureRegistryRoot(const std::string& root) {
	if (root.empty())
		return {};
	std::error_code ec;
	const fs::path p(root);
	fs::create_directories(p, ec);
	if (ec || !fs::is_directory(p, ec))
		return {};
	// Per-class pak store lands here (phase 2).
	fs::create_directories(p / "pak", ec);
#ifdef _WIN32
	// Invisible registry: hidden attribute on the root (children inherit
	// nothing — only the root needs to vanish from casual browsing).
	SetFileAttributesW(p.wstring().c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
	return fs::weakly_canonical(p, ec).generic_string();
}

// ---------------------------------------------------------------------------
// Hash
// ---------------------------------------------------------------------------

std::string HashBytes(const void* data, size_t size) {
	// FNV-1a 64 with a length finish. Deterministic across runs and
	// platforms; local-cache grade by design (see header).
	const auto* bytes = static_cast<const uint8_t*>(data);
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < size; ++i) {
		h ^= bytes[i];
		h *= 1099511628211ull;
	}
	h ^= uint64_t(size) * 0x9E3779B97F4A7C15ull;
	h *= 0xFF51AFD7ED558CCDull;
	h ^= h >> 32;
	char hex[32];
	std::snprintf(hex, sizeof(hex), "%016llx",
		static_cast<unsigned long long>(h));
	return hex;
}

// ---------------------------------------------------------------------------
// CacheDb (SQLite registry)
// ---------------------------------------------------------------------------

struct CacheDb::Impl {
	// pimpl keeps sqlite/SQLiteCpp headers out of the public surface.
	std::unique_ptr<SQLite::Database> db;
};

namespace {

bool InitSchema(SQLite::Database& db, std::string& err) {
	try {
		// WAL: readers (future cook-acceleration lookups) never block
		// the writer, and vice versa. synchronous=NORMAL is the
		// recommended WAL pairing — a crash may lose the last commits
		// but never corrupts (a cache may always be rebuilt).
		SQLite::Statement pragma1(db, "PRAGMA journal_mode=WAL");
		pragma1.executeStep();
		SQLite::Statement pragma2(db, "PRAGMA synchronous=NORMAL");
		pragma2.executeStep();
		// Schema migrations keyed on user_version: bump + ALTER for
		// future changes; v1 is the plain create, v2 adds the identity
		// uuid (the funnel: manifest-registered derivations). NOTE:
		// Database::exec returns the sqlite status (always 0 here), NOT
		// the pragma value — read the version through a SELECT or every
		// ALTER re-runs on every open (v1 survived that via IF NOT
		// EXISTS; ALTER does not).
		auto userVersion = [&]() {
			SQLite::Statement q(db, "PRAGMA user_version");
			q.executeStep();
			return q.getColumn(0).getInt();
		};
		if (userVersion() < 1) {
			db.exec(
				"CREATE TABLE IF NOT EXISTS artifacts ("
				"  hash TEXT NOT NULL,"
				"  klass TEXT NOT NULL,"
				"  uri TEXT NOT NULL,"
				"  pak TEXT NOT NULL,"
				"  offset INTEGER NOT NULL,"
				"  size INTEGER NOT NULL,"
				"  meta TEXT NOT NULL DEFAULT '',"
				"  created INTEGER NOT NULL,"
				"  PRIMARY KEY (klass, uri)"
				")");
			db.exec("CREATE INDEX IF NOT EXISTS idx_artifacts_hash "
				"ON artifacts (hash)");
			db.exec("PRAGMA user_version = 1");
		}
		if (userVersion() < 2) {
			db.exec("ALTER TABLE artifacts ADD COLUMN "
				"uuid TEXT NOT NULL DEFAULT ''");
			db.exec("CREATE INDEX IF NOT EXISTS idx_artifacts_uuid "
				"ON artifacts (klass, uuid)");
			db.exec("PRAGMA user_version = 2");
		}
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

void ReadRow(SQLite::Statement& q, ArtifactRecord& out) {
	out.hash = q.getColumn(0).getString();
	out.klass = q.getColumn(1).getString();
	out.uuid = q.getColumn(2).getString();
	out.uri = q.getColumn(3).getString();
	out.pak = q.getColumn(4).getString();
	out.offset = static_cast<uint64_t>(q.getColumn(5).getInt64());
	out.size = static_cast<uint64_t>(q.getColumn(6).getInt64());
	out.meta = q.getColumn(7).getString();
	out.created = q.getColumn(8).getInt64();
}

} // namespace

CacheDb::CacheDb() = default;

CacheDb::~CacheDb() {
	Close();
}

bool CacheDb::Open(const std::string& path, std::string& err) {
	Close();
	if (path.empty()) {
		err = "empty db path";
		return false;
	}
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	try {
		m_impl = new Impl();
		// CREATE opens rw + creates when missing; busy timeout guards
		// a second editor instance touching the same registry.
		m_impl->db = std::make_unique<SQLite::Database>(path,
			SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE, 3000);
		if (!InitSchema(*m_impl->db, err)) {
			Close();
			return false;
		}
		m_path = path;
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		Close();
		return false;
	}
}

void CacheDb::Close() {
	delete m_impl;
	m_impl = nullptr;
	m_path.clear();
}

bool CacheDb::Put(const ArtifactRecord& rec, std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "db not open";
		return false;
	}
	if (rec.klass.empty() || rec.uri.empty() || rec.hash.empty()) {
		err = "record needs klass, uri and hash";
		return false;
	}
	if (!IsValidArtifactKind(rec.klass)) {
		err = "unknown artifact class: " + rec.klass
			+ " (see artifact_kind.h)";
		return false;
	}
	const int64_t createdTs = (rec.created != 0) ? rec.created
		: std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	try {
		SQLite::Statement q(*m_impl->db,
			"INSERT INTO artifacts "
			"(hash, klass, uuid, uri, pak, offset, size, meta, created) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
			"ON CONFLICT (klass, uri) DO UPDATE SET "
			"  hash = excluded.hash, pak = excluded.pak, "
			"  offset = excluded.offset, size = excluded.size, "
			"  meta = excluded.meta, created = excluded.created, "
			// A payload write without identity must not clobber a
			// registered uuid (Flush after Register keeps it).
			"  uuid = CASE WHEN excluded.uuid <> '' "
			"    THEN excluded.uuid ELSE artifacts.uuid END");
		q.bind(1, rec.hash);
		q.bind(2, rec.klass);
		q.bind(3, rec.uuid);
		q.bind(4, rec.uri);
		q.bind(5, rec.pak);
		q.bind(6, static_cast<int64_t>(rec.offset));
		q.bind(7, static_cast<int64_t>(rec.size));
		q.bind(8, rec.meta);
		q.bind(9, createdTs);
		q.exec();
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

bool CacheDb::Get(const std::string& klass, const std::string& uri,
	ArtifactRecord& out, std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "db not open";
		return false;
	}
	try {
		SQLite::Statement q(*m_impl->db,
			"SELECT hash, klass, uuid, uri, pak, offset, size, meta, created "
			"FROM artifacts WHERE klass = ? AND uri = ?");
		q.bind(1, klass);
		q.bind(2, uri);
		if (q.executeStep()) {
			ReadRow(q, out);
			return true;
		}
		return false;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

bool CacheDb::GetByHash(const std::string& hash, ArtifactRecord& out,
	std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "db not open";
		return false;
	}
	try {
		SQLite::Statement q(*m_impl->db,
			"SELECT hash, klass, uuid, uri, pak, offset, size, meta, created "
			"FROM artifacts WHERE hash = ? LIMIT 1");
		q.bind(1, hash);
		if (q.executeStep()) {
			ReadRow(q, out);
			return true;
		}
		return false;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

bool CacheDb::FindByUuid(const std::string& klass, const std::string& uuid,
	ArtifactRecord& out, std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "db not open";
		return false;
	}
	if (klass.empty() || uuid.empty()) {
		err = "identity lookup needs klass and uuid";
		return false;
	}
	try {
		SQLite::Statement q(*m_impl->db,
			"SELECT hash, klass, uuid, uri, pak, offset, size, meta, created "
			"FROM artifacts WHERE klass = ? AND uuid = ? LIMIT 1");
		q.bind(1, klass);
		q.bind(2, uuid);
		if (q.executeStep()) {
			ReadRow(q, out);
			return true;
		}
		return false;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

bool CacheDb::Register(const std::string& klass, const std::string& uuid,
	const std::string& uri, const std::string& hash, const std::string& meta,
	std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "db not open";
		return false;
	}
	if (klass.empty() || uuid.empty() || uri.empty() || hash.empty()) {
		err = "registration needs klass, uuid, uri and hash";
		return false;
	}
	if (!IsValidArtifactKind(klass)) {
		err = "unknown artifact class: " + klass
			+ " (see artifact_kind.h)";
		return false;
	}
	const int64_t createdTs =
		std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	try {
		// Identity upsert: hash/uuid/meta refresh, but a staged
		// payload's location columns are NEVER clobbered (a Register
		// after a Flush must not orphan bytes). Fresh rows carry an
		// empty location (identity-only until a payload stages).
		SQLite::Statement q(*m_impl->db,
			"INSERT INTO artifacts "
			"(hash, klass, uuid, uri, pak, offset, size, meta, created) "
			"VALUES (?, ?, ?, ?, '', 0, 0, ?, ?) "
			"ON CONFLICT (klass, uri) DO UPDATE SET "
			"  hash = excluded.hash, uuid = excluded.uuid, "
			"  meta = excluded.meta, created = excluded.created");
		q.bind(1, hash);
		q.bind(2, klass);
		q.bind(3, uuid);
		q.bind(4, uri);
		q.bind(5, meta);
		q.bind(6, createdTs);
		q.exec();
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

bool CacheDb::Remove(const std::string& klass, const std::string& uri,
	std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "db not open";
		return false;
	}
	try {
		SQLite::Statement q(*m_impl->db,
			"DELETE FROM artifacts WHERE klass = ? AND uri = ?");
		q.bind(1, klass);
		q.bind(2, uri);
		q.exec();
		return true;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return false;
	}
}

std::vector<ArtifactRecord> CacheDb::All(std::string& err) {
	std::vector<ArtifactRecord> out;
	if (!m_impl || !m_impl->db) {
		err = "db not open";
		return out;
	}
	try {
		SQLite::Statement q(*m_impl->db,
			"SELECT hash, klass, uuid, uri, pak, offset, size, meta, created "
			"FROM artifacts ORDER BY klass, uri");
		while (q.executeStep()) {
			ArtifactRecord r;
			ReadRow(q, r);
			out.push_back(std::move(r));
		}
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		out.clear();
	}
	return out;
}

uint64_t CacheDb::Count(std::string& err) {
	if (!m_impl || !m_impl->db) {
		err = "db not open";
		return 0;
	}
	try {
		SQLite::Statement q(*m_impl->db, "SELECT COUNT(*) FROM artifacts");
		if (q.executeStep())
			return static_cast<uint64_t>(q.getColumn(0).getInt64());
		return 0;
	}
	catch (const SQLite::Exception& e) {
		err = e.what();
		return 0;
	}
}

// ---------------------------------------------------------------------------
// ArtifactStore
// ---------------------------------------------------------------------------

namespace {

} // namespace

ArtifactStore::ArtifactStore(std::string registryRoot, CacheDb& db)
	: m_root(std::move(registryRoot))
	, m_db(db) {
}

ArtifactStore::~ArtifactStore() = default;

std::string ArtifactStore::PakPath(const std::string& klass) const {
	return (fs::path(m_root) / ArtifactPakFileName(klass)).generic_string();
}

bool ArtifactStore::Put(const std::string& klass, const std::string& uri,
	std::span<const uint8_t> payload, const std::string& sourceHash,
	const std::string& meta, std::string& err, const std::string& uuid) {
	smvk::PakAssetKind kind = smvk::PakAssetKind::Texture;
	if (!ArtifactKindToPakKind(klass, kind)) {
		err = "unknown artifact class: " + klass
			+ " (see artifact_kind.h)";
		return false;
	}
	(void)kind; // kind matters at Flush (per-class pak), not at stage
	if (uri.empty() || payload.empty() || sourceHash.empty()) {
		err = "put needs uri, payload and sourceHash";
		return false;
	}
	// Same-uri restage replaces the pending entry (latest wins, the
	// registry precedent).
	for (auto& s : m_staged) {
		if (s.klass == klass && s.uri == uri) {
			s.payload.assign(payload.begin(), payload.end());
			s.sourceHash = sourceHash;
			s.meta = meta;
			if (!uuid.empty())
				s.uuid = uuid;
			return true;
		}
	}
	Staged s;
	s.klass = klass;
	s.uri = uri;
	s.payload.assign(payload.begin(), payload.end());
	s.sourceHash = sourceHash;
	s.meta = meta;
	s.uuid = uuid;
	m_staged.push_back(std::move(s));
	return true;
}

bool ArtifactStore::Get(const std::string& klass, const std::string& uri,
	std::vector<uint8_t>& out, std::string& err) {
	// Staged (not yet flushed) bytes win — write-behind coherence.
	for (const auto& s : m_staged) {
		if (s.klass == klass && s.uri == uri) {
			out = s.payload;
			return true;
		}
	}
	return GetFromDisk(klass, uri, out, err);
}

bool ArtifactStore::GetFromDisk(const std::string& klass,
	const std::string& uri, std::vector<uint8_t>& out,
	std::string& err) const {
	ArtifactRecord rec;
	std::string gerr;
	if (!m_db.Get(klass, uri, rec, gerr)) {
		err = "no artifact '" + uri + "' (class " + klass + ")";
		return false;
	}
	if (rec.size == 0 || rec.pak.empty()) {
		err = "no payload for '" + uri + "' (identity-only row: "
			"the derivation lives loose, nothing staged)";
		return false;
	}
	std::ifstream f(PakPath(klass), std::ios::binary);
	if (!f) {
		err = "class pak missing: " + klass;
		return false;
	}
	f.seekg(std::streamoff(rec.offset), std::ios::beg);
	out.resize(size_t(rec.size));
	f.read(reinterpret_cast<char*>(out.data()),
		std::streamsize(out.size()));
	if (f.gcount() != std::streamsize(out.size())) {
		err = "artifact read truncated: " + uri;
		out.clear();
		return false;
	}
	return true;
}

bool ArtifactStore::Has(const std::string& klass, const std::string& uri,
	const std::string& sourceHash) const {
	for (const auto& s : m_staged) {
		if (s.klass == klass && s.uri == uri)
			return s.sourceHash == sourceHash;
	}
	ArtifactRecord rec;
	std::string err;
	return m_db.Get(klass, uri, rec, err) && rec.hash == sourceHash;
}

bool ArtifactStore::Flush(std::string& err) {
	if (m_staged.empty())
		return true;
	// Group by class (order preserved within a class).
	std::vector<std::string> classes;
	for (const auto& s : m_staged)
		if (std::find(classes.begin(), classes.end(), s.klass)
			== classes.end())
			classes.push_back(s.klass);
	for (const std::string& klass : classes) {
		smvk::PakAssetKind kind{};
		if (!ArtifactKindToPakKind(klass, kind)) {
			err = "unknown artifact class: " + klass
				+ " (see artifact_kind.h)";
			return false;
		}
		std::vector<std::pair<std::string, std::span<const uint8_t>>>
			blobs;
		std::vector<const Staged*> batch;
		for (const auto& s : m_staged) {
			if (s.klass != klass)
				continue;
			blobs.emplace_back(s.uri,
				std::span<const uint8_t>(s.payload));
			batch.push_back(&s);
		}
		std::vector<smvk::PakAppendInfo> infos;
		if (!smvk::AppendPakBlobs(PakPath(klass), kind, blobs, infos,
				err))
			return false; // staged stays intact for a retry
		// Commit the registry rows (after the pak grew: a crash in
		// between leaves dead payload, never a dangling row).
		for (size_t i = 0; i < batch.size(); ++i) {
			ArtifactRecord rec;
			rec.hash = batch[i]->sourceHash;
			rec.klass = klass;
			rec.uuid = batch[i]->uuid;
			rec.uri = batch[i]->uri;
			rec.pak = ArtifactPakFileName(klass);
			rec.offset = infos[i].offset;
			rec.size = infos[i].size;
			rec.meta = batch[i]->meta;
			if (!m_db.Put(rec, err))
				return false;
		}
	}
	// Every staged klass was in `classes`: nothing can be left.
	m_staged.clear();
	return true;
}

bool ArtifactStore::Remove(const std::string& klass, const std::string& uri,
	std::string& err) {
	// Staged removal too (an unflushed Put can be un-Put).
	for (size_t i = 0; i < m_staged.size(); ++i)
		if (m_staged[i].klass == klass && m_staged[i].uri == uri) {
			m_staged.erase(m_staged.begin() + std::streamoff(i));
			break;
		}
	return m_db.Remove(klass, uri, err);
}

bool ArtifactStore::ResetClass(const std::string& klass, std::string& err) {
	smvk::PakAssetKind kind{};
	if (!ArtifactKindToPakKind(klass, kind)) {
		err = "unknown artifact class: " + klass
			+ " (see artifact_kind.h)";
		return false;
	}
	m_staged.erase(std::remove_if(m_staged.begin(), m_staged.end(),
		[&](const Staged& s) { return s.klass == klass; }),
		m_staged.end());
	std::error_code ec;
	fs::remove(fs::path(PakPath(klass)), ec);
	// Fresh error channels: a stale err from an earlier miss must not
	// read as a failure here.
	std::string aerr;
	auto rows = m_db.All(aerr);
	if (!aerr.empty()) {
		err = aerr;
		return false;
	}
	bool ok = true;
	for (const ArtifactRecord& r : rows) {
		if (r.klass != klass)
			continue;
		std::string rerr;
		ok = m_db.Remove(r.klass, r.uri, rerr) && ok;
	}
	return ok;
}

bool ArtifactStore::ReadPakImage(const std::string& klass,
	std::vector<uint8_t>& out, std::string& err) const {
	const std::string path = PakPath(klass);
	std::error_code ec;
	if (!fs::exists(fs::path(path), ec)) {
		err = "no class pak: " + klass;
		return false;
	}
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		err = "cannot open class pak: " + path;
		return false;
	}
	const auto size = f.tellg();
	f.seekg(0);
	out.resize(size_t(size));
	if (size > 0)
		f.read(reinterpret_cast<char*>(out.data()), size);
	return f.good() || size == 0;
}

} // namespace smvk::cache
