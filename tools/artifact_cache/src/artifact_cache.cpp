// Editor artifact cache implementation: scratch-root handling + the
// SQLite registry (SQLiteCpp RAII over the vendored sqlite3
// amalgamation). See artifact_cache.h for the cache contract.

#include <artifact_cache/artifact_cache.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include <SQLiteCpp/SQLiteCpp.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace smvk::cache {

// ---------------------------------------------------------------------------
// Scratch root
// ---------------------------------------------------------------------------

std::string DefaultScratchRoot() {
	// Preferred default: F:\ (keep C: from burning). Falls back to the
	// per-user app-data cache when the machine has no writable F:.
	{
		std::error_code ec;
		const fs::path probe = fs::path("F:\\.tourmalineCache");
		if (fs::exists("F:\\", ec))
			return probe.generic_string();
	}
#ifdef _WIN32
	if (const char* appData = std::getenv("LOCALAPPDATA")) {
		return (fs::path(appData) / "Tourmaline" / "cache")
			.generic_string();
	}
#endif
	if (const char* home = std::getenv("HOME")) {
		return (fs::path(home) / ".tourmalineCache").generic_string();
	}
	return ".tourmalineCache";
}

std::string EnsureScratchRoot(const std::string& root) {
	if (root.empty())
		return {};
	std::error_code ec;
	const fs::path p(root);
	fs::create_directories(p, ec);
	if (ec || !fs::is_directory(p, ec))
		return {};
	// Per-class pak store lands here (phase 2).
	fs::create_directories(p / "artifacts", ec);
#ifdef _WIN32
	// Invisible scratch: hidden attribute on the root (children inherit
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
		// future changes; v1 is the plain create.
		const int version = db.exec("PRAGMA user_version");
		if (version < 1) {
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
	out.uri = q.getColumn(2).getString();
	out.pak = q.getColumn(3).getString();
	out.offset = static_cast<uint64_t>(q.getColumn(4).getInt64());
	out.size = static_cast<uint64_t>(q.getColumn(5).getInt64());
	out.meta = q.getColumn(6).getString();
	out.created = q.getColumn(7).getInt64();
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
		// a second editor instance touching the same scratch.
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
	const int64_t createdTs = (rec.created != 0) ? rec.created
		: std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	try {
		SQLite::Statement q(*m_impl->db,
			"INSERT INTO artifacts "
			"(hash, klass, uri, pak, offset, size, meta, created) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
			"ON CONFLICT (klass, uri) DO UPDATE SET "
			"  hash = excluded.hash, pak = excluded.pak, "
			"  offset = excluded.offset, size = excluded.size, "
			"  meta = excluded.meta, created = excluded.created");
		q.bind(1, rec.hash);
		q.bind(2, rec.klass);
		q.bind(3, rec.uri);
		q.bind(4, rec.pak);
		q.bind(5, static_cast<int64_t>(rec.offset));
		q.bind(6, static_cast<int64_t>(rec.size));
		q.bind(7, rec.meta);
		q.bind(8, createdTs);
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
			"SELECT hash, klass, uri, pak, offset, size, meta, created "
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
			"SELECT hash, klass, uri, pak, offset, size, meta, created "
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
			"SELECT hash, klass, uri, pak, offset, size, meta, created "
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

} // namespace smvk::cache
