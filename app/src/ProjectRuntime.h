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
};

} // namespace app
