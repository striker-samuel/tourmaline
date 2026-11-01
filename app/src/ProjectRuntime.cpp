// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// ProjectRuntime — see ProjectRuntime.h. The pak reads ride the same
// background preload + PakBackend open the world loading uses (one
// open per archive, cached).

#include "ProjectRuntime.h"

#include "PakPreload.h"

#include <filesystem>
#include <fstream>
#include <iostream>
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

std::shared_ptr<smvk::PakBackend> ProjectRuntime::OpenPak(
	const std::string& pakFile, std::string& err) {
	err.clear();
	// The pak, preloaded + opened once, cached for the runtime's
	// lifetime (the PakBackend owns the image; reads borrow it). The
	// first open blocks on the preload future — see the header's
	// thread note.
	auto cached = m_backends.find(pakFile);
	if (cached == m_backends.end()) {
		const std::string pakPath = PakPath(pakFile);
		PakPreload pre = KickPakPreload(pakPath).get();
		if (!pre.backend) {
			err = "the content pak '" + pakFile + "' failed: "
				+ pre.error;
			return nullptr;
		}
		cached = m_backends.emplace(pakFile,
			std::move(pre.backend)).first;
	}
	return cached->second;
}

void ProjectRuntime::AdoptPakBackend(const std::string& pakFile,
	std::shared_ptr<smvk::PakBackend> backend) {
	if (pakFile.empty() || !backend)
		return;
	// First writer wins; a present entry stays untouched (the cache's
	// own OpenPak result and an adopted host backend are the same
	// archive by contract).
	m_backends.emplace(pakFile, std::move(backend));
}

std::shared_ptr<smvk::PakPager> ProjectRuntime::OpenPakPager(
	const std::string& pakFile, std::string& err) {
	err.clear();
	// Lazy cache, one pager per archive (the pager owns its file
	// handle + worker thread; the mount is the SMP8 small one —
	// header/footer/TOC only, no blob bytes, no preload future).
	auto cached = m_pagers.find(pakFile);
	if (cached == m_pagers.end()) {
		auto pager = std::make_shared<smvk::PakPager>(
			PakPath(pakFile));
		if (!pager->Valid()) {
			err = "the content pak '" + pakFile + "' failed to open"
				" for paging: " + pager->OpenError();
			return nullptr;
		}
		cached = m_pagers.emplace(pakFile, std::move(pager)).first;
	}
	return cached->second;
}

bool ProjectRuntime::AttachCompiledCatalog(
	const CompiledAssetDesc* assets, size_t assetCount,
	uint64_t fingerprint, std::string& err) {
	err.clear();
	m_compiledCatalogAttached = false;
	if (!assets && assetCount != 0) {
		err = "the compiled catalog is a null pointer with a nonzero"
			" count";
		return false;
	}
	// The fast gate: the manifest's fingerprint line (when present)
	// must equal the compiled fold. The row-by-row pass below is the
	// proof; this is the loud early answer.
	if (m_manifest.catalogFingerprint != 0
			&& m_manifest.catalogFingerprint != fingerprint) {
		char buf[160];
		std::snprintf(buf, sizeof(buf),
			"catalog fingerprint mismatch: the game was compiled"
			" against 0x%llX, the deployed content carries 0x%llX"
			" (recook the project or rebuild the game)",
			static_cast<unsigned long long>(fingerprint),
			static_cast<unsigned long long>(
				m_manifest.catalogFingerprint));
		err = buf;
		return false;
	}
	// The proof: every compiled row resolves through the manifest.
	for (size_t i = 0; i < assetCount; ++i) {
		const CompiledAssetDesc& a = assets[i];
		const smvk::AppManifestAsset* row = m_manifest.FindAsset(a.id);
		if (!row) {
			err = "the compiled catalog carries id 0x"
				+ std::to_string(a.id)
				+ " but the deployed manifest does not (a version"
				" skew — recook the project or rebuild the game)";
			return false;
		}
		uint8_t kind = 0;
		if (!smvk::CatalogKindFromString(row->kind, kind) || kind != a.kind) {
			err = "the compiled catalog's id 0x" + std::to_string(a.id)
				+ " is kind " + std::to_string(a.kind)
				+ " but the deployed manifest says '"
				+ (row->kind.empty() ? "?" : row->kind) + "'";
			return false;
		}
		if (row->path != a.path) {
			err = "the compiled catalog's id 0x" + std::to_string(a.id)
				+ " moved: '" + std::string(a.path) + "' compiled,"
				" '" + row->path + "' deployed";
			return false;
		}
		if (a.pakIndex >= m_manifest.paks.size()) {
			err = "the compiled catalog's id 0x" + std::to_string(a.id)
				+ " names pak index " + std::to_string(a.pakIndex)
				+ " but the manifest lists only "
				+ std::to_string(m_manifest.paks.size())
				+ " pak(s)";
			return false;
		}
		if (row->pak != m_manifest.paks[a.pakIndex]) {
			err = "the compiled catalog's id 0x" + std::to_string(a.id)
				+ " names pak '" + m_manifest.paks[a.pakIndex]
				+ "' (index " + std::to_string(a.pakIndex)
				+ ") but the manifest serves it from '"
				+ row->pak + "' (a mount-order skew)";
			return false;
		}
	}
	if (m_manifest.catalogFingerprint == 0)
		std::cerr << "[ProjectRuntime] the manifest carries no catalog"
			" fingerprint line (a pre-contract deployment) - the"
			" compiled catalog validated row-by-row only\n";
	m_compiledCatalogAttached = true;
	return true;
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
	// surface serves TEXTURE, SOUND, SHADER and GI blobs; meshes and
	// scenes are cluster-internal payloads with no VFS address):
	//   texture -> the embedded .ktx2 bytes (what every loader in the
	//              tree consumes — the v1 LoadTexture form);
	//   sound   -> the raw .wav bytes (AudioEngine::LoadWavFromMemory
	//              consumes them verbatim);
	//   shader  -> the raw compiled bytecode (the project-AUTHORED
	//              .spv — validated SPIR-V, distinct from the
	//              engine-native set; serve through smvk::shaders);
	//   gi      -> the .givolumemanifest bytes (a baked GI SET's row
	//              list — AssetManager::LoadGI parses it and loads
	//              the .givolume payloads it names, also VFS-served);
	//   scene   -> scenes load BY IDENTITY through the apphost's world
	//              loading (the appmanifest entry / the pak's scene
	//              list), never as raw bytes;
	//   mesh    -> meshes ship INSIDE their scene's cluster (the v1
	//              pak-format boundary) — load the owning scene.
	if (row->kind != "texture" && row->kind != "sound"
			&& row->kind != "shader" && row->kind != "gi") {
		err = "content id 0x" + std::to_string(id) + " is a '"
			+ row->kind + "' — only TEXTURE, SOUND, SHADER and GI"
				" payloads are VFS-addressed bytes (scenes load by"
				" identity through the world loader; meshes ship"
				" inside their scene's cluster)";
		return std::nullopt;
	}
	// The read lane (the SELECTIVE startup contract): a pak the
	// SCENE lane already opened (or the apphost adopted) serves from
	// its resident image — one copy of the bytes whatever opened
	// first. A pak nobody preloaded goes through the PAGER: the SMP8
	// small mount + ONE positional page read for this asset. NO
	// whole-image preload ever happens on the asset-read path.
	const smvk::PakTocEntry* entry = nullptr;
	smvk::PakAssetKind entryKind = smvk::PakAssetKind::Texture;
	std::optional<std::vector<uint8_t>> bytes;
	{
		const auto backendIt = m_backends.find(row->pak);
		if (backendIt != m_backends.end()) {
			bytes = backendIt->second->TryRead(row->path);
		}
		else {
			// The pager lane. The row's kind maps to the pak's VFS
			// kinds ("gi" manifests ride Texture-kind blobs).
			smvk::PakAssetKind kind = smvk::PakAssetKind::Texture;
			if (row->kind == "sound")
				kind = smvk::PakAssetKind::Sound;
			else if (row->kind == "shader")
				kind = smvk::PakAssetKind::Shader;
			std::string perr;
			std::shared_ptr<smvk::PakPager> pager =
				OpenPakPager(row->pak, perr);
			if (!pager) {
				err = perr;
				return std::nullopt;
			}
			entry = pager->Reader().Find(kind, row->path);
			if (entry) {
				entryKind = kind;
				smvk::PakPager::PinnedPage page =
					pager->Read(*entry, perr);
				if (page)
					bytes = smvk::PakServeVfsBytes(entryKind,
						page.bytes(), row->path);
				else
					err = perr;
			}
		}
	}
	if (!bytes || bytes->empty()) {
		if (err.empty())
			err = "the pak '" + row->pak + "' does not serve '"
				+ row->path + "' (content id 0x"
				+ std::to_string(id)
				+ " — a version skew between the build and the manifest?)";
		return std::nullopt;
	}
	return bytes;
}

} // namespace app
