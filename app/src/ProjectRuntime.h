// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// ProjectRuntime: the apphost's PUBLIC project façade — the SESSION
// over the content registry (the consolidated content architecture).
// The generated project.h gives game code STABLE IDS at compile time;
// the appmanifest's [ids] table mirrors them at run time. This class
// bridges the two: it OWNS the manifest (the identity source, until
// per-pak identity metadata replaces it) and ONE
// tour::asset::ContentRegistry — the single content-resolution
// authority where BOTH the stable-id form and the logical-path form
// converge on the same effective provider (ReadAsset(id) and
// ReadAssetByPath(kind, path) agree; an override serving a path wins
// for both — see Engine/Asset/content_registry.h).
//
// The v1 shape of the epoch-2 spec's illustrative APIs:
//   const auto* row = runtime.Asset(project::textures::Stone);
//   auto bytes = runtime.ReadAsset(project::textures::Stone);
// (a GPU-upload entry point lands on top of ReadAsset when a game
// element needs it — the bytes are the standard .ktx2 payload every
// loader in the tree already consumes.)

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/content_registry.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/pak_pager.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app {

class ProjectRuntime {
public:
	ProjectRuntime() = default;

	// Parse + validate the appmanifest at <tappPath> (every pak path
	// resolves MANIFEST-RELATIVE — a moved build directory keeps
	// working) and ADOPT it into the content registry (the base
	// providers + the record table). nullopt + err on any failure (the
	// caller fails the launch visibly).
	[[nodiscard]] static std::optional<ProjectRuntime> Load(
		const std::string& tappPath, std::string& err);

	// ---- THE DISCOVERY LANE (the .tapp-free form) ----------------------
	// Build the session from PAKS ALONE: every pak discovers through
	// its own "#ids" identity table (base providers + records; a pak
	// WITHOUT the blob joins as a path-serving provider only — the
	// legacy form, `requireIdentityBlob` false — so a viewer launch
	// over pre-identity content still resolves cross-pak paths).
	// The manifest stays EMPTY: the provider table is the authority
	// (PakNames() reflects it), the compiled-catalog/compiled-plans
	// gates do not apply (no deployment manifest to validate against),
	// and the entry point is the HOST's choice (a --scene name, the
	// first scene-bearing pak).
	[[nodiscard]] static std::optional<ProjectRuntime> OpenDiscovered(
		const std::vector<std::string>& pakPaths,
		std::string& err, bool requireIdentityBlob = false);
	// The pak provider names in MOUNT ORDER (the manifest's [paks] in
	// the .tapp lane; the discovered base providers in the discovery
	// lane) — the table every layer that iterates "the session's
	// paks" walks (ProjectScenes' adopt-or-open, the log lines).
	[[nodiscard]] std::vector<std::string> PakNames() const;

	// The parsed manifest (the entry pak/scene, the pak order, the id
	// table — see Engine/Asset/appmanifest.h). EMPTY in the discovery
	// lane (no deployment manifest — PakNames() + the registry are
	// the authority there).
	[[nodiscard]] const smvk::AppManifest& Manifest() const {
		return m_manifest;
	}
	// The manifest's own directory (the pak resolution base).
	[[nodiscard]] const std::string& BaseDir() const { return m_baseDir; }

	// ---- THE CONTENT REGISTRY (the resolution authority) ---------------
	// One registry per runtime façade: the identity table + the
	// provider set (base manifest paks + the host's overrides) + the
	// convergent byte lanes. The public seam for layers that resolve
	// by PATH (the cross-pak texture resolver, the scene-aware audio)
	// — everything the id form sees, the path form sees too.
	[[nodiscard]] tour::asset::ContentRegistry& Registry() {
		return m_registry;
	}
	[[nodiscard]] const tour::asset::ContentRegistry& Registry() const {
		return m_registry;
	}
	// The canonical record for a stable id (nullptr = the id is not
	// in this build — a version skew between the compiled header and
	// the packaged content; fail loudly, never guess). The RECORD is
	// identity; provider selection answers "who supplies it NOW".
	[[nodiscard]] const tour::asset::ContentRecord* Asset(
		uint64_t id) const {
		return m_registry.Find(id);
	}

	// The asset's bytes through the registry's effective provider
	// (OVERRIDE-AWARE: a mod/patch provider serving the record's path
	// wins — for the id form and the path form alike). Empty optional
	// + err on every refusal (unknown id, unservable kind, no provider
	// — the corrupt-owner case names the pak's own failure).
	std::optional<std::vector<uint8_t>> ReadAsset(uint64_t id,
		std::string& err);
	// The convergent PATH form: the kind + logical path resolve through
	// the SAME provider selection (a path that carries a record of
	// this kind resolves through that record's identity; a path with
	// no record resolves PATH-ONLY — mod-added content — through the
	// same provider precedence). The cross-pak texture resolver's lane.
	std::optional<std::vector<uint8_t>> ReadAssetByPath(uint8_t kind,
		const std::string& path, std::string& err);
	// A path with NO catalog identity (anim clips, loose dev files,
	// mod-added blobs): the same provider selection, probing every
	// VFS-addressable kind. The anim-clip fallback's lane — the last
	// host-side read that used to ride the overlay.
	std::optional<std::vector<uint8_t>> ReadAssetPath(
		const std::string& path, std::string& err);

	// ---- override providers (mods / patches / dev trees) ---------------
	// Registers an OVERRIDE provider with the registry: overrides
	// precede every base provider; among overrides EARLIER
	// REGISTRATION WINS (the host declares precedence by order).
	// `ReadAsset`/`ReadAssetByPath` see overrides IMMEDIATELY.
	bool AddPakOverride(const std::string& name,
		const std::string& pakPath, std::string& err) {
		return m_registry.AddPakOverride(name, pakPath, err);
	}
	bool AddLooseOverride(const std::string& name,
		const std::string& rootDir, std::string& err) {
		return m_registry.AddLooseOverride(name, rootDir, err);
	}

	// ---- the compiled-catalog contract (the flattened project.h) ----
	// The LAYOUT MIRROR of the generated project.h's AssetDesc (the
	// compiled game includes the generated header; this mirror lets
	// the runtime validate WITHOUT a header dependency — identical
	// member order/types, same TU ABI by construction; the sample
	// static_asserts the sizes agree).
	struct CompiledAssetDesc {
		uint64_t id = 0;
		uint8_t kind = 0;            // smvk::kCatalogKind*
		uint32_t pakIndex = 0;
		std::string_view path;
		std::string_view sceneName;  // reserved (deployment-side)
	};

	// Attaches the COMPILED catalog (the generated header's Assets
	// array + CatalogFingerprint — `project::Assets` is
	// `inline constexpr`, so the game passes a raw pointer + count).
	// Validates it against the DEPLOYED manifest:
	//   * the fingerprint line (when present) must equal the compiled
	//     fold — a mismatch is a version skew between the game binary
	//     and its content (recook the project or rebuild the game;
	//     fail loudly, never guess);
	//   * row-by-row: every compiled row must resolve to a manifest
	//     [ids] row with the same id, kind and path, and its pakIndex
	//     must index the manifest's [paks] (the mount-order table both
	//     sides share) naming the row's pak file.
	// A manifest with no fingerprint line (pre-contract) degrades to
	// the row-by-row form. Idempotent (re-attach re-validates).
	bool AttachCompiledCatalog(const CompiledAssetDesc* assets,
		size_t assetCount, uint64_t fingerprint, std::string& err);

	// True when AttachCompiledCatalog validated (the compiled catalog
	// is attached + agrees with the deployment manifest).
	[[nodiscard]] bool CompiledCatalogAttached() const {
		return m_compiledCatalogAttached;
	}

	// The pak's OPENED backend (the shared lazy cache ReadAsset uses —
	// one background preload + open per archive, kept for the runtime's
	// lifetime). The scene-instance layer (ProjectScenes) and any asset
	// cache resolve pak files through this; null + err on a missing or
	// corrupt archive. Thread note: the first open of a pak blocks on
	// its preload future — kick every pak you will need up front (see
	// ProjectScenes' constructor) if you want the reads overlapped.
	[[nodiscard]] std::shared_ptr<smvk::PakBackend> OpenPak(
		const std::string& pakFile, std::string& err);
	// The pak's PAGER (the SELECTIVE lane, SMP8): the header/footer/
	// TOC-only mount + page-granular reads through the async pager —
	// NO whole-image preload, ever. The registry's byte lanes use this
	// whenever a pak provider carries no adopted backend (a
	// scene-loaded pak serves from its already-resident image instead
	// — one copy of the bytes whatever lane opened first). One pager
	// per archive, released when a backend is adopted.
	[[nodiscard]] std::shared_ptr<smvk::PakPager> OpenPakPager(
		const std::string& pakFile, std::string& err);
	// The manifest's pak name by MOUNT INDEX (the compiled plans'
	// pakIndex key — the [paks] list order). Empty when out of range.
	[[nodiscard]] std::string ManifestPakName(size_t index) const;
	// ---- THE COMPILED-PLANS GATE (fail-closed) -----------------------
	// Validates the DEPLOYED paks against the generated
	// project_plans.h constants (PlansPakCount + PlansLayoutHash):
	// opens every manifest pak through the light SMP8 mount, folds
	// each pak's LAYOUT (the slot table + the mesh-plan join — the
	// shared engine fold; content bytes excluded), and compares. A
	// count or fold mismatch is a LOUD failure — redeploy the content
	// or rebuild the game; NEVER a silent fallback to the dynamic
	// resolver (the shipping acceptance contract). Call once at boot,
	// before any plan-driven load.
	bool AttachCompiledPlans(uint32_t compiledPakCount,
		const char* compiledLayoutHash, std::string& err);
	// Cheap cache presence (no open, no block): an ADOPTED backend is
	// registered for the pak (the registry's provider carries it —
	// reads then serve through the resident image).
	[[nodiscard]] bool HasPak(const std::string& pakFile) const {
		return m_registry.HasBackend(pakFile);
	}
	// Registers an ALREADY-OPENED backend under its pak file. The
	// apphost's --project launch preloads + mounts the manifest's paks
	// on its own; a runtime façade (or ProjectScenes) built alongside
	// that flow ADOPTS those backends instead of re-reading the
	// archives — one image per pak in memory, whatever constructs
	// first. No-op when the cache already carries the pak. The backend
	// must be a PakBackend opened over the pak the manifest names
	// (the caller's mount of that name guarantees it).
	void AdoptPakBackend(const std::string& pakFile,
		std::shared_ptr<smvk::PakBackend> backend);

	// Convenience: the manifest-relative (or absolute) pak path for a
	// row's pak file.
	[[nodiscard]] std::string PakPath(
		const std::string& pakFile) const;

private:
	smvk::AppManifest m_manifest;
	std::string m_baseDir;
	// THE CONTENT REGISTRY (Engine/Asset/content_registry.h): the
	// single resolution authority this façade fronts — the record
	// table (adopted from the manifest at Load), the provider set
	// (base manifest paks + the host's overrides) and BOTH byte
	// lanes (the adopted-backend image lane + the SMP8 pager lane).
	// The per-pak backend/pager caches the façade used to own live
	// HERE now (one image + one pager per archive, on the provider).
	tour::asset::ContentRegistry m_registry;
	bool m_compiledCatalogAttached = false;
};

// ---- THE TYPED CATALOG HELPER (the generated header's own types) --------
// The compiled catalog's Assets array is a GENERATED POD table
// (project.h); the validation mirror (CompiledAssetDesc) must stay
// LAYOUT-IDENTICAL to it. This helper does the proof ONCE (member
// order + types, static_asserted at every instantiation) and the
// pointer cast in ONE audited place — game code never
// reinterpret_cast's its generated data again (the sample's old
// hand-rolled proof lived at its call site).
template <typename Desc>
bool AttachCompiledCatalog(ProjectRuntime& rt,
	const Desc* assets, size_t assetCount, uint64_t fingerprint,
	std::string& err) {
	static_assert(sizeof(Desc)
			== sizeof(ProjectRuntime::CompiledAssetDesc),
		"the generated AssetDesc and the runtime mirror must stay"
		" layout-identical (size)");
	static_assert(offsetof(Desc, id)
			== offsetof(ProjectRuntime::CompiledAssetDesc, id)
		&& offsetof(Desc, kind)
			== offsetof(ProjectRuntime::CompiledAssetDesc, kind)
		&& offsetof(Desc, pakIndex)
			== offsetof(ProjectRuntime::CompiledAssetDesc, pakIndex)
		&& offsetof(Desc, path)
			== offsetof(ProjectRuntime::CompiledAssetDesc, path)
		&& offsetof(Desc, sceneName)
			== offsetof(ProjectRuntime::CompiledAssetDesc, sceneName),
		"the generated AssetDesc and the runtime mirror must stay"
		" layout-identical (member order)");
	// The null/count convention: nullptr valid only with count 0 (an
	// empty catalog — the row-by-row proof trivially passes).
	if (!assets && assetCount != 0) {
		err = "the compiled catalog is a null pointer with a nonzero"
			" count";
		return false;
	}
	return rt.AttachCompiledCatalog(
		reinterpret_cast<
			const ProjectRuntime::CompiledAssetDesc*>(assets),
		assetCount, fingerprint, err);
}

} // namespace app
