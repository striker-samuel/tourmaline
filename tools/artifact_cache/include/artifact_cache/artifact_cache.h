#pragma once
// Editor artifact cache (scratch-disk registry).
//
// The scratch disk holds every processed editor artifact: a SQLite
// registry (cache.db) plus per-class pak files (textures.pak,
// meshes.pak, ...) written next to it. It is a CACHE, never a source
// of truth: .tour -> final pak cooks must always succeed from the
// authored inputs alone — cache hits only accelerate processing.
//
// Location is user-personalizable (editor prefs [scratch] dir=...; a
// different drive keeps C: from filling up). The default root lives on
// F:\ when present and is created with the hidden attribute so the
// scratch dir stays invisible in normal browsing.

#include <cstdint>
#include <string>
#include <vector>

namespace smvk::cache {

// Default scratch root: F:/.tourmalineCache when F:\ is writable,
// else %LOCALAPPDATA%/Tourmaline/cache (machines without an F: drive).
// The editor prefs override always wins over this default.
std::string DefaultScratchRoot();

// Creates the root (and parents) when missing, applies the Windows
// hidden attribute, and returns the normalized path. Empty on
// failure. The artifacts/ subdirectory for the per-class paks is
// ensured too (phase 2 store layout).
std::string EnsureScratchRoot(const std::string& root);

// 64-bit FNV-1a content hash (hex, 16 chars) + length mix. Local-cache
// grade: collisions would need ~2^32 distinct artifacts; the cost of a
// miss is a reprocess, never a wrong result (paks re-validate).
std::string HashBytes(const void* data, size_t size);

// One registry entry: where a processed artifact lives in the scratch.
struct ArtifactRecord {
	std::string hash;    // content hash of the SOURCE input (HashBytes)
	std::string klass;   // "texture" | "mesh" | "material" | ...
	std::string uri;     // asset URI the artifact is registered under
	std::string pak;     // pak file name, relative to the scratch root
	uint64_t offset = 0; // byte offset of the payload inside the pak
	uint64_t size = 0;   // payload size in bytes
	std::string meta;    // kind-specific metadata (opaque to the db)
	int64_t created = 0; // unix seconds (insert time)
};

// SQLite registry over cache.db. Thin and synchronous: editor-scale
// (thousands of rows) with no threading needs. Main-thread use.
class CacheDb {
public:
	CacheDb();
	~CacheDb(); // closes if open
	CacheDb(const CacheDb&) = delete;
	CacheDb& operator=(const CacheDb&) = delete;

	// Opens (creating when missing) and initializes the schema
	// (PRAGMA user_version migration). False + err on failure.
	bool Open(const std::string& path, std::string& err);
	void Close();
	bool IsOpen() const { return m_impl != nullptr; }
	const std::string& Path() const { return m_path; }

	// Insert or replace (klass, uri) — latest registration wins. The
	// hash column is indexed for the "already processed?" fast path.
	bool Put(const ArtifactRecord& rec, std::string& err);
	// Latest record for (klass, uri). False when none.
	bool Get(const std::string& klass, const std::string& uri,
		ArtifactRecord& out, std::string& err);
	// First record matching the content hash (any class).
	bool GetByHash(const std::string& hash, ArtifactRecord& out,
		std::string& err);
	bool Remove(const std::string& klass, const std::string& uri,
		std::string& err);
	// Full listing (cache GC / inspection tooling).
	std::vector<ArtifactRecord> All(std::string& err);
	uint64_t Count(std::string& err);

private:
	struct Impl;
	Impl* m_impl = nullptr;
	std::string m_path;
};

} // namespace smvk::cache
