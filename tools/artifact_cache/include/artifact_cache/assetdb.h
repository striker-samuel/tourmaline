// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Source-asset identity database (Unity SourceAssetDB analog).
//
// Every project source asset carries its identity in a sibling .meta
// sidecar (`Avocado.gltf` + `Avocado.gltf.meta`) holding a 128-bit GUID.
// The GUID identifies the FILE and survives moves/renames (the sidecar
// travels with it); references between files bind to it instead of to
// paths. This database is the GUID<->path directory over the project:
// a regenerable cache (delete assets.db + rescan), never source data.
//
// Layering vs the neighbors (deliberate, Unity-mirrored):
// - ArtifactDB (cache.db: klass/uri rows + payloads) answers "do I have
//   a current import RESULT?" — keyed by WHAT was imported. SourceAssetDB
//   answers "WHERE is asset <guid>?" — keyed by WHICH asset. Different
//   questions, different tables, complementary (not redundant).
// - Content hashes (artifact `hash`, m_cookDefHash) answer "which VERSION
//   is this?" — orthogonal to GUID identity; both are kept.
// - Cooked/runtime identity (pak TOC paths, scene indices, gi/<stem>.*)
//   stays path-based: the player never queries this DB.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk::cache {

// 32 lowercase hex chars (matches smvk::tour::GenerateUuid output).
bool IsValidAssetGuid(const std::string& guid);
// "<assetPath>.meta" sibling path.
std::string MetaPathFor(const std::string& assetPath);
// Reads the sidecar guid. False + err when the sidecar is missing
// ("no .meta") or carries no/invalid guid ("invalid guid ...") —
// the caller issues a fresh GUID in both cases.
bool ReadAssetMeta(const std::string& assetPath, std::string& outGuid,
	std::string& err);
// Writes (or replaces) the sidecar. The guid must be valid. Preserves
// any INPUT PLUG rows already present (see ReadAssetPlugs) — adoption/
// re-adoption rewrites must never drop authored plugs.
bool WriteAssetMeta(const std::string& assetPath, const std::string& guid,
	const std::string& importer, std::string& err);

// ---------------------------------------------------------------------------
// INPUT PLUGS: per-asset import inputs serialized in the .meta sidecar
// ("plug.<kind>.<n>.old=..." / "plug.<kind>.<n>.new=..." rows). A plug is
// INPUT CONFIGURATION for whoever imports the asset — the importer applies
// it (e.g. gltf_import's texture relink: an image whose authored uri
// matches `oldUri` reads `newPath` instead), so plugs apply identically at
// editor import, cook and bake. Never the tour format: plugs belong to the
// ASSET (the tour already references assets by GUID) and travel with the
// file via this sidecar.
//
// Kinds: "texture" (image-uri relink). The rows are index-paired
// (hand-edited unpaired rows skip on read).
// ---------------------------------------------------------------------------
struct AssetPlug {
	std::string kind;    // "texture"
	std::string oldUri;  // plug key (the authored image uri)
	std::string newPath; // plug value (the replacement source)
};
// Every plug row of the sidecar (parse order; empty when none/missing).
std::vector<AssetPlug> ReadAssetPlugs(const std::string& assetPath);
// Rewrites ONLY the plug rows of one kind on the sidecar (guid/importer
// and other kinds' rows preserved). Empty `plugs` clears the kind. The
// sidecar's guid must be valid (missing sidecar = write error — adopt the
// asset first).
bool WriteAssetPlugs(const std::string& assetPath, const std::string& kind,
	const std::vector<AssetPlug>& plugs, std::string& err);

// One directory row: WHICH asset (guid) lives WHERE (project-relative
// posix path when inside the project, absolute generic otherwise).
struct AssetRecord {
	std::string guid;
	std::string path;
	std::string kind; // "model", ... (opaque to the db)
	int64_t size = 0;
	int64_t mtime = 0; // seconds since epoch (change token)
};

// ---------------------------------------------------------------------------
// THE PERSISTENT CONTENT-ID REGISTRY (the generated project.h's stable
// identities). ONE AND THE SAME DATABASE as the asset directory — the
// owner's call: no separate .tourids file, identity lives with identity
// in assets.db under the project's registry root. IDs belong to ASSETS
// (GUIDs), never paths: a rename keeps the id + the ORIGINAL symbol.
// Allocation is monotonic per kind and NEVER reuses — a deleted asset
// TOMBSTONES (active=0: the id+symbol stay reserved forever, the row
// drops out of the generated catalog).
//
// Id ranges (the generated header's namespaces): scenes 0x1000..0x1FFF,
// textures 0x2000..0x2FFF, meshes 0x3000..0x3FFF.
// ---------------------------------------------------------------------------
struct ContentIdRecord {
	std::string guid;   // the asset identity (sidecar/manifest GUID)
	uint64_t id = 0;    // the stable project-local id (kind-namespaced)
	std::string symbol; // the C++ identifier (stable from first assign)
	std::string kind;   // "scene" | "texture" | "mesh"
	std::string path;   // last-known project-relative path (informational
	                    // ONLY — never the identity)
	bool active = true; // false = tombstone (reserved, not cataloged)
};

struct RescanStats {
	size_t adopted = 0; // new rows for sidecar-carrying files found
	size_t moved = 0;   // known GUIDs re-pointed at a new path
	size_t missing = 0; // known GUIDs whose file is gone (rows KEPT)
	// Manifest-registered derived assets (.mesh/.material/.anim of a
	// gltf.meta manifest; no per-file sidecars — identity is
	// manifest-driven, registered here into the same table.
	size_t derived = 0;
};

class AssetDatabase {
public:
	AssetDatabase();
	~AssetDatabase();
	AssetDatabase(const AssetDatabase&) = delete;
	AssetDatabase& operator=(const AssetDatabase&) = delete;

	// Opens (creating when missing) <registryRoot>/assets.db for the
	// project at <projectRoot>. False + err on failure.
	bool Open(const std::string& registryRoot,
		const std::string& projectRoot, std::string& err);
	void Close();
	bool IsOpen() const { return m_impl != nullptr; }

	// Records (guid -> assetPath). The guid must be valid; the file
	// must exist. Last registration wins (a duplicated sidecar GUID
	// re-points at the latest adoption — rescan reports the fallout).
	bool AdoptFile(const std::string& assetPath, const std::string& guid,
		const std::string& kind, std::string& err);
	// GUID for a known path (no adoption; "" when unknown).
	bool GuidForPath(const std::string& assetPath, std::string& outGuid);
	// Absolute path for a GUID ("" when unknown OR the file is gone —
	// call RescanProject first when a reference dangles, moves heal).
	bool PathForGuid(const std::string& guid, std::string& outAbsPath);
	// Walk the project tree: adopt every sidecar-carrying file found,
	// re-point known GUIDs found at new paths, count rows whose file
	// is gone (kept — a retarget, not a delete). The healing pass for
	// Explorer-side moves/renames.
	bool RescanProject(RescanStats& stats, std::string& err);
	// Full listing (asset browser): every registered row, ordered by
	// stored path. `path` stays the STORED form (project-relative or
	// absolute) — resolve for display/opening via ResolvePath.
	bool All(std::vector<AssetRecord>& out, std::string& err);
	// Absolute path for a STORED row path ({} when the db is closed).
	// No existence requirement (the browser marks missing rows).
	std::string ResolvePath(const std::string& storedPath) const;

	// ---- the content-id registry (see ContentIdRecord) ----------------
	// Adopt-or-return: the guid's existing id+symbol (re-activating a
	// tombstone), else a fresh allocation (the kind's next free id + a
	// collision-free symbol derived from the path's stem at FIRST
	// registration — renames later keep this original symbol).
	bool EnsureContentId(const std::string& guid, const std::string& kind,
		const std::string& projectRelPath, uint64_t& outId,
		std::string& outSymbol, std::string& err);
	// Every row, tombstones included (the generator filters active).
	bool ListContentIds(std::vector<ContentIdRecord>& out,
		std::string& err);
	// Tombstone / re-activate by guid (the generation pass marks rows
	// whose assets vanished from the resolved plan).
	bool SetContentIdActive(const std::string& guid, bool active,
		std::string& err);

private:
	struct Impl;
	Impl* m_impl = nullptr;
	std::string m_dbPath;
	std::string m_projectRoot; // normalized generic
};

} // namespace smvk::cache
