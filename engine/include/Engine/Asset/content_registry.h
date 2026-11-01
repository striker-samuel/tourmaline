// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE CONTENT REGISTRY (the consolidated content architecture): the
// ONE content-resolution authority — "an asset has ONE identity, ONE
// effective provider, possibly many available versions".
//
// It answers exactly these questions (and nothing else — it is NOT a
// filesystem API in disguise: no arbitrary traversal, no file
// operations, no mount paths):
//
//   * What assets exist?                 (the record table)
//   * What is their stable identity?     (id + kind + logical path)
//   * Which provider currently supplies
//     each one?                          (provider selection)
//   * How do I load it?                  (Read — the byte lanes)
//   * Which providers are shadowed /
//     incompatible?                      (ProvidersFor diagnostics)
//
// THE CONVERGENCE CONTRACT (the reason this class exists): a lookup
// BY STABLE ID and a lookup BY LOGICAL PATH must agree about which
// version of an asset is active. Both forms resolve through the SAME
// provider selection, so a mod/patch/dev override that serves an
// asset's logical path wins for BOTH the compiled game (which holds
// constexpr ids from the generated project.h) and the serialized
// content (material texture URIs, scene-internal references) — never
// the old split where the [ids] join read the manifest-named pak
// while the VFS overlay read whichever mount preceded.
//
// Identity vs selection (the architectural split):
//   * The RECORD is identity: id, kind, logical path (+ the scene
//     join's cooked name). Today the .tapp appmanifest's [ids] table
//     is the identity source (the manifest compatibility lane); a
//     per-pak identity metadata lane (discovery without a manifest)
//     is the format-side successor that will feed the same table.
//   * The PROVIDER is selection: which pak / loose directory / memory
//     source currently supplies the record's path. BASE providers
//     are the manifest's release content (manifest order); OVERRIDE
//     providers are mods, patches and dev trees — they precede
//     base providers, and among overrides EARLIER REGISTRATION
//     WINS (the host declares its precedence by registration order).
//
// Byte lanes (mirroring the legacy ProjectRuntime::ReadAsset split):
//   * a pak provider with an ADOPTED backend (the whole image the
//     host preloaded or the scene lane opened) serves through that
//     backend — one copy of the bytes, whatever opened it first;
//   * a pak provider with no backend serves through the ASYNC PAGER
//     (the SMP8 light mount + ONE positional page read — no whole-
//     image preload EVER happens on the asset-read path);
//   * a loose/memory provider serves through its IVfsBackend.
//
// KINDS: byte reads serve the VFS-addressable payloads only —
// TEXTURE (.ktx2 envelope bytes), SOUND (raw .wav), SHADER (raw
// compiled bytecode) and GI manifests (Texture-kind blobs — the .gi/*
// payload channel). SCENE and MESH records refuse byte reads loudly
// (scenes load BY IDENTITY through the world loader; meshes ship
// inside their scene's cluster) — the refusal is the same contract
// the runtime façade always enforced, never a silent skip.
//
// THREADING: main/render thread only (construction, overrides,
// toggling, reads) — the same single-thread discipline the app-host
// runtime layers (ProjectRuntime/ProjectScenes) always carried. The
// underlying IVfsBackend/PakPager surfaces are internally safe for
// concurrent reads; this table is not.
//
// The pak format + the per-pak registry blobs ("#meshplan", "#schemas")
// stay implementation details below this boundary; the registry never
// parses cluster-internal payloads.

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/pak_pager.h>
#include <Engine/Vfs/vfs.h>

#include <cstdint>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tour::asset {

// One canonical CONTENT RECORD: the asset's STABLE IDENTITY. Built
// from the identity source ([ids] rows today; per-pak identity blobs
// next); immutable once adopted; NEVER carries provider state (the
// provider selection answers "who supplies it NOW").
struct ContentRecord {
	uint64_t id = 0;        // the stable content id (the catalog identity)
	uint8_t kind = 0;       // smvk::kCatalogKind* (the ABI kind byte)
	std::string path;       // the LOGICAL path (VFS-normalized; the join key)
	std::string pak;        // the manifest-declared owner pak (diagnostics)
	std::string sceneName;  // scene rows: the cooked scene name (the id->scene join)
};

// One CONTENT PROVIDER: a source that can supply bytes at logical
// paths. Base providers come from the manifest (release content, in
// manifest order); override providers are registered by the host
// (mods / patches / dev trees) and precede every base provider.
struct ContentProvider {
	enum class Type : uint8_t {
		Pak = 0,   // a pak archive (pager lane until a backend is adopted)
		Loose = 1, // a host directory or memory source (an IVfsBackend)
	};
	Type type = Type::Pak;
	std::string name;    // the mount identity (base: the manifest pak FILE name)
	std::string source;  // pak: the pak FILE PATH (the pager opens it); loose: diagnostics
	bool base = false;   // base = manifest release content; override = mod/patch/dev
	bool enabled = true; // the mod toggle (between-launches v1)
	bool pagerOpenTried = false;        // pak: open attempt made (loud once)
	std::string openError;              // pak: the pager open failure (a corrupt archive)
	uint64_t generation = 1;             // bumped on every enable/disable toggle
	std::shared_ptr<smvk::PakBackend> backend;  // pak: the RESIDENT image when adopted
	std::shared_ptr<smvk::IVfsBackend> loose;   // loose: the serving backend
	std::shared_ptr<smvk::PakPager> pager;      // pak: lazily opened (no image lane)

	[[nodiscard]] bool IsOverride() const { return !base; }
};

// The byte-read mapping for a catalog kind (the shared fold of the
// backend lane + the pager lane): TEXTURE and GI ride Texture-kind
// blobs (the .ktx2 envelope / the manifest row channel), SOUND and
// SHADER their own kinds; SCENE and MESH are identity-addressed and
// never byte-served. False = the kind has no VFS byte form.
[[nodiscard]] bool CatalogKindToPakKind(uint8_t kind,
	smvk::PakAssetKind& out);

class ContentRegistry {
public:
	// The product of a resolution: the record (null for a PATH-ONLY
	// resolution — mod-added content with no catalog row) and the
	// winning provider + its generation (a caller that caches the
	// resolution can compare generations later to detect a provider
	// toggle invalidating it).
	struct Resolution {
		const ContentRecord* record = nullptr;
		const ContentProvider* provider = nullptr;
		bool providerIsOverride = false;
		uint64_t providerGeneration = 0;
	};

	ContentRegistry() = default;

	// ---- construction -------------------------------------------------
	// Adopts a parsed + validated appmanifest as the BASE half: every
	// listed pak becomes a base provider (manifest order — the mount
	// order), every [ids] row a record. `manifestBaseDir` is the
	// manifest's own directory (its pak paths resolve RELATIVE to it,
	// exactly like the runtime façade's PakPath — a moved build
	// directory keeps working). REPLACES any previous state (the
	// between-launches rebuild form). Refuses rows whose pak is not
	// listed, kinds that do not map, or paths that do not normalize.
	bool AdoptManifest(const smvk::AppManifest& manifest,
		const std::string& manifestBaseDir, std::string& err);

	// ---- overrides (mods / patches / dev trees) -----------------------
	// Registers an OVERRIDE provider. Overrides precede every base
	// provider; among overrides EARLIER REGISTRATION WINS (the host
	// declares precedence by registration order). `name` must be
	// unique among providers. No bytes are read here — the pak lane
	// opens its TOC lazily on the first resolution that probes it.
	bool AddPakOverride(const std::string& name,
		const std::string& pakPath, std::string& err);
	// A loose host directory (the dev-override form): the registry
	// mounts a LooseBackend over rootDir. Reads serve the raw file
	// bytes at the logical path.
	bool AddLooseOverride(const std::string& name,
		const std::string& rootDir, std::string& err);
	// A generic backend override (memory sources, test backends).
	bool AddBackendOverride(const std::string& name,
		std::shared_ptr<smvk::IVfsBackend> backend, std::string& err);

	// ---- discovery (the .tapp-free lane) --------------------------------
	// Discovers a pak's OWN identity metadata (the "#ids" registry
	// blob — kPakAssetIdsPath) and merges it: the pak becomes a BASE
	// provider + its rows become records. NO whole-image load, NO
	// manifest — the light SMP8 mount + ONE page read for the blob
	// (the pager stays cached on the provider: later byte reads ride
	// it until a backend is adopted). A pak without the blob refuses
	// loudly (recook — discovery never guesses). A duplicate identity
	// across discovered paks is a DEPLOYMENT CONFLICT (loud, naming
	// both — one owner per asset, the same policing the build applies
	// at cook time); the discovery is ATOMIC (a conflicting pak adds
	// no records and no provider). Re-discovering a known name is
	// idempotent. Scene rows with an empty name ride the existing
	// single-scene inference at load time.
	bool DiscoverPak(const std::string& name,
		const std::string& pakPath, std::string& err);

	// ---- the convergent lookups ---------------------------------------
	// The record for a stable id (nullptr = unknown — a version skew
	// between the compiled header and the packaged content).
	[[nodiscard]] const ContentRecord* Find(uint64_t id) const;
	// The record for a kind + logical path (the reverse lookup —
	// nullptr when no record carries that pair).
	[[nodiscard]] const ContentRecord* Find(uint8_t kind,
		const std::string& path) const;

	// ---- resolution ----------------------------------------------------
	// Resolve by stable id: the record's logical path against every
	// ENABLED provider, overrides first (registration order), then
	// base (manifest order). Non-const: a pak provider with no
	// resident backend lazily opens its PAGER to probe the TOC.
	// Empty provider + err on: unknown id, an unservable kind, or no
	// provider serving the path (the corrupt-owner case names the
	// pak's own failure).
	Resolution Resolve(uint64_t id, std::string& err);
	// Resolve by kind + logical path: a record-carrying path resolves
	// through the SAME selection (the convergence contract); a path
	// with no record resolves PATH-ONLY (mod-added content) — the
	// same provider precedence, no catalog identity.
	Resolution Resolve(uint8_t kind, const std::string& path,
		std::string& err);

	// ---- the byte lanes -------------------------------------------------
	// The kind-appropriate bytes through the effective provider (the
	// adopted-backend lane, else the pager lane, else the loose
	// backend). SCENE and MESH ids refuse loudly (identity load).
	std::optional<std::vector<uint8_t>> Read(uint64_t id,
		std::string& err);
	std::optional<std::vector<uint8_t>> Read(uint8_t kind,
		const std::string& path, std::string& err);
	// A path with NO catalog identity (loose files, mod-added blobs):
	// the same provider selection, probing every VFS-addressable kind.
	std::optional<std::vector<uint8_t>> ReadPath(const std::string& path,
		std::string& err);

	// ---- pak-provider backend adoption (the shared-image lane) ---------
	// Registers a RESIDENT backend for the pak provider named
	// `pakFile` (the manifest pak name for base providers, the
	// registered name for overrides): every later read serves through
	// that image (no pager), releasing the provider's pager. Idempotent
	// per archive; a provider with a backend never answers through
	// another. No-op (false) when no provider carries that name.
	bool AdoptBackend(const std::string& pakFile,
		std::shared_ptr<smvk::PakBackend> backend);
	[[nodiscard]] bool HasBackend(const std::string& pakFile) const;
	// The provider's ADOPTED backend (the shared-image lane's
	// accessor — the scene/asset loaders hand the reader + image to
	// their cluster paths). Null when the pak provider carries none
	// (the pager lane serves its bytes).
	[[nodiscard]] std::shared_ptr<smvk::PakBackend> BackendFor(
		const std::string& pakFile) const;
	// The provider's PAGER (the explicit accessor — the compiled-plan
	// lane's TOC folding + the geometry-streaming page source): opens
	// lazily and REOPENS after a backend adoption released the cached
	// instance (the caller owns the shared_ptr it gets). Null + err
	// when the provider is unknown or its archive fails the light
	// mount (the failure records the provider's openError — the
	// corrupt-owner diagnostic feeds on it).
	[[nodiscard]] std::shared_ptr<smvk::PakPager> PagerFor(
		const std::string& pakFile, std::string& err);

	// ---- mod toggling (between-launches v1) -----------------------------
	// Enables/disables a provider by name (overrides AND base — the
	// host's deployment policy decides; a disabled base pak answers
	// nothing). New resolutions see the new selection IMMEDIATELY;
	// ALREADY-LOADED resources stay valid (a provider toggle never
	// mutates in-use resources — the host reloads what it wants).
	// Returns false (no-op) on an unknown name or a no-change toggle.
	bool SetProviderEnabled(const std::string& name, bool enabled);
	[[nodiscard]] bool ProviderEnabled(const std::string& name) const;
	// The provider's generation (bumped on every toggle): a caller that
	// cached a resolution compares this to detect the invalidation.
	// 0 = unknown provider.
	[[nodiscard]] uint64_t ProviderGeneration(
		const std::string& name) const;

	// ---- diagnostics ------------------------------------------------------
	// Every ENABLED provider that serves the path, in PRECEDENCE order
	// (the winner first): the shadowed-provider report (collision
	// diagnostics, tooling). Non-const: probes pak TOCs (see Resolve).
	std::vector<std::string> ProvidersFor(uint8_t kind,
		const std::string& path);
	// The provider table in precedence order (name + type + flags).
	struct ProviderInfo {
		std::string name;
		std::string source;
		ContentProvider::Type type{};
		bool base = false;
		bool enabled = true;
	};
	[[nodiscard]] std::vector<ProviderInfo> ProviderInfos() const;
	// The record table (the identity half — tooling/tests).
	[[nodiscard]] const std::deque<ContentRecord>& Records() const {
		return m_records;
	}
	// The record count (tooling).
	[[nodiscard]] size_t RecordCount() const { return m_records.size(); }

private:
	// Provider lookup by mount name (searches overrides then base).
	[[nodiscard]] ContentProvider* ProviderByName(
		const std::string& name);
	[[nodiscard]] const ContentProvider* ProviderByName(
		const std::string& name) const;
	// Does this provider serve `norm` for `kind`? Probes the resident
	// backend / the loose backend / the provider's pager TOC (opening
	// it lazily — a failed open is recorded once, loudly).
	[[nodiscard]] bool ProviderServes(ContentProvider& p,
		const std::string& norm, smvk::PakAssetKind kind);
	// The provider's pager (the BYTE-LANE helper — never called with
	// a resident backend present): lazily opened; a failed open
	// records openError ONCE and answers null from then on. Null when
	// the archive fails the light mount.
	[[nodiscard]] smvk::PakPager* PagerOf(ContentProvider& p);
	// The bytes for `norm` through this provider (kind fixed, or any
	// VFS-addressable kind when kind == nullptr). Empty optional on a
	// miss (err EMPTY — the caller composes the no-provider form);
	// err filled on a real failure (a corrupt page names its path).
	std::optional<std::vector<uint8_t>> ReadThrough(ContentProvider& p,
		const std::string& norm, const smvk::PakAssetKind* kind,
		std::string& err);
	// The internal resolution core (both public forms feed it):
	// `record` may be null (path-only — `pathKind` is then the byte
	// kind probing the path). `norm` is the normalized path.
	Resolution ResolveInternal(const ContentRecord* record,
		uint8_t pathKind, const std::string& norm, std::string& err);
	// The provider deques: overrides (registration order — earlier
	// registered = higher precedence) then base (manifest order).
	std::deque<ContentRecord> m_records;       // the identity table
	std::deque<ContentProvider> m_overrides;  // mods / patches / dev trees
	std::deque<ContentProvider> m_base;       // the manifest's release paks
	// id -> record index (the stable-id join).
	std::unordered_map<uint64_t, size_t> m_byId;
	// (kind, normalized path) -> record index (the reverse join).
	std::unordered_map<uint64_t, size_t> m_byPath;
	[[nodiscard]] uint64_t PathKey(uint8_t kind,
		const std::string& norm) const;
};

} // namespace tour::asset
