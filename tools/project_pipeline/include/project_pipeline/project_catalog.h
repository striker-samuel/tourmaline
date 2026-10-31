// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE GENERATED project.h (content as code): the resolved BuildPlan +
// the ASSET DATABASE's content-id registry (assets.db — ONE AND THE
// SAME store as the asset directory, the owner's call: no separate
// .tourids file, identity lives with identity) become a pure constexpr
// C++ catalog. Game code programs against stable IDs (ids belong to
// ASSETS — GUIDs — never paths: renames keep the id + the original
// symbol); the paks carry the same content. The header is
// GENERATION-STAMPED (timestamp + generation counters + a content
// hash, all constexpr — cheap version control for asset time): the
// stamp moves ONLY on a generation EVENT (any change to the emitted
// text, the explicit <Name>_stamp refresh, or a recipe build that
// moved the fingerprints). An unchanged generation re-emits
// byte-identical text, so write-only-on-change keeps holding and
// nothing that includes the header recompiles.
//
// The identity ladder per entity (the GUID the registry keys on):
//   scene  — the .tour's .meta sidecar (minted + adopted on first
//            generation when absent; the sidecar travels with the file,
//            so a renamed scene keeps its identity)
//   mesh   — the family manifest's source GUID (the .gltf.meta that
//            doubles as the imported family's sidecar — SourceAssetGuid)
//   texture— the file's .meta sidecar (minted + adopted when absent)
//
// Deleted assets TOMBSTONE (the registry row keeps id+symbol reserved,
// active=0, absent from the catalog). New assets allocate the kind
// range's next free id — monotonic, never reused. The catalog reflects
// the LAST generation; regenerating another recipe re-activates its
// rows (ids are never lost across recipes in one project).

#include <map>
#include <string>
#include <vector>

#include <artifact_cache/assetdb.h>

#include "build_plan.h"
#include "project.h"

namespace smvk::project {

struct CatalogEntry {
	uint64_t id = 0;
	std::string symbol; // the C++ identifier (stable from first assign)
	std::string path;   // project-relative (the plan's identity — the
	                    // runtime pairs it with the pak + the manifest)
	std::string pak;    // the owning pak id
};

struct ProjectCatalog {
	std::string projectName;
	uint64_t projectId = 0; // stable from the project identity
	// The namespaces (the registry's kind ranges): scenes 0x1xxx,
	// textures 0x2xxx, meshes 0x3xxx, sounds 0x4xxx — sorted by id,
	// deterministic.
	std::vector<CatalogEntry> scenes;
	std::vector<CatalogEntry> textures;
	std::vector<CatalogEntry> meshes;
	std::vector<CatalogEntry> sounds;
	// (id, file) in plan (mount) order — paks are named entities, not
	// id carriers: renaming a package updates this table; asset ids
	// stay.
	std::vector<std::pair<std::string, std::string>> paks;
	uint64_t entryScene = 0; // the scenes:: namespace id (0 = none)
};

// ---- the generation stamp + the EOF metadata -----------------------
// project.h is TWO artifacts in one file: the constexpr C++ catalog
// (the compiler's) and, appended after the C++ section, a
// machine-readable COMMENT block (TOURMALINE-METADATA-BEGIN/END — the
// tools'). The comment is never coded against; it carries the
// generation stamp + the last generation's file fingerprints. The
// fingerprints are the VERIFIED content hashes (every source hashed
// from bytes on every run — the previous records are the staleness
// witness for the cook-state rules and the byte-equality the no-op
// probe compares, NEVER a carry that skips rehashing: metadata hints
// do not prove byte equality).
struct GenerationStamp {
	uint64_t generation = 0; // 1-based; increments every generation event
	uint64_t timestamp = 0;  // unix seconds of the generation event
	// fnv1a64, 16 hex — the CATALOG SEMANTICS only (name, project id,
	// ids, symbols, paths, pak table, entry scene). A texture repixel
	// keeps this hash; the EOF asset record carries the new file hash.
	std::string contentHash;
};

// One asset source's fingerprint (the EOF [assets] record, keyed by
// the STABLE content id — never array position).
struct AssetFingerprint {
	uint64_t id = 0;
	std::string path;   // project-relative (the catalog identity)
	std::string pak;    // the owning pak id
	int64_t mtime = -1; // unix seconds (-1 = the source is missing)
	uint64_t size = 0;
	std::string hash;   // "fnv1a64:<16hex>"; empty when missing
};

// One published pak's fingerprint (the EOF [paks] record). `state` is
// the cook-state CLAIM, never a content proof:
//   verified — the recipe build wrote + verified + policed this exact
//              pak (the build-side header re-stamp), or nothing
//              changed since a generation that said so
//   stale    — an asset of the pak is unpublished, changed since the
//              last generation, or newer than the pak file
//   missing  — no published pak file found for the pak id
struct PakFingerprint {
	std::string file;   // the published (content-addressed) file name
	int64_t mtime = -1;
	uint64_t size = 0;
	std::string hash;   // "fnv1a64:<16hex>"; empty when missing
	std::string state;  // verified | stale | missing
};

// The previous header's EOF metadata (the generator's generation
// ledger + the cook-state witness). `present` is false for a missing
// file, a pre-stamp header, or an UNKNOWN metadata version — every
// one of those reads as "cache empty, scan fresh" (never an unsafe
// reuse). NOTE: the asset records are DIAGNOSTICS + staleness
// witnesses, never freshness proof — freshness is the CONTENT HASH
// (see FingerprintFile), recomputed from bytes on every run.
struct HeaderMetadata {
	bool present = false;
	uint64_t generation = 0;
	uint64_t timestamp = 0;
	std::string contentHash;
	std::map<uint64_t, AssetFingerprint> assets; // keyed by content id
	std::map<std::string, PakFingerprint> paks;  // keyed by file name
};

// Builds the catalog from the resolved plan against the registry:
// ensures content ids for every scene + every owned mesh/texture asset
// (BakeFiles are bake plumbing, not game-loadable content — no ids in
// v1), tombstones registry rows whose assets vanished from the plan,
// records the pak table. `contentDeps` collects every content file the
// generation touched (the CMake DEPFILE feed — rebuild the header when
// the content set changes).
bool GenerateProjectCatalog(const Project& project, const BuildPlan& plan,
	smvk::cache::AssetDatabase& db, ProjectCatalog& out,
	std::vector<std::string>& contentDeps, std::string& err);

// The catalog semantics fingerprint (see GenerationStamp::contentHash).
[[nodiscard]] std::string ComputeCatalogContentHash(
	const ProjectCatalog& c);

// The catalog IDENTITY fingerprint (the shared fold, appmanifest.h's
// ComputeCatalogFingerprint): over the entries in emission order,
// folding (id, kind byte, pakIndex, path) — the SAME value the
// generated header emits as project::CatalogFingerprint and the
// published .tapp carries on its `catalog` line. One fold, no drift
// between the compiled and deployed halves.
[[nodiscard]] uint64_t CatalogFingerprintOf(const ProjectCatalog& c);

// The header text (pure — the caller decides where it lands): the C++
// catalog + the stamp + the EOF metadata comment. Byte-deterministic
// for the same inputs (the timestamp is an INPUT here;
// ComposeProjectHeader is the entry that decides it).
[[nodiscard]] std::string EmitProjectHeader(const ProjectCatalog& c,
	const GenerationStamp& stamp,
	const std::vector<AssetFingerprint>& assets,
	const std::vector<PakFingerprint>& paks);

// Read a header file's EOF metadata (a missing file is NOT an error:
// out.present stays false). An unknown metadata version also reads as
// absent — a fresh scan, never a stale-cache reuse.
bool ReadProjectHeaderMetadata(const std::string& path,
	HeaderMetadata& out, std::string& err);

// stat one file (unix-seconds mtime + size). A missing file is not an
// error (mtime=-1, size=0); false is a real stat failure only.
bool StatFile(const std::string& absPath, int64_t& mtime,
	uint64_t& size, std::string& err);

// stat + hash one file: the VERIFIED content fingerprint. The hash is
// ALWAYS computed from the file's bytes — matching size + mtime is
// NEVER accepted as proof of unchanged content (a preserved-timestamp
// or backdated same-size edit must be detected; mtime/size ride the
// EOF records as diagnostics and demotion hints only, never as
// freshness). A missing file is not an error (mtime=-1, size=0, hash
// empty).
bool FingerprintFile(const std::string& absPath,
	int64_t& mtime, uint64_t& size, std::string& hash,
	std::string& err);

// The catalog's asset fingerprints (kind order, id-sorted — the
// emission order), every source hashed from bytes.
bool CollectAssetFingerprints(const Project& project,
	const ProjectCatalog& catalog,
	std::vector<AssetFingerprint>& out, std::string& err);

// The ids pass's pak discovery in `pakDir` (the publish dir): the
// published appmanifest(s) name the live content-addressed pak files,
// joined to the catalog's pak ids through the manifest's [ids] rows
// (stable ids — never file-name guessing). `assets` are the CURRENT
// asset fingerprints (the staleness witness). Computes the cook state
// per the PakFingerprint rules.
bool DiscoverPakFingerprints(const std::string& pakDir,
	const ProjectCatalog& catalog,
	const std::vector<AssetFingerprint>& assets,
	const HeaderMetadata& prev, std::vector<PakFingerprint>& out,
	std::string& err);

// THE stamp-policy entry — both writers (the ids command and the
// recipe build) go through here. Composes the new header text against
// the file at `path` (absent/legacy = generation 1):
//   unchanged content + unchanged fingerprints + !forceStamp
//     -> byte-identical text, changed=false (the caller's
//        write-only-on-change then never touches the file)
//   any change, or forceStamp (the <Name>_stamp target)
//     -> a NEW GENERATION: timestamp=now, generation=prev+1
void ComposeProjectHeader(const ProjectCatalog& c,
	const std::vector<AssetFingerprint>& assets,
	const std::vector<PakFingerprint>& paks, const std::string& path,
	bool forceStamp, std::string& text, GenerationStamp& stamp,
	bool& changed);

// Write-only-on-change: reads the existing file and writes only when
// the text differs (an unchanged catalog never touches the mtime, so
// nothing that includes the header recompiles). True + wrote=false
// when the file already matched.
bool WriteProjectHeaderIfChanged(const std::string& path,
	const std::string& text, bool& wrote, std::string& err);

} // namespace smvk::project
