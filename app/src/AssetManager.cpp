// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

// THE ASSET MANAGER FACADE - the implementation (see AssetManager.h
// for the surface contract + docs/content_as_code.md for the design).
//
// Composition, never duplication: every load rides an EXISTING layer -
// the AssetStore (the shared-resource ownership: the found-or-loaded
// carrier acquire/release + the asset-side refs that keep carriers
// alive across scene removals), the [ids] join (ProjectRuntime), the
// pak backends (OpenPak's shared cache), the cluster deserializer
// (through the store's cluster-source seam), and the GI build/publish
// (WorldRenderer's BuildGiVolumeSetFromPaths + the adopt entries).

#include "AssetManager.h"

#include "AssetStore.h"
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

	// The shared-resource ownership layer: the found-or-loaded
	// carrier acquire/release, the asset-side refs (the
	// park-on-unload bridge), the cluster-source seam.
	AssetStore store;

	// MESH slots: a loaded mesh family (the .geometry asset's cluster
	// rows). The carrier is acquired ONCE through the store (its refs
	// keep it alive across scene removals) and every mesh key
	// referencing it shares it.
	struct MeshSlot {
		uint32_t generation = 1;
		bool alive = false;
		bool keepResident = true;
		uint64_t key = 0;             // the catalog id
		std::string pak;              // the owning pak (logical name)
		smvk::Cluster* cluster = nullptr;       // store-held (refs keep it alive)
		smvk::ClusterStaticMeshHandle mesh{};   // the family's BASE mesh row
	};
	std::vector<MeshSlot> meshSlots;
	std::vector<uint32_t> meshFree;
	std::map<uint64_t, MeshHandle> meshByKey; // dedup per key

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
		: runtime(rt), world(w), ctx(c), renderer(r), store(rt, c) {}

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

	// The slot's CURRENT world mesh row, resolved FRESH against the
	// live meshLocations (the "resolve fresh, never cache" discipline
	// - the tables rebuild per resolve, and a PARKED carrier has no
	// row at all: UINT32_MAX then, the honest no-current-mapping
	// answer).
	uint32_t WorldMeshIndexOf(const MeshSlot& s) const {
		if (!s.cluster || !s.mesh.valid())
			return UINT32_MAX;
		for (uint32_t i = 0; i < world.meshLocations.size(); ++i) {
			const tour::World::MeshLocation& loc =
				world.meshLocations[i];
			if (loc.cluster == s.cluster && loc.handle == s.mesh)
				return i;
		}
		return UINT32_MAX;
	}
};

AssetManager::AssetManager(ProjectRuntime& runtime, ::tour::World& world,
		smvk::ResourceManager* ctx,
		::engine::WorldRenderer* renderer)
	: m_impl(new Impl{ runtime, world, ctx, renderer }) {}

AssetManager::~AssetManager() {
	delete m_impl;
}

AssetStore& AssetManager::Store() {
	return m_impl->store;
}

// ---- MESHES ---------------------------------------------------------------

MeshHandle AssetManager::LoadMesh(uint64_t key, std::string& err,
	const MeshLoadOptions& options) {
	err.clear();
	if (auto hit = m_impl->meshByKey.find(key);
			hit != m_impl->meshByKey.end()) {
		if (Impl::MeshSlot* s = m_impl->MeshSlotOf(hit->second)) {
			// keepResident upgrades only (never downgrades a shared
			// carrier's residency) — and the upgrade must reach the
			// STORE (the session ref): the slot flag and the store's
			// ledger stay in lockstep, or an unload would drop a
			// carrier the facade still promises to hold.
			if (options.keepResident && !s->keepResident) {
				s->keepResident = true;
				if (s->cluster)
					m_impl->store.MakeResident(s->cluster);
			}
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
		AssetStore::FamilyBlobPaths(*backend, familyPrefix);
	if (familyPaths.empty()) {
		err = "the pak '" + row->pak + "' carries no mesh blobs under '"
			+ familyPrefix + "' (a version skew between the catalog and"
			" the cooked content?)";
		return {};
	}
	// ACQUIRE the family's carrier through the store: found (held),
	// adopted from a world-registered scene cluster, or loaded through
	// the cluster-source seam - with asset-side refs that keep it
	// alive across scene removals (the park bridge).
	smvk::Cluster* cluster = m_impl->store.AcquireFamilyCluster(
		m_impl->world, *row, familyPaths, options.keepResident, err);
	if (!cluster)
		return {};
	// The family's BASE mesh (no /lodN suffix) -> the cluster pool row
	// (the spawn-side seam resolves the WORLD index fresh per query -
	// see WorldMeshIndexOf; the slot keeps the stable carrier row).
	smvk::ClusterStaticMeshHandle baseMesh{};
	for (const std::string& p : familyPaths) {
		if (p.find("/lod") != std::string::npos)
			continue; // a LOD sibling, not the base
		if (auto hit = cluster->FindMeshByUri(p)) {
			baseMesh = *hit;
			break;
		}
	}
	if (!baseMesh.valid()) {
		err = "the mesh family '" + row->path
			+ "' loaded but carries no base mesh (a cook anomaly)";
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
	slot.mesh = baseMesh;
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
	// carrier is acquired once per family; repeated keys short-circuit
	// to the cached handle).
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
	// Fresh resolution against the LIVE world tables (a cached index
	// is stale the moment any scene changes; a parked carrier - its
	// scene was removed while handles stay - has no row at all).
	return s ? m_impl->WorldMeshIndexOf(*s) : UINT32_MAX;
}

bool AssetManager::MeshValid(MeshHandle handle) const {
	return m_impl && m_impl->MeshSlotOf(handle) != nullptr;
}

bool AssetManager::UnloadMesh(MeshHandle handle) {
	Impl::MeshSlot* s = m_impl ? m_impl->MeshSlotOf(handle) : nullptr;
	if (!s)
		return false;
	// Release the slot's store reference FIRST (the cluster pointer is
	// the lookup key - identity only). The carrier drops when its last
	// ref releases: a PARKED carrier is destroyed by the world on the
	// last ref (deferred retirement), a registered one stays with its
	// scene owner, and keepResident's session ref holds it until
	// UnloadAllMeshes/teardown either way.
	if (s->cluster) {
		m_impl->store.ReleaseFamilyCluster(s->cluster);
		s->cluster = nullptr;
	}
	s->mesh = {};
	if (!s->keepResident)
		m_impl->meshByKey.erase(s->key);
	s->alive = false;
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
	// The session (keepResident) refs go with the teardown: the last
	// one per carrier releases it for destruction (a parked carrier
	// dies here; a registered one stays with its scene owner).
	m_impl->store.ReleaseAll();
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
