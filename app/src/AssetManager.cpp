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
		tour::Cluster* cluster = nullptr;       // store-held (refs keep it alive)
		tour::ClusterStaticMeshHandle mesh{};   // the family's BASE mesh row
	};
	std::vector<MeshSlot> meshSlots;
	std::vector<uint32_t> meshFree;
	std::map<uint64_t, MeshHandle> meshByKey; // dedup per key
	// The compiled-plan lane's dedup: (pakIndex << 32 | baseSlot) ->
	// the handle (a parallel key into the same slots).
	std::map<uint64_t, MeshHandle> meshByPlan;

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
	: m_impl(new Impl{ runtime, world, ctx, renderer }) {
	world.SetGameAssets(this);
}

AssetManager::~AssetManager() {
	if (m_impl && m_impl->world.gameAssets_ == this)
		m_impl->world.SetGameAssets(nullptr);
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
	// The canonical record (the registry's identity table — the
	// manifest [ids] join's successor surface).
	const tour::asset::ContentRecord* record =
		m_impl->runtime.Asset(key);
	if (!record) {
		err = "unknown content id " + HexId(key)
			+ " (a version skew between the compiled project.h and the"
			" packaged content?)";
		return {};
	}
	if (record->kind != smvk::kCatalogKindMesh) {
		err = "content id " + HexId(key) + " is a '"
			+ smvk::CatalogKindToString(record->kind)
			+ "' - LoadMesh takes MESH ids";
		return {};
	}
	// The mesh family: the record's .geometry path is the URI PREFIX the
	// pak's Mesh blobs carry ("<path>#mesh.<i>/prim.<j>" + /lodN
	// siblings). The blob paths come from the READER's TOC (the join),
	// the pool handles from the loaded cluster's URI registry.
	std::string perr;
	std::shared_ptr<smvk::PakBackend> backend =
		m_impl->runtime.OpenPak(record->pak, perr);
	if (!backend) {
		err = perr;
		return {};
	}
	// SMP8 THE COOKED MESH PLAN: the family -> member join comes from
	// the pak's Plan blob (the cook resolved it; the runtime never
	// string-scans the TOC). A missing plan/family = version skew =
	// loud failure.
	std::vector<smvk::PakMeshPlanMember> members;
	if (!AssetStore::FamilyMembers(*backend, record->path, members,
			err))
		return {};
	// ACQUIRE the family's carrier through the store: found (held),
	// adopted from a world-registered scene cluster, or loaded through
	// the cluster-source seam - with asset-side refs that keep it
	// alive across scene removals (the park bridge).
	tour::Cluster* cluster = m_impl->store.AcquireFamilyCluster(
		m_impl->world, record->pak, record->path, members,
		options.keepResident, err);
	if (!cluster)
		return {};
	// The family's BASE mesh (the plan's lodIndex 0 member) -> the
	// cluster pool row (the spawn-side seam resolves the WORLD index
	// fresh per query - see WorldMeshIndexOf; the slot keeps the
	// stable carrier row).
	tour::ClusterStaticMeshHandle baseMesh{};
	for (const smvk::PakMeshPlanMember& m : members) {
		if (m.lodIndex != 0)
			continue; // a LOD sibling, not the base
		if (auto hit = cluster->FindMeshByUri(m.tocPath)) {
			baseMesh = *hit;
			break;
		}
	}
	if (!baseMesh.valid()) {
		err = "the mesh family '" + record->path
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
	slot.pak = record->pak;
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
	// A node holds a loaded resource identity, not a duplicated asset.
	// Unloading while still attached would leave a dangling world row.
	if (m_impl->world.HasGameMeshBinding(handle.value))
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
	if (!s->keepResident) {
		m_impl->meshByKey.erase(s->key);
		m_impl->meshByPlan.erase(s->key); // the plan lane's parallel key
	}
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
	// Drop node associations before releasing their loaded resources.
	// The World retains nodes/transforms and its other value components.
	m_impl->world.ClearGameMeshBindings();
	// SNAPSHOT the handles before unloading: UnloadMesh ERASES the
	// slot's meshByKey/meshByPlan entries (every non-keepResident
	// slot), so unloading while range-iterating meshByKey is iterator
	// invalidation - the erased node's successor links are followed by
	// the next increment. libstdc++ segfaults there (the Linux CI
	// pak_host_tests SIGSEGV in std::_Rb_tree_increment <- here);
	// MSVC's erase leaves the walkable links intact, which is why
	// Windows passed. keepResident slots stay in the map through the
	// loop and are wiped by the clear() below - same contract as
	// before, minus the UB.
	std::vector<MeshHandle> handles;
	handles.reserve(m_impl->meshByKey.size());
	for (const auto& [key, handle] : m_impl->meshByKey)
		handles.push_back(handle);
	for (const MeshHandle handle : handles)
		(void)UnloadMesh(handle);
	m_impl->meshByKey.clear();
	m_impl->meshByPlan.clear();
	m_impl->batchMemory.clear();
	// The session (keepResident) refs go with the teardown: the last
	// one per carrier releases it for destruction (a parked carrier
	// dies here; a registered one stays with its scene owner).
	m_impl->store.ReleaseAll();
}

smvk::MeshEntityHandle AssetManager::SpawnMesh(MeshHandle mesh,
	const glm::mat4& frame, NodeId parent)
{
	const Impl::MeshSlot* s = m_impl
		? m_impl->MeshSlotOf(mesh)
		: nullptr;
	if (!s)
		return {};
	// The handle's CURRENT world row, resolved FRESH (the parked-
	// carrier contract: no row -> no spawn, never a stale row into a
	// rebuilt table).
	const uint32_t row = m_impl->WorldMeshIndexOf(*s);
	if (row == UINT32_MAX)
		return {};
	return m_impl->world.SpawnMesh(row, frame, parent);
}

MeshHandle AssetManager::LoadMeshByPlan(const smvk::MeshPlanRef& plan,
	std::string& err, const MeshLoadOptions& options)
{
	err.clear();
	if (!m_impl)
		return {};
	// Dedup by the PLAN IDENTITY (the compiled address), never the
	// catalog id (the two lanes are parallel keys into the same
	// slots).
	const uint64_t planKey =
		(uint64_t(plan.pakIndex) << 32) | plan.baseSlot;
	if (auto hit = m_impl->meshByPlan.find(planKey);
			hit != m_impl->meshByPlan.end()) {
		if (Impl::MeshSlot* s = m_impl->MeshSlotOf(hit->second)) {
			if (options.keepResident && !s->keepResident) {
				s->keepResident = true;
				if (s->cluster)
					m_impl->store.MakeResident(s->cluster);
			}
			return hit->second;
		}
		m_impl->meshByPlan.erase(hit);
	}
	// The plan acquire (per-asset fail-closed: the pak by index, the
	// slot validated against the deployed TOC).
	tour::Cluster* cluster =
		m_impl->store.AcquireFamilyClusterByPlan(m_impl->world, plan,
			options.keepResident, err);
	if (!cluster)
		return {};
	// The base mesh's URI: the slot entry's OWN path (the compiled
	// key — the store's validation proved it belongs to the plan's
	// family).
	std::string perr;
	std::shared_ptr<smvk::PakPager> pager =
		m_impl->runtime.OpenPakPager(
			m_impl->runtime.ManifestPakName(plan.pakIndex), perr);
	if (!pager) {
		err = perr;
		return {};
	}
	const smvk::PakTocEntry* base =
		pager->Reader().FindBySlot(plan.baseSlot);
	tour::ClusterStaticMeshHandle baseMesh{};
	if (base)
		if (auto hit = cluster->FindMeshByUri(base->path))
			baseMesh = *hit;
	if (!baseMesh.valid()) {
		err = "the compiled plan for family '"
			+ std::string(plan.familyPath)
			+ "' acquired its carrier but the base mesh row is"
			" missing (a cook anomaly - recook)";
		return {};
	}
	// The slot (the same mint as the catalog lane).
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
	slot.key = planKey;
	slot.pak = m_impl->runtime.ManifestPakName(plan.pakIndex);
	slot.cluster = cluster;
	slot.mesh = baseMesh;
	const MeshHandle handle{
		(uint64_t(generation) << 32) | index };
	m_impl->meshByPlan.emplace(planKey, handle);
	return handle;
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
	const tour::asset::ContentRecord* record =
		m_impl->runtime.Asset(key);
	if (!record) {
		err = "unknown content id " + HexId(key)
			+ " (a version skew between the compiled project.h and the"
			" packaged content?)";
		return {};
	}
	if (record->kind != smvk::kCatalogKindGi) {
		err = "content id " + HexId(key) + " is a '"
			+ smvk::CatalogKindToString(record->kind)
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
		err = "the GI manifest '" + record->path + "' lists no volumes";
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

namespace tour {

app::MeshHandle World::LoadMesh(uint64_t catalogKey, std::string& error)
{
	if (!gameAssets_) {
		error = "World has no attached AssetManager";
		return {};
	}
	return gameAssets_->LoadMesh(catalogKey, error);
}

bool World::NodeRef::SetMesh(app::MeshHandle mesh) const
{
	// Validate the generation and loaded resource before touching the
	// existing binding. A missing or parked mesh leaves it untouched.
	if (!owner_.IsGameNode(handle_) || !owner_.gameAssets_
			|| !owner_.gameAssets_->MeshValid(mesh))
		return false;
	const uint32_t worldMesh = owner_.gameAssets_->MeshWorldIndex(mesh);
	if (worldMesh == UINT32_MAX)
		return false;
	if (!tour::scene::SetMesh(owner_.scene, NodeHandleSlot(handle_), worldMesh))
		return false;
	owner_.gameMeshBindings_[handle_] = mesh.value;
	owner_.RefreshRuntimeSceneCaches();
	return true;
}

bool World::NodeRef::RemoveMesh() const
{
	if (!owner_.IsGameNode(handle_))
		return false;
	const bool removed = tour::scene::RemoveMesh(
		owner_.scene, NodeHandleSlot(handle_));
	owner_.gameMeshBindings_.erase(handle_);
	if (removed)
		owner_.RefreshRuntimeSceneCaches();
	return removed;
}

} // namespace tour
