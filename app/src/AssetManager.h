// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#pragma once
// THE ASSET MANAGER FACADE (docs/content_as_code.md): the explicit
// load/blend surface over the compiled catalog's typed keys -
// gameplay code SELECTS assets at compile time (project.h's
// MatchAssets / the typed namespaces) and loads them EXPLICITLY here.
//
//   inline constexpr auto WorldMeshes =
//       project::MatchAssets<project::MeshKey, "^world_.*$">();
//   auto batch = assets.LoadMeshes(WorldMeshes.data(),
//       WorldMeshes.size());
//
// The manager composes the EXISTING runtime layers - never a parallel
// loading path: the AssetStore (the shared-resource OWNERSHIP layer -
// found-or-loaded carriers, asset-side refs, the park-on-unload
// bridge), ProjectRuntime (the [ids] join + the pak backends), the
// World's cluster loader (DeserializeCluster - the mesh/material/
// texture dependency resolution), and the WorldRenderer's GI
// machinery (BuildGiVolumeSetFromPaths + the blend publish).
//
// OWNERSHIP MODEL: resource handles (MeshHandle/GiHandle) name LOADED
// SHARED ASSETS; the World's entity handles name INSTANCES - the two
// never conflate. Mesh loading ACQUIRES the mesh's OWNING SCENE
// CLUSTER through the store (the v1 pak-format boundary: meshes ship
// inside their scene's cluster) - the store holds asset-side
// references, so a scene removal PARKS the carrier instead of
// destroying it out from under live handles (the handle stays valid;
// the mesh rows stay resident; the scene's nodes leave the world -
// removal never destroys an independently held asset, never
// resurrects scene content). Repeated keys deduplicate (the carrier
// is acquired once, never re-carved). GI sets load by their manifest
// rows and stay RESIDENT while referenced (SetGIBlend keeps BOTH sets
// live; only the scalar alpha moves per frame - the game owns ALL
// interpolation timing, never the engine).
//
// This header is the public surface; the implementation lives in
// AssetManager.cpp (the AudioRuntime facade discipline).

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/asset_plan.h>
#include <Engine/Asset/mesh_handle.h>
#include <Engine/Asset/lut_handle.h>
#include <Engine/Scene/entity_handle.h>
#include <Engine/Scene/scene_node_id.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {
class WorldRenderer; // the renderer facade (the GI publish seam)
}
namespace tour {
class World; // the engine-owned world (smvk::World is an alias)
class TextureStreamer; // (moved: engine-owned streamer, tour::)
}
namespace smvk {
class ResourceManager;
using ::tour::TextureStreamer;
class OverlayVfs;
}

namespace app {

class ProjectRuntime;
class AssetStore; // the shared-resource ownership layer (AssetStore.h)

// Mesh loading options (the v1 shape - see docs/content_as_code.md
// for the documented follow-ups).
struct MeshLoadOptions {
	// v1 always resolves the owning carrier (materials + textures
	// ride the cluster's dependency graph). The flags are carried for
	// the subset-load lane (the pak's per-mesh blobs ARE addressable;
	// the future geometry-only fast path).
	bool loadMaterials = true;
	bool loadTextures = true;
	// keepResident=false releases the carrier when the last handle
	// drops (a parked carrier is destroyed on the last ref - deferred
	// behind in-flight frames); true holds the store's SESSION ref
	// for the session (the prefetch idiom, released at UnloadAllMeshes
	// or teardown).
	bool keepResident = true;
};

// GiHandle: a loaded GI SET (a baked lighting set - one or more GI
// volumes). Generational; zero = invalid. Multiple sets stay resident
// simultaneously (the blend's two endpoints).
struct GiHandle {
	std::uint64_t value = 0;
	constexpr bool operator==(const GiHandle&) const = default;
	constexpr explicit operator bool() const { return value != 0; }
};

// A batch result: one handle PER INPUT KEY, in input order (a
// duplicate key in the input yields the SAME handle - deduplication
// is per key, never per slot). Raw pointer + count per the public-API
// convention; the manager owns the memory until UnloadMesh releases
// the batch's last reference.
struct MeshBatch {
	MeshHandle* handles = nullptr;
	std::size_t count = 0;

	MeshHandle operator[](std::size_t i) const
		{ return handles[i]; }
	// The pointer/count projection (nullptr valid only when count 0).
	const MeshHandle* data() const { return handles; }
};

class AssetManager {
public:
	// The facade's seams: the runtime (ids -> paks), the world (the
	// cluster loader), the context (GPU uploads), the renderer (the
	// GI publish). Nullptr ctx = the GPU-less test seam (the store's
	// DEFAULT cluster source refuses loudly - override
	// Store().clusterSource for the offline lane; the GI lane refuses
	// loudly either way).
	AssetManager(ProjectRuntime& runtime, ::tour::World& world,
		smvk::ResourceManager* ctx,
		::engine::WorldRenderer* renderer = nullptr);
	~AssetManager();

	AssetManager(const AssetManager&) = delete;
	AssetManager& operator=(const AssetManager&) = delete;

	// The shared-asset OWNERSHIP layer under this facade (see
	// AssetStore.h): the cluster-provider seam (the GPU-less test
	// lane / custom cluster sources) + the retention diagnostics.
	AssetStore& Store();

	// ---- MESHES -------------------------------------------------------
	// Loads one mesh BY KEY: resolves the [ids] row -> the owning pak
	// -> ACQUIRES the scene cluster that carries it through the store
	// (found, adopted from a loaded scene, or loaded through the
	// cluster-source seam: the mesh/material/texture graph resolves
	// exactly as scene loading) -> the mesh's cluster row. Repeated
	// calls with the same key return the SAME handle (the carrier is
	// acquired once; the store's refs keep it alive across scene
	// removals). Empty handle + err on a bad key/missing pak.
	MeshHandle LoadMesh(std::uint64_t key, std::string& err,
		const MeshLoadOptions& options = {});
	// The batch form (raw pointer + count; nullptr valid only when
	// count == 0 - the borrowed-input convention: the manager never
	// retains the input past the call). Duplicate keys dedup; the
	// per-pak grouping acquires each owning carrier once.
	MeshBatch LoadMeshes(const std::uint64_t* keys, std::size_t count,
		std::string& err, const MeshLoadOptions& options = {});
	// The handle's CURRENT world mesh row (the spawn-side seam),
	// resolved FRESH against the live world's meshLocations every
	// call (the tables rebuild per resolve - a cached index is stale
	// the moment any scene changes; the asset itself is not intrinsi-
	// cally world-indexed - a shared mesh can be carried by several
	// scene instances, and a PARKED carrier has no row at all).
	// UINT32_MAX when the carrier has no current world mapping.
	std::uint32_t MeshWorldIndex(MeshHandle handle) const;
	bool MeshValid(MeshHandle handle) const;
	// Releases ONE handle's reference; the carrier drops when the
	// last reference releases (a parked carrier is destroyed on the
	// last ref - deferred retirement; keepResident holds the session
	// ref until UnloadAllMeshes/teardown, and a carrier still owned
	// by a live scene stays with that scene).
	bool UnloadMesh(MeshHandle handle);
	// Releases EVERY mesh reference + the session (keepResident) refs
	// (the session teardown form).
	void UnloadAllMeshes();

	// ---- the spawn-side seam (the typed-key gameplay lane) ---------
	// Instances one LOADED mesh: LoadMesh(project::meshes::X) ->
	// SpawnMesh(handle, frame) - app/game code drives the project's
	// TYPED KEYS end to end, no authored scene node, no cluster
	// picking. Resolves the handle's CURRENT world row FRESH (a parked
	// carrier - its scene was removed while the asset stays live -
	// has NO row: the spawn refuses with an empty handle, never a
	// stale row). The entity is WORLD-owned: world.DestroyMesh(
	// entity) kills it (the manager owns the RESOURCE, never the
	// instance). `parent` = the same resolved-hierarchy contract as
	// World::SpawnMeshFromNode.
	smvk::MeshEntityHandle SpawnMesh(MeshHandle mesh,
		const glm::mat4& frame, NodeId parent);

	// ---- THE COMPILED-PLAN LANE (the static hot path) ----------------
	// Loads one mesh BY COMPILED PLAN (the private project_plans.h
	// constants — Engine/Asset/asset_plan.h): the pak by MOUNT INDEX,
	// the family by its STABLE SLOT — NO manifest row lookup, NO
	// mesh-plan parse, NO TOC string scan (the per-asset fail-closed
	// validation lives in the store's plan acquire; the boot-time
	// gate is ProjectRuntime::AttachCompiledPlans). Repeated plans
	// dedup by (pakIndex, baseSlot). The returned handle feeds the
	// same MeshWorldIndex/SpawnMesh/UnloadMesh surface as the
	// catalog lane.
	MeshHandle LoadMeshByPlan(const smvk::MeshPlanRef& plan,
		std::string& err,
		const MeshLoadOptions& options = {});

	// ---- ARTISTIC COLOR LUTS ------------------------------------------
	// A LutKey in project::luts is a typed catalog view over texture
	// assets in luts/. Loading caches decoded RGBA bytes; SetLut
	// uploads them through the renderer, never scans local directories.
	LutHandle LoadLut(std::uint64_t key, std::string& err);
	bool SetLut(LutHandle handle);
	bool UnloadLut(LutHandle handle);

	// ---- GI BAKE SETS --------------------------------------------------
	// Loads a GI set BY KEY: the [ids] row's .givolumemanifest bytes
	// through the pak's VFS surface (ReadAsset), its .givolume rows
	// parsed, the set built (the renderer's BuildGiVolumeSetFromPaths
	// - the same per-volume load/upload as scene GI). Multiple sets
	// stay resident; each LoadGI of the same key returns the SAME
	// handle.
	GiHandle LoadGI(std::uint64_t key, std::string& err);
	// Publishes a resident set as the ACTIVE GI (the renderer's
	// AdoptGiVolumeSet - the hot-swap publish; old resources retire
	// behind in-flight frames). False when the renderer seam is absent
	// or the handle is invalid.
	bool SetGI(GiHandle gi);
	// The BLEND: both sets evaluate INDEPENDENTLY each frame; the
	// shader interpolates the RESULTING lighting by alpha (0 = fully
	// `from`, 1 = fully `to`, 0.5 = equal; clamped). Both sets stay
	// resident; repeated calls with the same handles ONLY move the
	// scalar (no rebuild, no re-upload). The GAME owns the timing -
	// no engine-side durations/easing/timers by design.
	bool SetGIBlend(GiHandle from, GiHandle to, float alpha);
	// Releases a GI set's reference (the renderer-side resources
	// retire when no handle + no active publish references them).
	bool UnloadGI(GiHandle gi);

	// Diagnostics.
	std::size_t LoadedMeshCount() const;
	std::size_t LoadedGiCount() const;

private:
	struct Impl;
	Impl* m_impl = nullptr;
};

} // namespace app
