// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// ProjectRuntime — see ProjectRuntime.h. The pak reads ride the same
// background preload + PakBackend open the world loading uses (one
// open per archive, cached).

#include "ProjectRuntime.h"

#include "PakPreload.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace app {

namespace {

namespace fs = std::filesystem;

} // namespace

std::optional<ProjectRuntime> ProjectRuntime::Load(
	const std::string& tappPath, std::string& err) {
	smvk::AppManifest manifest;
	if (!smvk::LoadAppManifest(tappPath, manifest, err))
		return std::nullopt;
	ProjectRuntime rt;
	rt.m_manifest = std::move(manifest);
	rt.m_baseDir =
		fs::path(tappPath).lexically_normal().parent_path()
			.generic_string();
	return rt;
}

const smvk::AppManifestAsset* ProjectRuntime::Asset(
	const std::string& kind, const std::string& path) const {
	for (const smvk::AppManifestAsset& a : m_manifest.assets)
		if (a.kind == kind && a.path == path)
			return &a;
	return nullptr;
}

std::string ProjectRuntime::PakPath(const std::string& pakFile) const {
	fs::path p(pakFile);
	if (p.is_relative())
		p = fs::path(m_baseDir) / p;
	return p.lexically_normal().generic_string();
}

std::optional<std::vector<uint8_t>> ProjectRuntime::ReadAsset(
	uint64_t id, std::string& err) {
	err.clear();
	const smvk::AppManifestAsset* row = Asset(id);
	if (!row) {
		err = "unknown content id 0x" + std::to_string(id);
		return std::nullopt;
	}
	// KIND DISPATCH (the honest channel per kind — the pak's VFS
	// surface serves TEXTURE blobs only; meshes and scenes are
	// cluster-internal payloads with no VFS address):
	//   texture -> the embedded .ktx2 bytes (what every loader in the
	//              tree consumes — the v1 LoadTexture form);
	//   scene   -> scenes load BY IDENTITY through the apphost's world
	//              loading (the appmanifest entry / the pak's scene
	//              list), never as raw bytes;
	//   mesh    -> meshes ship INSIDE their scene's cluster (the v1
	//              pak-format boundary) — load the owning scene.
	if (row->kind != "texture") {
		err = "content id 0x" + std::to_string(id) + " is a '"
			+ row->kind + "' — only TEXTURE payloads are VFS-addressed"
				" bytes (scenes load by identity through the world"
				" loader; meshes ship inside their scene's cluster)";
		return std::nullopt;
	}
	// The row's pak, preloaded + opened once, cached for the runtime's
	// lifetime (the PakBackend owns the image; reads borrow it).
	auto cached = m_backends.find(row->pak);
	if (cached == m_backends.end()) {
		const std::string pakPath = PakPath(row->pak);
		PakPreload pre = KickPakPreload(pakPath).get();
		if (!pre.backend) {
			err = "the content pak failed: " + pre.error;
			return std::nullopt;
		}
		cached = m_backends.emplace(row->pak,
			std::move(pre.backend)).first;
	}
	auto bytes = cached->second->TryRead(row->path);
	if (!bytes || bytes->empty()) {
		err = "the pak '" + row->pak + "' does not serve '"
			+ row->path + "' (content id 0x" + std::to_string(id)
			+ " — a version skew between the build and the manifest?)";
		return std::nullopt;
	}
	return bytes;
}

} // namespace app
