// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Editor artifact cache (project-owned artifact registry).
//
// Each project owns its artifact dir (<root>/.tourmaline/, hidden):
// a SQLite registry (cache.db) plus per-class pak files (pak/*.pak)
// written next to it. It is a CACHE, never a source of truth:
// .tour -> final pak cooks must always succeed from the authored
// inputs alone — cache hits only accelerate processing. There is no
// global/user registry disk: without a project cache the tools run
// plain (content roots join their <contentDir>/.tourmaline by
// convention when one carries a cache.db — never by flag).
// Early-match is two-tier: mtime decides stale (MeshArtifactStale —
// missing artifacts, digest mismatch, or artifact-predates-source),
// then content hash confirms the hit (Has exact-hash probe) before
// any staged bytes serve. Either tier missing degrades to reprocess,
// never to a wrong result.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace smvk::cache {

// Creates the root (and parents) when missing, applies the Windows
// hidden attribute, and returns the normalized path. Empty on
// failure. The pak/ subdirectory for the per-class paks is
// ensured too (phase 2 store layout).
std::string EnsureRegistryRoot(const std::string& root);

// Convention (no flag): the project cache for a content root lives at
// <contentDir>/.tourmaline and participates only when its cache.db
// already exists — never created here, so bare trees stay clean.
// Returns the normalized root, or "" when absent. Cook/pak resolve
// their registry + funnel DB through this; explicit programmatic
// roots (the editor's store) bypass it.
std::string ExistingRegistryRootFor(const std::string& contentDir);

// 64-bit FNV-1a content hash (hex, 16 chars) + length mix. Local-cache
// grade: collisions would need ~2^32 distinct artifacts; the cost of a
// miss is a reprocess, never a wrong result (paks re-validate).
std::string HashBytes(const void* data, size_t size);

// One registry entry: where a processed artifact lives in the registry.
struct ArtifactRecord {
	std::string hash;    // content hash of the SOURCE input (HashBytes)
	std::string klass;   // canonical kind (see artifact_kind.h)
	std::string uuid;    // artifact identity (manifest GUID for derived
	                     // family members; "" when unregistered)
	std::string uri;     // asset URI the artifact is registered under
	std::string pak;     // pak file name, relative to the registry root
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
	// First record with this (klass, uuid) — the identity lookup for
	// manifest-registered derived artifacts. False when none.
	bool FindByUuid(const std::string& klass, const std::string& uuid,
		ArtifactRecord& out, std::string& err);
	// Identity registration (the derive path): upserts the identity
	// columns (hash, uuid, meta) for (klass, uri) WITHOUT touching a
	// staged payload's location columns — a Flush after a Register
	// must not clobber, and a Register after a Flush must not orphan
	// bytes. Rows may carry no payload yet (pak empty + size 0):
	// identity-only rows address loose-backed derivations until a
	// payload stages. Kind must be canonical (see artifact_kind.h).
	bool Register(const std::string& klass, const std::string& uuid,
		const std::string& uri, const std::string& hash,
		const std::string& meta, std::string& err);
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
// Layout under the registry root (the folders-for-schema):
//   pak/texture.pak, pak/mesh.pak, ... (see artifact_kind.h — file
//     names are singular-by-construction: pak/<klass>.pak)
//     One pak per canonical kind, grown in place (engine AppendPakBlobs keeps
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
	// registryRoot: the EnsureRegistryRoot()-ed cache root.
	ArtifactStore(std::string registryRoot, CacheDb& db);
	~ArtifactStore();
	ArtifactStore(const ArtifactStore&) = delete;
	ArtifactStore& operator=(const ArtifactStore&) = delete;

	// Stage one cooked artifact. klass: canonical kind (see
	// artifact_kind.h — unknown kinds fail closed); uri: asset URI;
	// sourceHash: content hash of the INPUT the artifact was processed
	// from (the "already cooked?" key); payload: the cooked bytes; meta:
	// kind-specific metadata (opaque to the store); uuid: artifact
	// identity for manifest-registered derivations ("" = unregistered).
	bool Put(const std::string& klass, const std::string& uri,
		std::span<const uint8_t> payload, const std::string& sourceHash,
		const std::string& meta, std::string& err,
		const std::string& uuid = "");
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
		std::string uuid; // manifest identity ("" = unregistered)
	};
	std::string PakPath(const std::string& klass) const;
	bool GetFromDisk(const std::string& klass, const std::string& uri,
		std::vector<uint8_t>& out, std::string& err) const;

	std::string m_root;
	CacheDb& m_db;
	std::vector<Staged> m_staged;
};

} // namespace smvk::cache
