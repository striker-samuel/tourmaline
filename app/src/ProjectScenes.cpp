// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// ProjectScenes — see ProjectScenes.h. The loads ride the same
// background-preload + PakBackend opens the apphost's world loading
// uses; the pak->cluster step is World::LoadClusterFromPak (through
// the replaceable clusterLoader seam), and every load runs the shared
// PostLoadTail so an additive scene gets the entry scene's exact
// treatment (anim attach, cooked static flags, the resolve).

#include "ProjectScenes.h"

#include "PakPreload.h"
#include "PakWorldLoader.h"

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/asset_registry.h> // NormalizeAssetUri
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Utils/texture_loader.h>
#include <Engine/Vfs/vfs.h>

#include <algorithm>
#include <cstdio>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace app {

namespace {

// Pak stem -> the anim fallback's scene id (the loader's own copy of
// PakApp's StemOf; keeps this layer free of host includes).
std::string StemOf(const std::string& p) {
	const size_t slash = p.find_last_of("/\\");
	const std::string file = (slash == std::string::npos)
		? p : p.substr(slash + 1);
	const size_t dot = file.find_last_of('.');
	return (dot == std::string::npos || dot == 0)
		? file : file.substr(0, dot);
}

std::string HexId(uint64_t id) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "0x%llX",
		static_cast<unsigned long long>(id));
	return buf;
}

// The DEFAULT pak->cluster step: World::LoadClusterFromPak verbatim
// (DeserializeCluster + RegisterCluster; throws on failure — caught
// here, the staged uploads drained, surfaced as err + null).
smvk::Cluster* DefaultClusterLoader(smvk::World& world,
	smvk::ResourceManager* ctx, const smvk::PakReader& reader,
	uint32_t sceneIndex, smvk::TextureStreamer* streamer,
	std::shared_ptr<const std::vector<uint8_t>> imagePin,
	const smvk::ExternalTextureResolver* externalTexture,
	std::string& err) {
	if (!ctx) {
		// Null ctx is the GPU-less test seam: the real deserialize
		// needs an upload context, so the DEFAULT loader refuses it —
		// tests pair a null ctx with their own clusterLoader.
		err = "the default cluster load needs a ResourceManager"
			" (null ctx = the GPU-less test seam)";
		return nullptr;
	}
	try {
		return &world.LoadClusterFromPak(*ctx, reader, sceneIndex,
			streamer, imagePin, externalTexture);
	}
	catch (const std::exception& e) {
		// DeserializeCluster drains its own staged uploads on failure;
		// drain anything queued since, then fail the load.
		ctx->WaitForUploads();
		err = e.what();
		return nullptr;
	}
}

} // namespace

ProjectScenes::ProjectScenes(ProjectRuntime& runtime,
	smvk::OverlayVfs& contentVfs)
	: clusterLoader(DefaultClusterLoader)
	, m_runtime(runtime)
	, m_vfs(contentVfs) {
	// ---- the adopt-or-open pass (every manifest pak, one image each)
	// ----
	// (a) ADOPT what the host already mounted: a --project launch
	//     preloads + mounts the whole set at startup, and adopting
	//     those backends keeps ONE pak image per archive in memory
	//     whatever constructs first (the façade's cache gets them via
	//     AdoptPakBackend, so its reads serve through the same image).
	std::map<std::string, std::shared_ptr<smvk::PakBackend>> mounted;
	for (const smvk::VfsMount& m : contentVfs.Mounts()) {
		if (auto pb = std::dynamic_pointer_cast<smvk::PakBackend>(
				m.backend))
			mounted.emplace(m.name, std::move(pb));
	}
	// (b) KICK parallel preloads for the rest (the runtime's own cache
	//     counts as opened — HasPak, no double read).
	std::vector<std::string> missing;
	for (const std::string& pak : runtime.Manifest().paks) {
		if (mounted.count(pak) || runtime.HasPak(pak))
			continue;
		missing.push_back(pak);
	}
	std::map<std::string, std::shared_ptr<smvk::PakBackend>> opened;
	std::vector<std::future<PakPreload>> futures;
	for (const std::string& pak : missing)
		futures.push_back(KickPakPreload(runtime.PakPath(pak)));
	for (size_t i = 0; i < futures.size(); ++i) {
		// Join ALL futures regardless of failures (a std::future
		// destructor blocks — never leave one undrained).
		PakPreload pre = futures[i].get();
		if (pre.backend)
			opened.emplace(missing[i], std::move(pre.backend));
		else
			m_openError = "the project pak '" + missing[i]
				+ "' failed: " + pre.error;
	}
	// (c) Register + mount, in MANIFEST order (the mount order the
	//     overlay's cross-pak resolution depends on). First failure
	//     poisons the layer (Valid() == false) but keeps mounting —
	//     the error names the pak.
	for (const std::string& pak : runtime.Manifest().paks) {
		std::shared_ptr<smvk::PakBackend> backend;
		if (auto it = mounted.find(pak); it != mounted.end())
			backend = it->second;
		else if (auto it = opened.find(pak); it != opened.end())
			backend = it->second;
		else {
			std::string perr;
			backend = runtime.OpenPak(pak, perr); // cache hit — instant
			if (!backend && m_openError.empty())
				m_openError = "the project pak '" + pak
					+ "' failed: " + perr;
		}
		if (!backend)
			continue;
		runtime.AdoptPakBackend(pak, backend);
		if (!mounted.count(pak))
			contentVfs.Mount(backend, 10, pak);
	}
	if (m_openError.empty()) {
		std::cerr << "[Scenes] project ready: "
			<< runtime.Manifest().paks.size() << " pak(s) adopted/mounted"
			<< "\n";
	}
	else {
		std::cerr << "[Scenes] project pak pass failed: " << m_openError
			<< "\n";
	}
}

// ---- the id->scene join ------------------------------------------------

std::optional<ProjectScenes::SceneTarget> ProjectScenes::ResolveSceneTarget(
	uint64_t sceneId, std::string& err) {
	err.clear();
	if (auto cached = m_targets.find(sceneId); cached != m_targets.end())
		return cached->second;
	const smvk::AppManifestAsset* row = m_runtime.Asset(sceneId);
	if (!row) {
		err = "unknown content id " + HexId(sceneId)
			+ " (a version skew between the compiled project.h and the"
			" packaged content?)";
		return std::nullopt;
	}
	if (row->kind != "scene") {
		err = "content id " + HexId(sceneId) + " is a '" + row->kind
			+ "' — scene loading takes SCENE ids (the pak's cooked"
			" scene, by identity)";
		return std::nullopt;
	}
	std::string perr;
	std::shared_ptr<smvk::PakBackend> backend = m_runtime.OpenPak(
		row->pak, perr);
	if (!backend) {
		err = perr;
		return std::nullopt;
	}
	std::string lerr;
	const std::vector<smvk::PakSceneInfo> scenes =
		smvk::ListPakScenes(backend->Reader(), &lerr);
	if (scenes.empty()) {
		err = "the scene pak '" + row->pak + "' carries no scenes: "
			+ lerr;
		return std::nullopt;
	}
	SceneTarget t;
	t.pak = row->pak;
	if (!row->name.empty()) {
		// The recorded name (the build's ListPakScenes result — the
		// id->scene join). A mismatch is a build/runtime version skew
		// and fails loudly, never a guess.
		size_t idx = scenes.size();
		for (size_t i = 0; i < scenes.size(); ++i)
			if (scenes[i].name == row->name) {
				idx = i;
				break;
			}
		if (idx == scenes.size()) {
			err = "the cooked scene '" + row->name + "' is not in pak '"
				+ row->pak + "' (a build/runtime version skew — recook"
				" the project)";
			return std::nullopt;
		}
		t.sceneIndex = uint32_t(idx);
		t.sceneName = row->name;
	}
	else if (scenes.size() == 1) {
		// No recorded name (a row that predates the name token): a
		// single-scene pak is unambiguous — take it.
		t.sceneIndex = 0;
		t.sceneName = scenes[0].name;
	}
	else {
		err = "the scene pak '" + row->pak + "' carries "
			+ std::to_string(scenes.size()) + " scenes and the manifest"
			" row records no cooked name — recook (the build records"
			" the name since the scene-name token)";
		return std::nullopt;
	}
	m_targets.emplace(sceneId, t);
	return t;
}

// ---- scene instances ---------------------------------------------------

SceneHandle ProjectScenes::LoadScene(smvk::World& world,
	smvk::ResourceManager* ctx, smvk::TextureStreamer* streamer,
	uint64_t sceneId, std::string& err) {
	err.clear();
	// REPLACE semantics: every loaded project scene unloads first.
	// Each removal is a full world resolve (load-time only); the
	// loads that follow re-fold whatever survives.
	while (true) {
		SceneHandle any;
		for (uint32_t i = 0; i < m_slots.size(); ++i)
			if (m_slots[i].loaded) {
				any = SceneHandle{
					(uint64_t(m_slots[i].generation) << 32) | i };
				break;
			}
		if (!any)
			break;
		std::string rerr;
		if (!RemoveScene(any, world, rerr)) {
			err = "the replace unload failed: " + rerr;
			return {};
		}
	}
	return LoadInternal(world, ctx, streamer, sceneId, err);
}

SceneHandle ProjectScenes::LoadSceneAdditive(smvk::World& world,
	smvk::ResourceManager* ctx, smvk::TextureStreamer* streamer,
	uint64_t sceneId, std::string& err) {
	err.clear();
	return LoadInternal(world, ctx, streamer, sceneId, err);
}

SceneHandle ProjectScenes::LoadInternal(smvk::World& world,
	smvk::ResourceManager* ctx, smvk::TextureStreamer* streamer,
	uint64_t sceneId, std::string& err) {
	if (!m_openError.empty()) {
		err = "the project's paks did not all open: " + m_openError;
		return {};
	}
	std::optional<SceneTarget> target = ResolveSceneTarget(sceneId, err);
	if (!target)
		return {};
	std::string perr;
	std::shared_ptr<smvk::PakBackend> backend = m_runtime.OpenPak(
		target->pak, perr);
	if (!backend) {
		err = perr;
		return {};
	}
	// The cross-pak texture resolver (the same hook LoadProjectWorld
	// installs): a material URI with no blob in the scene's pak is
	// served from whatever mounted backend carries it. Null ctx (the
	// GPU-less seam) never resolves — the real deserialize that invokes
	// it never runs without a context.
	smvk::ResourceManager* resolverCtx = ctx;
	smvk::ExternalTextureResolver resolver =
		[resolverCtx, this](smvk::Cluster& c,
			const std::string& uri) -> smvk::TextureHandle {
		if (!resolverCtx) {
			std::cerr << "[Scenes] cross-pak texture unresolved (no"
				" upload context): " << uri << "\n";
			return {};
		}
		const std::string norm = smvk::NormalizeAssetUri(uri).Str();
		auto bytes = m_vfs.TryRead(norm.empty() ? uri : norm);
		if (!bytes || bytes->empty()) {
			std::cerr << "[Scenes] cross-pak texture unresolved: "
				<< uri << "\n";
			return {};
		}
		std::vector<smvk::KtxMemoryJob> jobs;
		jobs.push_back(smvk::KtxMemoryJob{
			0, bytes->data(), bytes->size(), uri });
		smvk::ParallelTextureStats stats;
		auto holders = smvk::LoadKtxTexturesParallelMemory(*resolverCtx,
			jobs, {}, &stats);
		const auto it = holders.find(0);
		if (it == holders.end()) {
			std::cerr << "[Scenes] cross-pak texture decode failed: "
				<< uri << "\n";
			return {};
		}
		const smvk::TextureHandle th = it->second;
		c.textures.create(std::move(it->second));
		c.RegisterTextureUri(uri, th);
		return th;
	};
	// The handle slot is taken BEFORE the load so the failure path
	// frees it (a failed load never leaves a phantom instance).
	SceneHandle handle = AllocHandle();
	SceneInstance* inst = Instance(handle);
	inst->contentId = sceneId;
	inst->pak = target->pak;
	inst->sceneName = target->sceneName;
	inst->sceneIndex = target->sceneIndex;
	smvk::Cluster* cluster = clusterLoader(world, ctx,
		backend->Reader(), target->sceneIndex, streamer,
		backend->Image(), &resolver, err);
	if (!cluster) {
		FreeHandle(handle);
		err = "the scene load for id " + HexId(sceneId) + " failed: "
			+ err;
		return {};
	}
	inst->cluster = cluster;
	// The shared post-load tail: anim attach, cooked static flags, the
	// resolve that folds the new cluster into the merged world scene
	// (the additive scene's nodes become world nodes exactly like the
	// entry scene's).
	PostLoadTail(world, StemOf(target->pak), m_vfs);
	if (ctx)
		ctx->FlushUploads();
	std::cerr << "[Scenes] scene " << HexId(sceneId) << " loaded (pak '"
		<< target->pak << "', cooked scene '"
		<< (target->sceneName.empty() ? "<unnamed>" : target->sceneName)
		<< "')\n";
	return handle;
}

bool ProjectScenes::RemoveScene(SceneHandle scene, smvk::World& world,
	std::string& err, std::vector<uint64_t>* releasedAssets) {
	err.clear();
	SceneInstance* inst = Instance(scene);
	if (!inst || !inst->loaded) {
		err = "stale or unknown scene handle " + HexId(scene.value)
			+ " (already removed?)";
		return false;
	}
	// The unload: the world erases the cluster this load created and
	// rebuilds the merged scene over the survivors — the scene's
	// nodes/rows vanish from the world exactly as they appeared.
	if (inst->cluster)
		world.UnloadCluster(*inst->cluster);
	std::vector<uint64_t> dropped = SceneReleaseAssets(scene);
	if (releasedAssets)
		*releasedAssets = std::move(dropped);
	FreeHandle(scene);
	return true;
}

bool ProjectScenes::IsLoaded(SceneHandle scene) const {
	const SceneInstance* inst = Instance(scene);
	return inst && inst->loaded;
}

uint32_t ProjectScenes::LoadedSceneCount() const {
	uint32_t n = 0;
	for (const SceneInstance& s : m_slots)
		if (s.loaded)
			++n;
	return n;
}

SceneHandle ProjectScenes::FindScene(uint64_t sceneId) const {
	for (uint32_t i = 0; i < m_slots.size(); ++i)
		if (m_slots[i].loaded && m_slots[i].contentId == sceneId)
			return SceneHandle{
				(uint64_t(m_slots[i].generation) << 32) | i };
	return {};
}

uint64_t ProjectScenes::SceneContentId(SceneHandle scene) const {
	const SceneInstance* inst = Instance(scene);
	return inst ? inst->contentId : 0;
}

smvk::Cluster* ProjectScenes::SceneCluster(SceneHandle scene) const {
	const SceneInstance* inst = Instance(scene);
	return inst ? inst->cluster : nullptr;
}

// ---- ownership-aware prefetch ------------------------------------------

bool ProjectScenes::Prefetch(uint64_t assetId, std::string& err) {
	err.clear();
	if (!m_openError.empty()) {
		err = "the project's paks did not all open: " + m_openError;
		return false;
	}
	const smvk::AppManifestAsset* row = m_runtime.Asset(assetId);
	if (!row) {
		err = "unknown content id " + HexId(assetId);
		return false;
	}
	// Warm the owning pak (the background preload + CRC open — the
	// expensive half of a first read). Scenes additionally resolve +
	// cache their SceneTarget (the name join), so the later load
	// skips straight to the deserialize.
	if (row->kind == "scene") {
		if (!ResolveSceneTarget(assetId, err))
			return false;
	}
	else {
		std::string perr;
		if (!m_runtime.OpenPak(row->pak, perr)) {
			err = perr;
			return false;
		}
	}
	++m_pins[assetId];
	return true;
}

bool ProjectScenes::Release(uint64_t assetId) {
	auto it = m_pins.find(assetId);
	if (it == m_pins.end() || it->second == 0)
		return false;
	if (--it->second == 0)
		m_pins.erase(it);
	return true;
}

bool ProjectScenes::IsPinned(uint64_t assetId) const {
	auto it = m_pins.find(assetId);
	return it != m_pins.end() && it->second > 0;
}

bool ProjectScenes::IsHeld(uint64_t assetId) const {
	return IsPinned(assetId) || m_assetRefs.count(assetId) != 0;
}

// ---- the ownership registry --------------------------------------------

void ProjectScenes::SceneAcquireAsset(SceneHandle scene, uint64_t assetId) {
	SceneInstance* inst = Instance(scene);
	if (!inst || !inst->loaded)
		return;
	// Idempotent per scene (a re-discover of the same scene must not
	// double the refcount).
	if (std::find(inst->ownedAssets.begin(), inst->ownedAssets.end(),
			assetId) != inst->ownedAssets.end())
		return;
	inst->ownedAssets.push_back(assetId);
	++m_assetRefs[assetId];
}

std::vector<uint64_t> ProjectScenes::SceneReleaseAssets(
	SceneHandle scene) {
	std::vector<uint64_t> dropped;
	SceneInstance* inst = Instance(scene);
	if (!inst || !inst->loaded)
		return dropped;
	for (uint64_t id : inst->ownedAssets) {
		auto it = m_assetRefs.find(id);
		if (it == m_assetRefs.end() || it->second == 0)
			continue; // registry drift — never underflow
		if (--it->second == 0) {
			m_assetRefs.erase(it);
			// Held while ANY owner lives or an explicit pin exists;
			// the drop list is exactly what decoded-form caches evict.
			if (!IsPinned(id))
				dropped.push_back(id);
		}
	}
	inst->ownedAssets.clear();
	return dropped;
}

// ---- the instance table ------------------------------------------------

ProjectScenes::SceneInstance* ProjectScenes::Instance(
	SceneHandle scene) {
	if (!scene)
		return nullptr;
	const uint32_t idx = scene.Index();
	if (idx >= m_slots.size())
		return nullptr;
	SceneInstance& s = m_slots[idx];
	if (!s.loaded || s.generation != scene.Generation())
		return nullptr;
	return &s;
}

const ProjectScenes::SceneInstance* ProjectScenes::Instance(
	SceneHandle scene) const {
	if (!scene)
		return nullptr;
	const uint32_t idx = scene.Index();
	if (idx >= m_slots.size())
		return nullptr;
	const SceneInstance& s = m_slots[idx];
	if (!s.loaded || s.generation != scene.Generation())
		return nullptr;
	return &s;
}

SceneHandle ProjectScenes::AllocHandle() {
	uint32_t index;
	if (!m_free.empty()) {
		index = m_free.back();
		m_free.pop_back();
	}
	else {
		index = uint32_t(m_slots.size());
		m_slots.emplace_back();
	}
	SceneInstance& slot = m_slots[index];
	slot.loaded = true;
	return SceneHandle{ (uint64_t(slot.generation) << 32) | index };
}

void ProjectScenes::FreeHandle(SceneHandle scene) {
	const uint32_t idx = scene.Index();
	if (idx >= m_slots.size())
		return;
	SceneInstance& slot = m_slots[idx];
	slot.loaded = false;
	slot.contentId = 0;
	slot.pak.clear();
	slot.sceneName.clear();
	slot.sceneIndex = 0;
	slot.cluster = nullptr;
	slot.ownedAssets.clear();
	// The generation bump is the stale-handle guarantee: a freed slot
	// mints generation+1 on its next allocation, so every copy of the
	// old handle fails validation forever. A wrapped generation
	// retires the slot instead of reusing it (the all-zero value is
	// reserved for "no scene").
	if (slot.generation == UINT32_MAX)
		return;
	++slot.generation;
	if (slot.generation == 0) {
		slot.generation = UINT32_MAX; // wrapped — retire, never reuse
		return;
	}
	m_free.push_back(idx);
}

} // namespace app
