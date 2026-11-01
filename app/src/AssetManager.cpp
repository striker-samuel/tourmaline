// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

// THE ASSET MANAGER FACADE - the implementation (see AssetManager.h
// for the surface contract + docs/content_as_code.md for the design).
//
// Composition, never duplication: every load rides an EXISTING layer -
// the [ids] join (ProjectRuntime), the pak backends (OpenPak's shared
// cache), the cluster deserializer (World::LoadClusterFromPak - the
// mesh/material/texture dependency graph), and the GI build/publish
// (WorldRenderer's BuildGiVolumeSetFromPaths + the adopt entries).

#include "AssetManager.h"

#include "ProjectRuntime.h"

#include <Engine/App/world.h>
#include <Engine/Asset/bake_asset.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Renderer/WorldRenderer.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>

namespace app {

namespace {

std::string HexId(uint64_t id) {
	std::ostringstream os;
	os << "0x" << std::hex << id;
	return os.str();
}

} // namespace

struct AssetManager::Impl {
	ProjectRuntime& runtime;
	::tour::World& world;
	smvk::ResourceManager* ctx;
	::engine::WorldRenderer* renderer;

	// MESH slots: a loaded mesh family (the .geometry asset's cluster
	// rows). The owning cluster loads ONCE per (pak, scene) - the
	// loaded-scene cache below - and every mesh key referencing it
	// shares it.
	struct MeshSlot {
		uint32_t generation = 1;
		bool alive = false;
		bool keepResident = true;
		uint64_t key = 0;             // the catalog id
		std::string pak;              // the owning pak (logical name)
		smvk::Cluster* cluster = nullptr;
		uint32_t worldMeshIndex = UINT32_MAX; // the base mesh's row
	};
	std::vector<MeshSlot> meshSlots;
	std::vector<uint32_t> meshFree;
	std::map<uint64_t, MeshHandle> meshByKey; // dedup per key
	// The loaded-scene cache: (pak, sceneIndex) -> the world cluster.
	// Loading a mesh's owning scene through the SAME world the apphost
	// uses means an ALREADY-loaded scene is found + shared (the
	// cluster search happens first in the world itself).
	std::map<std::pair<std::string, uint32_t>, smvk::Cluster*>
		sceneClusters;

	// GI slots: a loaded set's manifest rows (the identity), kept
	// resident while referenced. The renderer builds/publishes on
	// SetGI/SetGIBlend; both endpoints of a blend stay live here.
	struct GiSlot {
		uint32_t generation = 1;
		bool alive = false;
		uint64_t key = 0;
		std::vector<std::string> volumePaths;
	};
	std::vector<GiSlot> giSlots;
	std::vector<uint32_t> giFree;
	std::map<uint64_t, GiHandle> giByKey;

	// The batch result memory (one live batch at a time - the v1
	// contract: LoadMeshes returns a view the manager owns; the next
	// LoadMeshes/UnloadAllMeshes invalidates it).
	std::vector<MeshHandle> batchMemory;

	Impl(ProjectRuntime& rt, ::tour::World& w, smvk::ResourceManager* c,
			::engine::WorldRenderer* r)
		: runtime(rt), world(w), ctx(c), renderer(r) {}

	MeshSlot* MeshSlotOf(MeshHandle h) {
		const uint32_t idx = uint32_t(h.value & 0xFFFFFFFFu);
		const uint32_t gen = uint32_t(h.value >> 32);
		if (idx >= meshSlots.size())
			return nullptr;
		MeshSlot& s = meshSlots[idx];
		if (!s.alive || s.generation != gen)
			return nullptr;
		return &s;
	}

	GiSlot* GiSlotOf(GiHandle h) {
		const uint32_t idx = uint32_t(h.value & 0xFFFFFFFFu);
		const uint32_t gen = uint32_t(h.value >> 32);
		if (idx >= giSlots.size())
			return nullptr;
		GiSlot& s = giSlots[idx];
		if (!s.alive || s.generation != gen)
			return nullptr;
		return &s;
	}

	// Finds the world cluster that carries a mesh family: the cluster
	// search first (an already-loaded scene shares), else NULL. The
	// family's blob paths come from the pak READER's Mesh TOC (the
	// [ids] row's .geometry path is the URI PREFIX the blobs carry).
	smvk::Cluster* FindClusterWithMeshFamily(
		const std::vector<std::string>& familyPaths) {
		if (familyPaths.empty())
			return nullptr;
		// (a) The loaded-scene cache.
		for (auto& [k, cluster] : sceneClusters)
			if (cluster && clusterHasFamily(*cluster, familyPaths))
				return cluster;
		// (b) ANY world cluster (the apphost may have loaded the
		// owning scene outside this manager).
		for (const auto& c : world.clusters)
			if (c && clusterHasFamily(*c, familyPaths))
				return c.get();
		return nullptr;
	}

	static bool clusterHasFamily(smvk::Cluster& c,
			const std::vector<std::string>& familyPaths) {
		for (const std::string& p : familyPaths)
			if (c.FindMeshByUri(p))
				return true;
		return false;
	}

	// The pak's Mesh blobs under the family prefix (the join).
	static std::vector<std::string> FamilyBlobPaths(
		const smvk::PakBackend& backend,
		const std::string& familyPrefix) {
		std::vector<std::string> out;
		for (const auto& e : backend.Reader().Entries()) {
			if (e.kind != tour::asset::PakAssetKind::Mesh)
				continue;
			if (e.path.rfind(familyPrefix, 0) != 0)
				continue;
			out.push_back(e.path);
		}
		return out;
	}

	// Ensures the mesh family's owning pak-scene cluster is loaded
	// (the same deserialize the scene loader runs; null ctx = the
	// offline seam).
	smvk::Cluster* EnsureFamilyCluster(const smvk::AppManifestAsset& row,
		const std::vector<std::string>& familyPaths, std::string& err);
};

AssetManager::AssetManager(ProjectRuntime& runtime, ::tour::World& world,
		smvk::ResourceManager* ctx,
		::engine::WorldRenderer* renderer)
	: m_impl(new Impl{ runtime, world, ctx, renderer }) {}

AssetManager::~AssetManager() {
	delete m_impl;
}

// ---- the owning-cluster ensure -------------------------------------------

smvk::Cluster* AssetManager::Impl::EnsureFamilyCluster(
	const smvk::AppManifestAsset& row,
	const std::vector<std::string>& familyPaths, std::string& err) {
	if (!ctx) {
		err = "LoadMesh needs a GPU context (the mesh's owning scene"
			" cluster deserializes + uploads)";
		return nullptr;
	}
	if (smvk::Cluster* found = FindClusterWithMeshFamily(familyPaths))
		return found;
	// Load the pak's FIRST scene cluster (the v1 mesh join: mesh rows
	// carry no cooked-scene token; a multi-scene pak whose scenes
	// split one .geometry family refuses loudly - the subset-load
	// follow-up addresses it, see docs/content_as_code.md).
	std::string perr;
	std::shared_ptr<smvk::PakBackend> backend =
		runtime.OpenPak(row.pak, perr);
	if (!backend) {
		err = perr;
		return nullptr;
	}
	std::string lerr;
	const std::vector<smvk::PakSceneInfo> scenes =
		smvk::ListPakScenes(backend->Reader(), &lerr);
	if (scenes.empty()) {
		err = "the pak '" + row.pak + "' carries no scenes: " + lerr;
		return nullptr;
	}
	// Prefer the scene whose cluster carries the family (multi-scene
	// paks): deserialize is the only proof, so walk the scenes in
	// order - a deserialize is a load; on a miss, unload and try the
	// next. v1 keeps it to the FIRST scene (the common single-scene
	// project pak) and searches the world after.
	for (uint32_t si = 0; si < uint32_t(scenes.size()); ++si) {
		smvk::Cluster* loaded = nullptr;
		try {
			loaded = &world.LoadClusterFromPak(*ctx,
				backend->Reader(), si);
		}
		catch (const std::exception& e) {
			err = std::string("the owning cluster failed to load: ")
				+ e.what();
			return nullptr;
		}
		if (clusterHasFamily(*loaded, familyPaths)) {
			sceneClusters[{ row.pak, si }] = loaded;
			world.ResolveAndFixup_clusters();
			return loaded;
		}
		// Not this scene's family: drop the cluster again (a
		// load-time miss, the resolve folded nothing user-visible).
		world.UnloadCluster(*loaded);
	}
	err = "no scene in pak '" + row.pak + "' carries the mesh family '"
		+ row.path + "' (a multi-scene split family - the v1 join"
		" boundary, see docs/content_as_code.md)";
	return nullptr;
}

// ---- MESHES ---------------------------------------------------------------

MeshHandle AssetManager::LoadMesh(uint64_t key, std::string& err,
	const MeshLoadOptions& options) {
	err.clear();
	if (auto hit = m_impl->meshByKey.find(key);
			hit != m_impl->meshByKey.end()) {
		if (Impl::MeshSlot* s = m_impl->MeshSlotOf(hit->second)) {
			// keepResident upgrades only (never downgrades a shared
			// cluster's residency).
			if (options.keepResident)
				s->keepResident = true;
			return hit->second;
		}
		m_impl->meshByKey.erase(hit);
	}
	const smvk::AppManifestAsset* row = m_impl->runtime.Asset(key);
	if (!row) {
		err = "unknown content id " + HexId(key)
			+ " (a version skew between the compiled project.h and the"
			" packaged content?)";
		return {};
	}
	if (row->kind != "mesh") {
		err = "content id " + HexId(key) + " is a '" + row->kind
			+ "' - LoadMesh takes MESH ids";
		return {};
	}
	// The mesh family: the row's .geometry path is the URI PREFIX the
	// pak's Mesh blobs carry ("<path>#mesh.<i>/prim.<j>" + /lodN
	// siblings). The blob paths come from the READER's TOC (the join),
	// the pool handles from the loaded cluster's URI registry.
	std::string perr;
	std::shared_ptr<smvk::PakBackend> backend =
		m_impl->runtime.OpenPak(row->pak, perr);
	if (!backend) {
		err = perr;
		return {};
	}
	const std::string familyPrefix = row->path + "#";
	const std::vector<std::string> familyPaths =
		Impl::FamilyBlobPaths(*backend, familyPrefix);
	if (familyPaths.empty()) {
		err = "the pak '" + row->pak + "' carries no mesh blobs under '"
			+ familyPrefix + "' (a version skew between the catalog and"
			" the cooked content?)";
		return {};
	}
	smvk::Cluster* cluster =
		m_impl->EnsureFamilyCluster(*row, familyPaths, err);
	if (!cluster)
		return {};
	// The family's BASE mesh (no /lodN suffix) -> the cluster pool
	// row -> the WORLD mesh index (the spawn-side seam).
	uint32_t worldMeshIndex = UINT32_MAX;
	{
		// The world's meshLocations index = the resolve order; find
		// the family's base URI across the world's clusters (the
		// location tables are rebuilt per resolve - resolve fresh,
		// never cache, the world.cpp discipline).
		for (const std::string& p : familyPaths) {
			if (p.find("/lod") != std::string::npos)
				continue; // a LOD sibling, not the base
			const auto hit = cluster->FindMeshByUri(p);
			if (!hit)
				continue;
			for (uint32_t i = 0;
					i < m_impl->world.meshLocations.size(); ++i) {
				const tour::World::MeshLocation& loc =
					m_impl->world.meshLocations[i];
				if (loc.cluster == cluster && loc.handle == *hit) {
					worldMeshIndex = i;
					break;
				}
			}
			if (worldMeshIndex != UINT32_MAX)
				break;
		}
	}
	if (worldMeshIndex == UINT32_MAX) {
		err = "the mesh family '" + row->path
			+ "' loaded but no base mesh resolved (a cook anomaly)";
		return {};
	}
	// The slot.
	uint32_t index;
	uint32_t generation;
	if (!m_impl->meshFree.empty()) {
		index = m_impl->meshFree.back();
		m_impl->meshFree.pop_back();
		generation = m_impl->meshSlots[index].generation;
	}
	else {
		index = uint32_t(m_impl->meshSlots.size());
		m_impl->meshSlots.emplace_back();
		generation = m_impl->meshSlots.back().generation;
	}
	Impl::MeshSlot& slot = m_impl->meshSlots[index];
	slot.alive = true;
	slot.keepResident = options.keepResident;
	slot.key = key;
	slot.pak = row->pak;
	slot.cluster = cluster;
	slot.worldMeshIndex = worldMeshIndex;
	const MeshHandle handle{
		(uint64_t(generation) << 32) | index };
	m_impl->meshByKey.emplace(key, handle);
	return handle;
}

MeshBatch AssetManager::LoadMeshes(const uint64_t* keys, size_t count,
	std::string& err, const MeshLoadOptions& options) {
	err.clear();
	if (!keys && count != 0) {
		err = "LoadMeshes: a null key pointer with a nonzero count";
		return {};
	}
	m_impl->batchMemory.assign(count, MeshHandle{});
	// Dedup + per-pak grouping fall out of LoadMesh's key cache (the
	// owning cluster loads once per family; repeated keys short-
	// circuit to the cached handle).
	for (size_t i = 0; i < count; ++i) {
		std::string lerr;
		m_impl->batchMemory[i] = LoadMesh(keys[i], lerr, options);
		if (!m_impl->batchMemory[i]) {
			err = "the batch's key " + HexId(keys[i]) + " failed: "
				+ lerr;
			// The batch shape stays (failed slots read as invalid
			// handles) - the caller can inspect per-slot.
		}
	}
	return { m_impl->batchMemory.data(), m_impl->batchMemory.size() };
}

std::uint32_t AssetManager::MeshWorldIndex(MeshHandle handle) const {
	const Impl::MeshSlot* s = m_impl
		? m_impl->MeshSlotOf(handle)
		: nullptr;
	return s ? s->worldMeshIndex : UINT32_MAX;
}

bool AssetManager::MeshValid(MeshHandle handle) const {
	return m_impl && m_impl->MeshSlotOf(handle) != nullptr;
}

bool AssetManager::UnloadMesh(MeshHandle handle) {
	Impl::MeshSlot* s = m_impl ? m_impl->MeshSlotOf(handle) : nullptr;
	if (!s)
		return false;
	// v1 residency: keepResident clusters stay for the session (the
	// prefetch idiom); a non-resident drop releases the slot (the
	// cluster itself stays until the owning scene unloads - the
	// shared-cluster refcount follow-up).
	if (!s->keepResident)
		m_impl->meshByKey.erase(s->key);
	s->alive = false;
	s->cluster = nullptr;
	if (s->generation != UINT32_MAX) {
		++s->generation;
		if (s->generation != 0)
			m_impl->meshFree.push_back(
				uint32_t(handle.value & 0xFFFFFFFFu));
	}
	return true;
}

void AssetManager::UnloadAllMeshes() {
	if (!m_impl)
		return;
	for (auto& [key, handle] : m_impl->meshByKey)
		(void)UnloadMesh(handle);
	m_impl->meshByKey.clear();
	m_impl->batchMemory.clear();
}

// ---- GI BAKE SETS ---------------------------------------------------------

GiHandle AssetManager::LoadGI(uint64_t key, std::string& err) {
	err.clear();
	if (auto hit = m_impl->giByKey.find(key);
			hit != m_impl->giByKey.end()) {
		if (m_impl->GiSlotOf(hit->second))
			return hit->second;
		m_impl->giByKey.erase(hit);
	}
	const smvk::AppManifestAsset* row = m_impl->runtime.Asset(key);
	if (!row) {
		err = "unknown content id " + HexId(key)
			+ " (a version skew between the compiled project.h and the"
			" packaged content?)";
		return {};
	}
	if (row->kind != "gi") {
		err = "content id " + HexId(key) + " is a '" + row->kind
			+ "' - LoadGI takes GI ids (.givolumemanifest sets)";
		return {};
	}
	// The manifest bytes through the pak's VFS surface (the gi/*
	// payloads ride Texture-kind blobs).
	auto bytes = m_impl->runtime.ReadAsset(key, err);
	if (!bytes) {
		if (err.empty())
			err = "the GI manifest read failed (id " + HexId(key) + ")";
		return {};
	}
	// Parse the rows (SplitManifestRow: bare paths, legacy guid
	// columns, comments - the .cvmanifest row shape).
	std::vector<std::string> paths;
	{
		std::string cur;
		const auto flush = [&] {
			const std::string p = smvk::bake::SplitManifestRow(cur);
			if (!p.empty())
				paths.push_back(p);
			cur.clear();
		};
		for (char c : *bytes) {
			if (c == '\n' || c == '\r')
				flush();
			else
				cur.push_back(c);
		}
		flush();
	}
	if (paths.empty()) {
		err = "the GI manifest '" + row->path + "' lists no volumes";
		return {};
	}
	// The slot.
	uint32_t index;
	uint32_t generation;
	if (!m_impl->giFree.empty()) {
		index = m_impl->giFree.back();
		m_impl->giFree.pop_back();
		generation = m_impl->giSlots[index].generation;
	}
	else {
		index = uint32_t(m_impl->giSlots.size());
		m_impl->giSlots.emplace_back();
		generation = m_impl->giSlots.back().generation;
	}
	Impl::GiSlot& slot = m_impl->giSlots[index];
	slot.alive = true;
	slot.key = key;
	slot.volumePaths = std::move(paths);
	const GiHandle handle{ (uint64_t(generation) << 32) | index };
	m_impl->giByKey.emplace(key, handle);
	return handle;
}

bool AssetManager::SetGI(GiHandle gi) {
	const Impl::GiSlot* s = m_impl ? m_impl->GiSlotOf(gi) : nullptr;
	if (!s || !m_impl->renderer)
		return false;
	return m_impl->renderer->AdoptGiVolumeSet(s->volumePaths);
}

bool AssetManager::SetGIBlend(GiHandle from, GiHandle to, float alpha) {
	if (!m_impl || !m_impl->renderer)
		return false;
	const Impl::GiSlot* a = m_impl->GiSlotOf(from);
	const Impl::GiSlot* b = m_impl->GiSlotOf(to);
	if (!a || !b)
		return false;
	return m_impl->renderer->AdoptGiVolumeBlend(
		a->volumePaths, b->volumePaths, alpha);
}

bool AssetManager::UnloadGI(GiHandle gi) {
	Impl::GiSlot* s = m_impl ? m_impl->GiSlotOf(gi) : nullptr;
	if (!s)
		return false;
	// An active-set GI the renderer still publishes stays renderer-
	// owned (ClearGiBlend/the next SetGI retires it); the manager
	// only drops its reference.
	m_impl->giByKey.erase(s->key);
	s->alive = false;
	s->volumePaths.clear();
	if (s->generation != UINT32_MAX) {
		++s->generation;
		if (s->generation != 0)
			m_impl->giFree.push_back(
				uint32_t(gi.value & 0xFFFFFFFFu));
	}
	return true;
}

std::size_t AssetManager::LoadedMeshCount() const {
	return m_impl ? m_impl->meshByKey.size() : 0;
}

std::size_t AssetManager::LoadedGiCount() const {
	return m_impl ? m_impl->giByKey.size() : 0;
}

} // namespace app
