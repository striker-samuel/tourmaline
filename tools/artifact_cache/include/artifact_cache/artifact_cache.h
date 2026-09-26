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
#include <span>
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

// ---------------------------------------------------------------------------
// Artifact store: the per-class pak files + registry glue.
//
// Layout under the scratch root:
//   artifacts/textures.pak, meshes.pak, materials.pak, ...
//     One pak per class, grown in place (engine AppendPakBlobs keeps
//     the image valid for PakReader::Open at all times; entries are
//     uncompressed so registry (offset, size) reads are direct).
//   cache.db
//     (klass, uri) -> hash (of the SOURCE the artifact was cooked
//     from), pak, offset, size, meta.
//
// Write path: Put stages in memory (cheap); Flush appends all staged
// payloads per class pak in ONE grow + commits the registry rows (one
// CRC pass per flush regardless of staged count — batch-friendly for
// imports). Reads see staged bytes before Flush (write-behind
// coherence). A crash between pak growth and registry commit leaves
// dead payload only — harmless (cache).
// ---------------------------------------------------------------------------
class ArtifactStore {
public:
	// scratchRoot: the EnsureScratchRoot()-ed cache root.
	ArtifactStore(std::string scratchRoot, CacheDb& db);
	~ArtifactStore();
	ArtifactStore(const ArtifactStore&) = delete;
	ArtifactStore& operator=(const ArtifactStore&) = delete;

	// Stage one cooked artifact. klass: "texture" | "mesh" |
	// "material" | "light" | "camera" | "scene". sourceHash: content
	// hash of the INPUT the artifact was processed from (the
	// "already cooked?" key); payload: the cooked bytes; meta:
	// kind-specific metadata (opaque to the store).
	bool Put(const std::string& klass, const std::string& uri,
		std::span<const uint8_t> payload, const std::string& sourceHash,
		const std::string& meta, std::string& err);
	// Direct read: staged map first, then the class pak at the
	// registry's (offset, size). False when unknown.
	bool Get(const std::string& klass, const std::string& uri,
		std::vector<uint8_t>& out, std::string& err);
	// Registry-only probe (no IO): true when (klass, uri) is
	// registered with exactly this source hash — the "already
	// processed?" fast path. A hint, not a guarantee: Get failing
	// after Has means reprocess (the cache contract).
	bool Has(const std::string& klass, const std::string& uri,
		const std::string& sourceHash) const;
	// Commit all staged puts (one pak grow per touched class + one
	// registry transaction). No-op when nothing is staged.
	bool Flush(std::string& err);
	size_t StagedCount() const { return m_staged.size(); }
	// Registry removal (the payload becomes dead space; GC compacts).
	bool Remove(const std::string& klass, const std::string& uri,
		std::string& err);
	// Corruption repair: drop one class pak + its registry rows. The
	// store keeps working; callers reprocess on demand.
	bool ResetClass(const std::string& klass, std::string& err);
	// Whole-class pak image for read-only consumers (VFS mounts —
	// PakBackend borrows the bytes; keep the vector alive).
	bool ReadPakImage(const std::string& klass,
		std::vector<uint8_t>& out, std::string& err) const;
	const std::string& Root() const { return m_root; }

private:
	struct Staged {
		std::string klass;
		std::string uri;
		std::vector<uint8_t> payload;
		std::string sourceHash;
		std::string meta;
	};
	std::string PakPath(const std::string& klass) const;
	bool GetFromDisk(const std::string& klass, const std::string& uri,
		std::vector<uint8_t>& out, std::string& err) const;

	std::string m_root;
	CacheDb& m_db;
	std::vector<Staged> m_staged;
};

} // namespace smvk::cache
