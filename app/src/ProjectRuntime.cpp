// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// ProjectRuntime — see ProjectRuntime.h (the SESSION over the content
// registry). The manifest is the identity source (the .tapp
// compatibility lane); the registry owns resolution + both byte
// lanes. The whole-image preloads ride the same KickPakPreload the
// apphost's world loading uses; an opened backend is ADOPTED into
// the registry so every read — id form AND path form — serves
// through the same image.

#include "ProjectRuntime.h"

#include <Engine/Asset/asset_plan.h>

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
	// Adopt the manifest into the registry (the base providers —
	// every listed pak in manifest order — + the record table). A
	// manifest whose rows do not adopt (an unlisted pak, a duplicate
	// identity, an unnormalizable path) fails the launch visibly.
	if (!rt.m_registry.AdoptManifest(rt.m_manifest, rt.m_baseDir, err)) {
		err = "the manifest '" + tappPath + "' does not adopt into the"
			" content registry: " + err;
		return std::nullopt;
	}
	return rt;
}

std::optional<ProjectRuntime> ProjectRuntime::OpenDiscovered(
	const std::vector<std::string>& pakPaths, std::string& err,
	bool requireIdentityBlob) {
	ProjectRuntime rt;
	// Every pak joins through its OWN identity table (the light
	// mount + one page read; the pager stays cached on the provider).
	// The provider NAME is the pak file's name — the mount identity
	// every layer (logs, ProjectScenes' adopt-or-open, scene targets)
	// addresses it by, exactly like a manifest pak name.
	for (const std::string& pakPath : pakPaths) {
		const std::string name =
			fs::path(pakPath).filename().generic_string();
		if (!rt.m_registry.DiscoverPak(name, pakPath, err,
				requireIdentityBlob)) {
			err = "the pak '" + pakPath + "' does not join the content"
				" session: " + err;
			return std::nullopt;
		}
	}
	return rt;
}

std::vector<std::string> ProjectRuntime::PakNames() const {
	// The registry's BASE providers in order — the manifest's [paks]
	// list in the .tapp lane (AdoptManifest registered them in
	// manifest order), the discovered base providers in the discovery
	// lane. ONE authority: whatever iterated the manifest's pak list
	// iterates this (the manifest itself is a compatibility surface).
	std::vector<std::string> names;
	for (const tour::asset::ContentRegistry::ProviderInfo& p :
			m_registry.ProviderInfos())
		if (p.base)
			names.push_back(p.name);
	return names;
}

std::string ProjectRuntime::PakPath(const std::string& pakFile) const {
	// The registry's provider SOURCE first: the .tapp lane's
	// manifest-relative resolution AND the discovery lane's given
	// pak path both land on the provider at adoption/discovery time —
	// one answer whatever lane built the session.
	for (const tour::asset::ContentRegistry::ProviderInfo& p :
			m_registry.ProviderInfos())
		if (p.base && p.name == pakFile
				&& p.type == tour::asset::ContentProvider::Type::Pak)
			return p.source;
	// The manifest-relative fallback (a pak the manifest names but
	// the registry does not carry — possible only mid-adoption
	// anomalies; the callers fail loudly on the missing archive).
	fs::path p(pakFile);
	if (p.is_relative())
		p = fs::path(m_baseDir) / p;
	return p.lexically_normal().generic_string();
}

std::shared_ptr<smvk::PakBackend> ProjectRuntime::OpenPak(
	const std::string& pakFile, std::string& err) {
	err.clear();
	// The registry's adopted backend first: an archive opened (or
	// adopted from the host) once stays ONE image for the runtime's
	// lifetime, whatever asks next.
	if (std::shared_ptr<smvk::PakBackend> cached =
			m_registry.BackendFor(pakFile))
		return cached;
	const std::string pakPath = PakPath(pakFile);
	PakPreload pre = KickPakPreload(pakPath).get();
	if (!pre.backend) {
		err = "the content pak '" + pakFile + "' failed: "
			+ pre.error;
		return nullptr;
	}
	// The adoption registers the resident image with the registry's
	// pak provider (its pager — if a byte read lazily opened one — is
	// released; the image serves from here on).
	m_registry.AdoptBackend(pakFile, pre.backend);
	return pre.backend;
}

void ProjectRuntime::AdoptPakBackend(const std::string& pakFile,
	std::shared_ptr<smvk::PakBackend> backend) {
	// First writer wins; a present backend stays untouched (the
	// façade's own OpenPak result and an adopted host backend are the
	// same archive by contract). Nullptr/no-provider = a no-op (the
	// loud forms stay with the callers that can report).
	m_registry.AdoptBackend(pakFile, std::move(backend));
}

std::shared_ptr<smvk::PakPager> ProjectRuntime::OpenPakPager(
	const std::string& pakFile, std::string& err) {
	err.clear();
	// The registry's provider pager (the light SMP8 mount, cached on
	// the provider until a backend is adopted).
	std::shared_ptr<smvk::PakPager> pager =
		m_registry.PagerFor(pakFile, err);
	if (!pager && err.empty())
		err = "no pak provider named '" + pakFile
			+ "' (the manifest does not list it)";
	return pager;
}

std::string ProjectRuntime::ManifestPakName(size_t index) const
{
	if (index >= m_manifest.paks.size())
		return {};
	return m_manifest.paks[index];
}

bool ProjectRuntime::AttachCompiledPlans(uint32_t compiledPakCount,
	const char* compiledLayoutHash, std::string& err)
{
	err.clear();
	if (compiledPakCount != m_manifest.paks.size()) {
		err = "the compiled plans name " + std::to_string(
				compiledPakCount) + " pak(s) but the manifest lists "
			+ std::to_string(m_manifest.paks.size())
			+ " — a version skew between the build and the deployed"
			" content; redeploy or rebuild the game (never a"
			" dynamic fallback)";
		return false;
	}
	// Open every manifest pak through the light mount + fold its
	// LAYOUT (the shared engine fold — the same one the generator
	// used; content bytes excluded by design).
	std::vector<std::string> names;
	std::vector<std::string> fingerprints;
	names.reserve(m_manifest.paks.size());
	fingerprints.reserve(m_manifest.paks.size());
	for (const std::string& pak : m_manifest.paks) {
		std::string perr;
		std::shared_ptr<smvk::PakPager> pager = OpenPakPager(pak, perr);
		if (!pager) {
			err = perr;
			return false;
		}
		names.push_back(pak);
		fingerprints.push_back(
			smvk::PakLayoutFingerprint(pager->Reader()));
	}
	const std::string deployed = smvk::FoldPakLayouts(names.data(),
		fingerprints.data(), names.size());
	if (deployed != std::string(compiledLayoutHash
			? compiledLayoutHash : "")) {
		err = "the deployed paks' LAYOUT does not match the compiled"
			" plans (compiled " + std::string(compiledLayoutHash
				? compiledLayoutHash : "<empty>")
			+ ", deployed " + deployed
			+ ") — the content was repacked with a different layout;"
			" redeploy the matching generation or rebuild the game"
			" (never a dynamic fallback)";
		return false;
	}
	return true;
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
				+ " '" + row->path + "' deployed";
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
	// THE CONVERGENT ID FORM: the registry resolves the record's path
	// against every provider (overrides first), so a mod/patch/dev
	// provider serving the path wins for the compiled game exactly as
	// it does for the serialized-content path form. The kind gates
	// (scene/mesh load by identity; texture/sound/shader/gi serve
	// bytes) + both byte lanes (adopted image, else the SMP8 pager)
	// live in the registry.
	return m_registry.Read(id, err);
}

std::optional<std::vector<uint8_t>> ProjectRuntime::ReadAssetByPath(
	uint8_t kind, const std::string& path, std::string& err) {
	// THE CONVERGENT PATH FORM: a path that carries a record of this
	// kind resolves through THAT record's identity; a path with no
	// record resolves PATH-ONLY (mod-added content) — the same
	// provider precedence either way. This is the cross-pak texture
	// resolver's lane (a material URI with no blob in the loading
	// pak), the scene-aware audio's future byte lane, and the form
	// that must AGREE with ReadAsset's answer (the architecture's
	// first acceptance contract).
	return m_registry.Read(kind, path, err);
}

} // namespace app
