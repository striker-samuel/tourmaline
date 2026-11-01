// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#pragma once
// THE ASSET STORE (the shared-resource OWNERSHIP layer — the first
// landed slice of the consolidated content architecture, docs/
// content_as_code.md): device-scoped shared ownership of loaded asset
// resources, independent of any scene-instance system (ProjectScenes
// never knows who holds assets — scene loading stays clean of
// asset-holder coupling by design).
//
// V1 BRIDGE (explicitly temporary): mesh families ship INSIDE their
// owning scene cluster (the pak-format boundary), so the resource the
// store holds is the CLUSTER itself — acquired through the world the
// load runs against, refcounted (Cluster::assetRefs +
// World::RetainCluster), and PARKED rather than destroyed when a
// scene instance unloads while handles still reference it
// (World::UnloadCluster's shared-asset parking: the scene's nodes
// leave the merged fold — removal never resurrects scene content —
// while the resource rows stay alive). The pak-vNext pager + the
// shared mesh store (page-resident mesh rows uploaded through
// Renderer/geometry_upload.h) replace the cluster carrier: the
// permanent model, same public seam.
//
// THE SEMANTIC CONTRACT (the architecture's operation table):
//   LoadMesh()            acquire a shared asset; no new scene instance
//   LoadSceneAdditive()   a new scene-owned cluster (ProjectScenes)
//   RemoveScene()         removes only that scene instance
//   asset still ref'd    the underlying resource is preserved (parked)
//   last ref released     the resource becomes eligible for destruction
//   ResolveAndFixup()     rebuilds the world from its surviving clusters
//
// DEVICE SCOPE: one store per GPU device/context (the pak backends
// come from the ProjectRuntime it holds). The CLUSTERS it bridges are
// world-registered objects — a cluster lives in ONE world at a time;
// cross-world GPU sharing arrives with the permanent model. Teardown
// order: the store (and every facade holding it) must be released
// BEFORE the world it acquired against (ReleaseAll / destruction
// release the refs; the world destroys parked clusters on the last
// one). Main-thread only (cluster lifetime state).

#include <Engine/Asset/appmanifest.h>
#include <Engine/App/world.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace app {

class ProjectRuntime;

class AssetStore {
public:
	// The cluster-provider seam (mirrors ProjectScenes::ClusterLoader
	// — the same replaceable-pak-to-cluster discipline): given (world,
	// pak reader, scene index) produce the loaded cluster REGISTERED
	// into that world. The DEFAULT is the engine's real deserialize
	// (World::LoadClusterFromPak — needs a GPU context; a null ctx
	// refuses loudly, the production lane pinned by every cooked
	// project launch); the GPU-LESS tests substitute hand-built
	// clusters exactly like the ProjectScenes test seam.
	using ClusterSource = std::function<smvk::Cluster*(
		tour::World& world, const smvk::PakReader& reader,
		uint32_t sceneIndex, std::string& err)>;

	AssetStore(ProjectRuntime& runtime, smvk::ResourceManager* ctx);
	~AssetStore();

	AssetStore(const AssetStore&) = delete;
	AssetStore& operator=(const AssetStore&) = delete;

	// The seam (public member, ProjectScenes::clusterLoader style).
	ClusterSource clusterSource;

	// ---- MESH FAMILIES -------------------------------------------------
	// Acquires the mesh family's owning cluster — found (held by this
	// store, or any world-registered cluster: the apphost may have
	// loaded the owning scene) or loaded through the clusterSource
	// walk — holding ASSET-SIDE references: one per acquire (released
	// through ReleaseFamilyCluster) plus ONE STICKY SESSION REF when
	// `resident` (the keepResident idiom; upgrades only — released at
	// ReleaseAll/teardown). The FAMILY -> MEMBER join comes from the
	// pak's cooked mesh plan (SMP8, the Plan blob — see
	// PakReader::MeshPlanFamily): the cook resolved the ambiguity, the
	// runtime NEVER string-scans the TOC. Null + err on failure. The
	// returned cluster is REGISTERED in `world` (a found/loaded
	// carrier — the v1 boundary: the owning scene's nodes are part of
	// the world) or PARKED in it (a previously acquired carrier whose
	// scene went away; its rows are valid, its nodes are not in the
	// fold).
	smvk::Cluster* AcquireFamilyCluster(tour::World& world,
		const smvk::AppManifestAsset& row,
		const std::vector<smvk::PakMeshPlanMember>& members,
		bool resident, std::string& err);
	// Releases ONE acquire reference (the slot-death pairing; a no-op
	// for a cluster this store does not hold — pointer IDENTITY only,
	// never a dereference). When the last acquire ref of a PARKED
	// cluster drops (and no session ref exists) the world destroys it.
	void ReleaseFamilyCluster(smvk::Cluster* cluster);
	// Upgrades a held carrier to RESIDENT (the sticky session ref) —
	// the keepResident-UPGRADE path on a dedup hit (a default-options
	// load over a live transient handle): the slot flag and this
	// store's ledger must stay in lockstep or an unload would drop a
	// carrier the facade still promises to hold. No-op when already
	// resident or not held.
	void MakeResident(smvk::Cluster* cluster);
	// Session teardown: drops EVERY reference (acquire + session).
	// The last one per cluster releases it for destruction (a parked
	// cluster dies here; a registered one stays with its scene owner).
	void ReleaseAll();

	// The family -> member join (SMP8, the cooked mesh plan): the
	// pak's Plan blob resolves the family's members (base + lodN
	// siblings). False + err when the pak carries no plan / no such
	// family (a version skew between the catalog and the cooked
	// content - recook). NEVER a TOC string scan.
	static bool FamilyMembers(const smvk::PakBackend& backend,
		const std::string& familyPath,
		std::vector<smvk::PakMeshPlanMember>& out, std::string& err);

	// ---- diagnostics ----------------------------------------------------
	// Clusters this store currently holds references for.
	[[nodiscard]] std::size_t HeldClusterCount() const;
	[[nodiscard]] bool HoldsCluster(smvk::Cluster* cluster) const;

private:
	// One held carrier's ledger. `refs` counts OUTSTANDING ACQUIRE
	// references; `resident` marks the sticky session ref (the world's
	// assetRefs total = refs + (resident ? 1 : 0) — every store ref is
	// one World::RetainCluster). The record outlives refs==0 while
	// resident (the session ref's world pointer is needed at ReleaseAll).
	struct Held {
		tour::World* world = nullptr;
		uint32_t refs = 0;
		bool resident = false;
	};

	ProjectRuntime& m_runtime;
	smvk::ResourceManager* m_ctx;
	std::map<smvk::Cluster*, Held> m_held;

	smvk::Cluster* FindHeldWithFamily(tour::World& world,
		const std::vector<smvk::PakMeshPlanMember>& members) const;
	static bool clusterHasFamily(smvk::Cluster& cluster,
		const std::vector<smvk::PakMeshPlanMember>& members);
	void TakeRef(tour::World& world, smvk::Cluster* cluster,
		bool resident);
};

} // namespace app
