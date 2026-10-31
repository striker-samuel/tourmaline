// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// ProjectRuntime: the apphost's PUBLIC project façade — the runtime
// half of content-as-code. The generated project.h gives game code
// STABLE IDS at compile time; the appmanifest's [ids] table (written
// by the build from the same asset-db registry) mirrors them at run
// time. This class bridges the two: resolve a stable id to its catalog
// row (kind/pak/path) and read the asset's bytes through its pak (the
// preloaded backend serves Texture + Sound-kind blobs by VFS path —
// the same channel the cross-pak texture resolution uses).
//
// The v1 shape of the epoch-2 spec's illustrative APIs:
//   const auto* row = runtime.Asset(project::textures::Stone);
//   auto bytes = runtime.ReadAsset(project::textures::Stone);
// (a GPU-upload entry point lands on top of ReadAsset when a game
// element needs it — the bytes are the standard .ktx2 payload every
// loader in the tree already consumes.)

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/pak.h>

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
	// working). nullopt + err on any failure (the caller fails the
	// launch visibly).
	[[nodiscard]] static std::optional<ProjectRuntime> Load(
		const std::string& tappPath, std::string& err);

	// The parsed manifest (the entry pak/scene, the pak order, the id
	// table — see Engine/Asset/appmanifest.h).
	[[nodiscard]] const smvk::AppManifest& Manifest() const {
		return m_manifest;
	}
	// The manifest's own directory (the pak resolution base).
	[[nodiscard]] const std::string& BaseDir() const { return m_baseDir; }

	// The catalog row for a stable id (nullptr = the id is not in this
	// build — a version skew between the compiled header and the
	// packaged content; fail loudly, never guess).
	[[nodiscard]] const smvk::AppManifestAsset* Asset(
		uint64_t id) const {
		return m_manifest.FindAsset(id);
	}
	// The row for a kind+path (the header's Catalog rows carry both —
	// the reverse lookup).
	[[nodiscard]] const smvk::AppManifestAsset* Asset(
		const std::string& kind, const std::string& path) const;

	// The asset's bytes through its pak: the archive preloads + opens
	// (cached per pak — one read per archive), then the backend serves
	// the VFS path. Empty optional = the row's pak is missing/corrupt
	// or the path is not served by it (err filled).
	std::optional<std::vector<uint8_t>> ReadAsset(uint64_t id,
		std::string& err);

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
	// Cheap cache presence (no open, no block).
	[[nodiscard]] bool HasPak(const std::string& pakFile) const {
		return m_backends.count(pakFile) != 0;
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
	// Lazily preloaded + opened backends, keyed by pak file (the
	// PakBackend owns its image; the cache keeps archives alive for
	// the runtime's lifetime).
	std::map<std::string, std::shared_ptr<smvk::PakBackend>>
		m_backends;
	bool m_compiledCatalogAttached = false;
};

} // namespace app
