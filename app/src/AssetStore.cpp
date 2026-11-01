// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

// THE ASSET STORE - the implementation (see AssetStore.h for the
// ownership contract + docs/content_as_code.md for the design).
//
// Composition, never duplication: the found-or-loaded logic is the
// AssetManager's old EnsureFamilyCluster walk (verbatim semantics —
// the held-cluster + world-cluster searches, the scene walk, the
// split-family refusal), now owning REFS (the park-on-unload bridge)
// instead of caching raw Cluster* pointers (the use-after-free the
// extraction fixes: a RemoveScene destroyed clusters the manager
// still pointed at).

#include "AssetStore.h"

#include "ProjectRuntime.h"

#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>

namespace app {

AssetStore::AssetStore(ProjectRuntime& runtime, smvk::ResourceManager* ctx)
	: m_runtime(runtime), m_ctx(ctx)
{
	// The DEFAULT cluster source: the engine's real deserialize (the
	// production lane — pinned by every cooked project launch). Null
	// ctx refuses loudly; the GPU-less tests override the seam.
	clusterSource =
		[this](tour::World& world, const smvk::PakReader& reader,
			uint32_t sceneIndex, std::string& err) -> smvk::Cluster* {
		if (!m_ctx) {
			err = "LoadMesh needs a GPU context (the mesh's owning"
				" scene cluster deserializes + uploads)";
			return nullptr;
		}
		try {
			return &world.LoadClusterFromPak(*m_ctx, reader, sceneIndex);
		}
		catch (const std::exception& e) {
			err = std::string("the owning cluster failed to load: ")
				+ e.what();
			return nullptr;
		}
	};
}

AssetStore::~AssetStore()
{
	// Defensive teardown: an owner that skipped ReleaseAll still
	// must not leak references (the park bridge would keep every
	// held cluster alive past its world). Order contract: the store
	// dies BEFORE the world (see the header).
	ReleaseAll();
}

// ---- the found-or-loaded acquire ------------------------------------------

smvk::Cluster* AssetStore::AcquireFamilyCluster(tour::World& world,
	const smvk::AppManifestAsset& row,
	const std::vector<smvk::PakMeshPlanMember>& members, bool resident,
	std::string& err)
{
	err.clear();
	if (members.empty()) {
		err = "the mesh family '" + row.path + "' has no cooked"
			" members (the pak's mesh plan is a version skew -"
			" recook)";
		return nullptr;
	}
	// (a) A cluster this store already holds (registered OR parked)
	// against THIS world: the shared-acquire dedup — repeated keys
	// never re-load, never re-carve GPU ranges.
	if (smvk::Cluster* held = FindHeldWithFamily(world, members)) {
		TakeRef(world, held, resident);
		return held;
	}
	// (b) ANY world-registered cluster (the apphost may have loaded
	// the owning scene through ProjectScenes): adopt it — the asset
	// rides the scene's own cluster, no duplicate carrier.
	for (const auto& c : world.clusters)
		if (c && clusterHasFamily(*c, members)) {
			TakeRef(world, c.get(), resident);
			return c.get();
		}
	// (c) Load through the seam: mesh rows carry no cooked-scene
	// token, so a deserialize is the only proof a scene carries the
	// family — walk the pak's scenes in order; on a miss, unload and
	// try the next (the v1 join boundary: a multi-scene pak whose
	// scenes SPLIT one .geometry family refuses loudly; the
	// subset-load follow-up addresses it).
	std::string perr;
	std::shared_ptr<smvk::PakBackend> backend =
		m_runtime.OpenPak(row.pak, perr);
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
	for (uint32_t si = 0; si < uint32_t(scenes.size()); ++si) {
		if (!clusterSource) {
			err = "the AssetStore has no cluster source installed";
			return nullptr;
		}
		smvk::Cluster* loaded = clusterSource(world,
			backend->Reader(), si, err);
		if (!loaded) {
			if (err.empty())
				err = "the cluster source failed (scene "
					+ std::to_string(si) + ")";
			return nullptr;
		}
		if (clusterHasFamily(*loaded, members)) {
			// The fresh carrier folds into the world exactly like the
			// old EnsureFamilyCluster did (World::LoadClusterFromPak's
			// own resolve parity — a source that registers a cluster
			// must see it resolved).
			world.ResolveAndFixup_clusters();
			TakeRef(world, loaded, resident);
			return loaded;
		}
		// Not this scene's family: drop the cluster again (a
		// load-time miss — UnloadCluster's own resolve folds nothing
		// user-visible back in).
		world.UnloadCluster(*loaded);
	}
	err = "no scene in pak '" + row.pak + "' carries the mesh family '"
		+ row.path + "' (a multi-scene split family - the v1 join"
		" boundary, see docs/content_as_code.md)";
	return nullptr;
}

void AssetStore::ReleaseFamilyCluster(smvk::Cluster* cluster)
{
	// Pointer IDENTITY only until the ref is confirmed — a cluster
	// this store does not hold is a no-op, never a dereference.
	const auto it = m_held.find(cluster);
	if (it == m_held.end())
		return;
	Held& h = it->second;
	if (h.refs == 0)
		return; // only the session ref remains (ReleaseAll owns it)
	if (h.world)
		h.world->ReleaseCluster(*cluster);
	if (--h.refs == 0 && !h.resident)
		m_held.erase(it); // nothing left holding it: the record goes
}

void AssetStore::MakeResident(smvk::Cluster* cluster)
{
	const auto it = m_held.find(cluster);
	if (it == m_held.end())
		return;
	Held& h = it->second;
	if (h.resident || !h.world)
		return;
	h.resident = true;
	h.world->RetainCluster(*cluster);
}

void AssetStore::ReleaseAll()
{
	for (auto& [cluster, h] : m_held) {
		if (!h.world)
			continue;
		// Every ref the store took: the acquire refs + the session
		// ref. The LAST one may destroy a parked cluster (the world
		// does it) — the ledger below is map storage, not cluster
		// state, so the loop is safe past that point.
		const uint32_t total = h.refs + (h.resident ? 1 : 0);
		for (uint32_t i = 0; i < total; ++i)
			h.world->ReleaseCluster(*cluster);
	}
	m_held.clear();
}

// ---- the join + the searches -----------------------------------------------

bool AssetStore::FamilyMembers(const smvk::PakBackend& backend,
	const std::string& familyPath,
	std::vector<smvk::PakMeshPlanMember>& out, std::string& err)
{
	// The COOKED MESH PLAN (SMP8): the cook resolved the family ->
	// member join; the runtime never string-scans the TOC. A missing
	// plan/family is a version skew - recook, never a silent empty.
	return backend.Reader().MeshPlanFamily(familyPath, out, err);
}

std::size_t AssetStore::HeldClusterCount() const
{
	return m_held.size();
}

bool AssetStore::HoldsCluster(smvk::Cluster* cluster) const
{
	return m_held.find(cluster) != m_held.end();
}

smvk::Cluster* AssetStore::FindHeldWithFamily(tour::World& world,
	const std::vector<smvk::PakMeshPlanMember>& members) const
{
	// A held cluster is refcounted alive (every record implies >= 1
	// world assetRef) — the dereference is safe by construction. A
	// record against ANOTHER world is skipped: a cluster lives in one
	// world at a time (the v1 bridge; the permanent store lifts this).
	for (const auto& [cluster, h] : m_held)
		if (h.world == &world && cluster
			&& clusterHasFamily(*cluster, members))
			return cluster;
	return nullptr;
}

bool AssetStore::clusterHasFamily(smvk::Cluster& cluster,
	const std::vector<smvk::PakMeshPlanMember>& members)
{
	for (const smvk::PakMeshPlanMember& m : members)
		if (cluster.FindMeshByUri(m.tocPath))
			return true;
	return false;
}

void AssetStore::TakeRef(tour::World& world, smvk::Cluster* cluster,
	bool resident)
{
	const auto [it, inserted] = m_held.try_emplace(cluster,
		Held{ &world, 0, false });
	Held& h = it->second;
	h.world = &world; // a re-acquire against a new world re-homes the record
	++h.refs;
	world.RetainCluster(*cluster);
	// keepResident upgrades only (never downgrades a shared carrier's
	// residency — the LoadMesh rule): the sticky session ref.
	if (resident && !h.resident) {
		h.resident = true;
		world.RetainCluster(*cluster);
	}
}

} // namespace app
