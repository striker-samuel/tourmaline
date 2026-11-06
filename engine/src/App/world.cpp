// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/App/world.h>

#include <cassert>
#include <iostream>
#include <stdexcept>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <immintrin.h>
#ifdef _WIN32
#include <intrin.h>
#else
#include <cpuid.h> // __get_cpuid / __get_cpuid_count (the GCC cpuid path)
#endif

#include <cstdio>
#include <Engine/Scene/scene.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Rendergraph/terrain_meta.h>
#include <Engine/Rendergraph/terrain_occlusion.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/geometry_streamer.h>
#include <Engine/Renderer/renderview.h>

namespace tour {
// (migration compat) the impl still spells the RHI/engine smvk
// types unqualified; qualify as call sites migrate, then drop this.
using namespace ::smvk;

	// Cluster ctor lives here so the App module owns the Cluster lifecycle
	// (its Scene + resource pools). The mesh upload/destroy methods and the
	// destructor stay in mesh.cpp; this constructor just wires the resource
	// context and the back-pointer to the owning World.
	Cluster::Cluster(ResourceManager* ctx)
		: ctx_(ctx)
	{}

	// ---------------------------------------------------------------------------
	// World
	// ---------------------------------------------------------------------------

	Cluster& World::LoadCluster(ResourceManager& ctx)
	{
		auto cluster = std::make_unique<Cluster>(&ctx);
		Cluster& ref = *cluster;
		RegisterCluster(std::move(cluster));
		return ref;
	}

	Cluster& World::LoadClusterFromPak(ResourceManager& ctx, const PakReader& reader,
		uint32_t sceneIndex, ::smvk::TextureStreamer* streamer,
		std::shared_ptr<const std::vector<uint8_t>> imagePin,
		const ExternalTextureResolver* externalTexture)
	{
		std::string err;
		auto cluster = DeserializeCluster(ctx, reader, &err, sceneIndex,
			streamer, std::move(imagePin), externalTexture,
			scene.nodeSlotBudget);
		if (!cluster)
			throw std::runtime_error("LoadClusterFromPak failed: " + err);
		Cluster& ref = *cluster;
		RegisterCluster(std::move(cluster));
		// Terrain rides the pak independent of scenes: harvest non-fatally
		// (a broken height tile must never fail the scene load).
		std::string terrErr;
		if (!LoadTerrainFromPak(reader, terrErr))
			std::cerr << "[World] terrain harvest skipped: " << terrErr << "\n";
		return ref;
	}

	bool World::LoadTerrainFromPak(const PakReader& reader, std::string& err) {
		terrainSource = TerrainSource{};
		auto blobs = reader.BlobsOfKind(PakAssetKind::Terrain);
		if (blobs.empty())
			return true; // no terrain: not an error (old paks, mesh paks)
		const PakTocEntry* metaBlob = nullptr;
		for (const PakTocEntry* e : blobs) {
			if (e->path == terrain::kTerrainMetaBlobPath) {
				metaBlob = e;
				break;
			}
		}
		if (!metaBlob) {
			err = "Terrain blobs present but terrain/meta missing";
			return false;
		}
		terrain::TerrainMeta meta;
		float tileMinMax[16][2];
		if (!terrain::ParseTerrainMetaBlob(reader.Payload(*metaBlob), meta,
			tileMinMax)) {
			err = "terrain/meta malformed";
			return false;
		}
		const uint32_t nH = 4;  // v1 fixed grid (cook-enforced)
		const uint32_t nS = 16; // worldSize/gBlock = 4096/256
        // Legacy paks have 273 rows; newer paks add 16 shadow-height
        // rows + one max hierarchy. Validate required paths below, and
        // require either all optional occlusion rows or none.
        const size_t legacy = 1 + size_t(nH) * nH + size_t(nS) * nS;
        const size_t occlusion = size_t(nH) * nH + 1;
        if (blobs.size() != legacy && blobs.size() != legacy + occlusion) {
            err = "Terrain blob count mismatch (expected legacy or occlusion layout)";
            return false;
        }
        const bool hasOcclusion = blobs.size() == legacy + occlusion;
        if (hasOcclusion) {
            for (uint32_t tz = 0; tz < nH; ++tz)
                for (uint32_t tx = 0; tx < nH; ++tx) {
                    const std::string path = "terrain/occlusion/"
                        + terrain::HTileName(tx, tz);
                    const auto* entry = reader.Find(PakAssetKind::Terrain, path);
                    if (!entry || reader.Payload(*entry).size() !=
                        size_t(meta.hRes) * meta.hRes * 4) {
                        err = "missing or malformed " + path;
                        return false;
                    }
                }
            const auto* entry = reader.Find(PakAssetKind::Terrain,
                terrain::kOcclusionPyramidPath);
            if (!entry || reader.Payload(*entry).size() !=
                size_t(terrain::kOcclusionAtlasWidth)
                    * terrain::kOcclusionAtlasHeight * 2) {
                err = "missing or malformed terrain/occlusion/max_pyramid";
                return false;
            }
            const auto data = reader.Payload(*entry);
            terrainSource.occlusionPyramid.assign(data.begin(), data.end());
        }
		terrain::TerrainHField hf;
		hf.meta = meta;
		hf.tiles.resize(size_t(nH) * size_t(nH));
		for (uint32_t tz = 0; tz < nH; ++tz) {
			for (uint32_t tx = 0; tx < nH; ++tx) {
				const PakTocEntry* found = reader.Find(PakAssetKind::Terrain,
					terrain::HTileBlobPath(tx, tz));
				if (!found) {
					err = "missing " + terrain::HTileBlobPath(tx, tz);
					return false;
				}
				const auto bytes = reader.Payload(*found);
				if (bytes.size() != size_t(meta.hRes) * size_t(meta.hRes) * 4) {
					err = terrain::HTileBlobPath(tx, tz) + " bad size";
					return false;
				}
				auto& dst = hf.tiles[size_t(tz) * size_t(nH) + size_t(tx)];
				dst.assign(bytes.begin(), bytes.end());
			}
		}
		if (!hf.valid()) {
			err = "harvested H-field invalid";
			return false;
		}
		std::vector<std::vector<uint8_t>> masks(size_t(nS) * size_t(nS));
		for (uint32_t tz = 0; tz < nS; ++tz) {
			for (uint32_t tx = 0; tx < nS; ++tx) {
				const PakTocEntry* found = reader.Find(PakAssetKind::Terrain,
					terrain::STileBlobPath(tx, tz));
				if (!found) {
					err = "missing " + terrain::STileBlobPath(tx, tz);
					return false;
				}
				const auto bytes = reader.Payload(*found);
				if (bytes.size()
					!= size_t(terrain::kMaskTileRes) * size_t(terrain::kMaskTileRes) * 4) {
					err = terrain::STileBlobPath(tx, tz) + " bad size";
					return false;
				}
				masks[size_t(tz) * size_t(nS) + size_t(tx)].assign(
					bytes.begin(), bytes.end());
			}
		}
		terrainSource.hfield = std::move(hf);
		terrainSource.masks = std::move(masks);
		terrainSource.hasTerrain = true;
		std::cerr << "[World] terrain harvested: 16 H + 256 S tiles\n";
		return true;
	}

	Cluster& World::EnsureDynamicCluster(ResourceManager* ctx) {
		for (auto& c : clusters)
			if (c && c->isDynamic)
				return *c;
		auto cluster = std::make_unique<Cluster>(ctx);
		cluster->isDynamic = true;
		Cluster& ref = *cluster;
		RegisterCluster(std::move(cluster));
		return ref;
	}

	NodeId World::CreateDecalAt(const glm::mat4& frame, SceneDecal def)
	{
		Cluster& dyn = EnsureDynamicCluster(nullptr);

		// Durable home: a root node in the DYNAMIC CLUSTER's own scene.
		// ResolveAndFixup folds cluster scenes into the merged world, so
		// this node (and its decal link) re-enters every rebuild — the
		// runtime entity survives pak-cluster unloads exactly like cooked
		// content.
		const int cnode = addNode(dyn.scene, -1, 0);
		if (cnode < 0)
			return UINT32_MAX;
		(void)tour::scene::SetLocalTransform(dyn.scene, MakeNodeHandle(dyn.scene, uint32_t(cnode)), frame);
		dyn.scene.globalTransform[size_t(cnode)] = frame;
		if (!def.name.empty()) {
			const uint32_t cnameId = uint32_t(dyn.scene.nodeNames.size());
			dyn.scene.nodeNames.push_back(def.name);
			SetNodeNameIndex(dyn.scene, uint32_t(cnode), cnameId);
		}
		const ClusterDecalHandle handle = dyn.CreateDecal(std::move(def));
		if (!handle.valid())
			return UINT32_MAX;
		// Cluster-local pool index — the resolve remap rewrites it to a
		// world decalLocations index (same as cooked links).
		SetDecal(dyn.scene, uint32_t(cnode), handle.index());

		// Merged-scene mirror for IMMEDIATE visibility (no resolve ran):
		// same frame + name, linked to the same pool entry through a
		// pushed decalLocations row. The mirror (and this row) die at the
		// next resolve — the cluster-scene original above folds back in.
		const int mnode = addNode(scene, -1, 0);
		if (mnode < 0)
			return UINT32_MAX;
		(void)tour::scene::SetLocalTransform(scene, MakeNodeHandle(scene, uint32_t(mnode)), frame);
		scene.globalTransform[size_t(mnode)] = frame;
		{
			const uint32_t cnameIdx =
				GetNodeNameIndex(dyn.scene, uint32_t(cnode));
			if (cnameIdx != 0xFFFFFFFFu) {
				const uint32_t nameId = uint32_t(scene.nodeNames.size());
				scene.nodeNames.push_back(dyn.scene.nodeNames[cnameIdx]);
				SetNodeNameIndex(scene, uint32_t(mnode), nameId);
			}
		}
		const uint32_t worldDecalIdx = uint32_t(decalLocations.size());
		decalLocations.push_back({ &dyn, handle });
		SetDecal(scene, uint32_t(mnode), worldDecalIdx);
		markAsChanged(scene, mnode);
		return uint32_t(mnode);
	}

	// ---- runtime audio-source entities (the gameplay-handle surface)

	AudioSourceHandle World::SpawnAudioSource(const glm::mat4& localFrame,
		NodeId parent, const SceneAudioSource& def)
	{
		Cluster& dyn = EnsureDynamicCluster(nullptr);
		// Durable home: the dynamic cluster's own scene (survives pak
		// unloads + resolves — the CreateDecalAt pattern). `parent`
		// (a MERGED node id) resolves to the durable side only when it
		// lives in the dynamic cluster itself; a foreign parent is a
		// same-frame convenience for the mirror below (the durable
		// node roots at the cluster root — reparenting across cluster
		// scenes is not a thing; the resolved hierarchy owns it).
		const int cnode = addNode(dyn.scene, -1, 0);
		if (cnode < 0)
			return {};
		(void)tour::scene::SetLocalTransform(dyn.scene, MakeNodeHandle(dyn.scene, uint32_t(cnode)), localFrame);
		dyn.scene.globalTransform[size_t(cnode)] = localFrame;
		SetAudioSource(dyn.scene, NodeId(cnode), def);
		// Merged-scene mirror for immediate visibility (dies at the
		// next resolve; the durable row above folds back in).
		const int mnode = addNode(scene, -1, 0);
		NodeId merged = UINT32_MAX;
		if (mnode >= 0) {
			(void)tour::scene::SetLocalTransform(scene, MakeNodeHandle(scene, uint32_t(mnode)), localFrame);
			scene.globalTransform[size_t(mnode)] = localFrame;
			SetAudioSource(scene, NodeId(mnode), def);
			markAsChanged(scene, mnode);
			merged = NodeId(mnode);
		}
		// The registry slot (generation 1 on a fresh slot; a reused
		// slot mints its bumped generation — the stale-handle
		// guarantee, see entity_handle.h).
		uint32_t index;
		uint32_t generation;
		if (!audioEntityFree.empty()) {
			index = audioEntityFree.back();
			audioEntityFree.pop_back();
			generation = audioEntities[index].generation;
		}
		else {
			index = uint32_t(audioEntities.size());
			audioEntities.emplace_back();
			generation = audioEntities.back().generation;
		}
		AudioSourceEntity& e = audioEntities[index];
		e.alive = true;
		e.owner = &dyn;
		e.clusterNode = NodeId(cnode);
		e.mergedNode = merged;
		return MakeEntityHandle<AudioSourceTag>(index, generation);
	}

	bool World::DestroyAudioSource(AudioSourceHandle handle)
	{
		if (!handle)
			return false;
		const uint32_t idx = handle.Index();
		if (idx >= audioEntities.size())
			return false;
		AudioSourceEntity& e = audioEntities[idx];
		if (!e.alive || e.generation != handle.Generation())
			return false;
		// Drop the rows (durable + the current mirror) + retire the
		// nodes. The audio manager's voices stop on its NEXT update
		// (IsAudioSourceAlive fails); nothing here reaches the mixer.
		if (e.owner) {
			RemoveAudioSource(e.owner->scene, e.clusterNode);
			RetireNodeSlot(e.owner->scene, e.clusterNode);
		}
		if (e.mergedNode != UINT32_MAX
				&& e.mergedNode < scene.hierarchy.size()) {
			RemoveAudioSource(scene, e.mergedNode);
			RetireNodeSlot(scene, e.mergedNode);
		}
		// The generation bump: every stale copy of this handle fails
		// validation forever; a wrapped generation retires the slot.
		e.alive = false;
		e.owner = nullptr;
		if (e.generation != UINT32_MAX) {
			++e.generation;
			if (e.generation != 0)
				audioEntityFree.push_back(idx);
			// (a wrap to 0 keeps the slot retired — never reused)
		}
		return true;
	}

	bool World::IsAudioSourceAlive(AudioSourceHandle handle) const
	{
		if (!handle)
			return false;
		const uint32_t idx = handle.Index();
		if (idx >= audioEntities.size())
			return false;
		const AudioSourceEntity& e = audioEntities[idx];
		return e.alive && e.generation == handle.Generation();
	}

	NodeId World::AudioSourceNode(AudioSourceHandle handle) const
	{
		if (!handle)
			return UINT32_MAX;
		const uint32_t idx = handle.Index();
		if (idx >= audioEntities.size())
			return UINT32_MAX;
		const AudioSourceEntity& e = audioEntities[idx];
		if (!e.alive || e.generation != handle.Generation())
			return UINT32_MAX;
		return e.mergedNode;
	}

	// ---- runtime mesh entities + the animator component ---------------

	namespace {
		// The world-level index a (cluster, pool-handle) pair resolves
		// through the CURRENT location tables (rebuilt per resolve —
		// look it up fresh every use, never cache it).
		uint32_t WorldIndexOfMesh(
			const std::vector<World::MeshLocation>& locations,
			const Cluster* cluster, ClusterStaticMeshHandle handle)
		{
			for (uint32_t i = 0; i < locations.size(); ++i)
				if (locations[i].cluster == cluster
						&& locations[i].handle == handle)
					return i;
			return UINT32_MAX;
		}
		uint32_t WorldIndexOfMaterial(
			const std::vector<World::MaterialLocation>& locations,
			const Cluster* cluster, ClusterMaterialHandle handle)
		{
			for (uint32_t i = 0; i < locations.size(); ++i)
				if (locations[i].cluster == cluster
						&& locations[i].handle == handle)
					return i;
			return UINT32_MAX;
		}
	} // namespace

	MeshEntityHandle World::SpawnMeshFromNode(NodeId sourceNode,
		const glm::mat4& frame, NodeId parent)
	{
		// The source: a merged-scene node carrying the mesh (+optional
		// material) associations to instance.
		(void)parent;
		if (sourceNode >= scene.hierarchy.size())
			return {};
		const MeshRow* srcMesh = FindMesh(scene, sourceNode);
		if (!srcMesh || srcMesh->mesh >= meshLocations.size())
			return {};
		const MaterialRow* srcMat = FindMaterial(scene, sourceNode);
		const MaterialLocation* matLoc = nullptr;
		if (srcMat && srcMat->material < materialLocations.size())
			// (the material may belong to another cluster — the ENTITY
			// binding carries the pair, the mirror resolves it through
			// the world table the same way at mirror time.)
			matLoc = &materialLocations[srcMat->material];
		return SpawnMeshEntityInternal(meshLocations[srcMesh->mesh],
			matLoc, frame);
	}

	MeshEntityHandle World::SpawnMesh(uint32_t worldMeshIndex,
		const glm::mat4& frame, NodeId parent,
		uint32_t worldMaterialIndex)
	{
		// The KEY-DRIVEN entry (see the header block): the row is the
		// whole source — a loaded asset handle resolved to its CURRENT
		// world mesh index (the app-side MeshWorldIndex discipline:
		// fresh, never cached — a parked carrier has no row at all).
		(void)parent;
		if (worldMeshIndex >= meshLocations.size())
			return {};
		const MaterialLocation* matLoc = nullptr;
		if (worldMaterialIndex != kInvalidIndex
				&& worldMaterialIndex < materialLocations.size())
			matLoc = &materialLocations[worldMaterialIndex];
		return SpawnMeshEntityInternal(meshLocations[worldMeshIndex],
			matLoc, frame);
	}

	MeshEntityHandle World::SpawnMeshEntityInternal(
		const MeshLocation& meshLoc, const MaterialLocation* matLoc,
		const glm::mat4& frame)
	{
		Cluster* meshOwner = meshLoc.cluster;
		const ClusterStaticMeshHandle meshHandle = meshLoc.handle;
		if (!meshOwner)
			return {};
		const ClusterMaterialHandle matHandle =
			matLoc ? matLoc->handle : ClusterMaterialHandle{};
		// The durable transform node (the dynamic cluster's own scene —
		// the CreateDecalAt pattern; the MESH binding is cross-cluster
		// so it lives on the ENTITY, not the dyn-scene tables).
		Cluster& dyn = EnsureDynamicCluster(nullptr);
		const int cnode = addNode(dyn.scene, -1, 0);
		if (cnode < 0)
			return {};
		(void)tour::scene::SetLocalTransform(dyn.scene, MakeNodeHandle(dyn.scene, uint32_t(cnode)), frame);
		dyn.scene.globalTransform[size_t(cnode)] = frame;
		// (the durable node roots at the cluster root; the resolved
		// hierarchy owns the merge — same as the audio entities)
		// The merged mirror for immediate visibility (mesh + material
		// rows through the CURRENT world location tables).
		const uint32_t worldMeshIdx =
			WorldIndexOfMesh(meshLocations, meshOwner, meshHandle);
		NodeId merged = UINT32_MAX;
		const int mnode = addNode(scene, -1, 0);
		if (mnode >= 0 && worldMeshIdx != UINT32_MAX) {
			(void)tour::scene::SetLocalTransform(scene, MakeNodeHandle(scene, uint32_t(mnode)), frame);
			scene.globalTransform[size_t(mnode)] = frame;
			SetMesh(scene, NodeId(mnode), worldMeshIdx);
			if (matLoc) {
				const uint32_t worldMatIdx = WorldIndexOfMaterial(
					materialLocations, matLoc->cluster, matLoc->handle);
				if (worldMatIdx != UINT32_MAX)
					SetMaterial(scene, NodeId(mnode), worldMatIdx);
			}
			markAsChanged(scene, mnode);
			merged = NodeId(mnode);
		}
		// The registry slot.
		uint32_t index;
		uint32_t generation;
		if (!meshEntityFree.empty()) {
			index = meshEntityFree.back();
			meshEntityFree.pop_back();
			generation = meshEntities[index].generation;
		}
		else {
			index = uint32_t(meshEntities.size());
			meshEntities.emplace_back();
			generation = meshEntities.back().generation;
		}
		MeshEntity& e = meshEntities[index];
		e.alive = true;
		e.transformOwner = &dyn;
		e.clusterNode = NodeId(cnode);
		e.mergedNode = merged;
		e.meshOwner = meshOwner;
		e.mesh = meshHandle;
		e.material = matHandle;
		e.animatorSlot = UINT32_MAX;
		return MakeEntityHandle<MeshEntityTag>(index, generation);
	}

	bool World::DestroyMesh(MeshEntityHandle handle)
	{
		if (!handle)
			return false;
		const uint32_t idx = handle.Index();
		if (idx >= meshEntities.size())
			return false;
		MeshEntity& e = meshEntities[idx];
		if (!e.alive || e.generation != handle.Generation())
			return false;
		// The ATTACHED animator dies with the entity (explicitly-owned
		// attachment — the Kill() contract; independently-held animator
		// handles fail validation after this).
		if (e.animatorSlot != UINT32_MAX
				&& e.animatorSlot < animators.size()) {
			AnimatorEntity& a = animators[e.animatorSlot];
			if (a.alive) {
				if (a.state.target && a.state.target->poseExternallyDriven)
					a.state.target->poseExternallyDriven = false;
				a.alive = false;
				a.state = AnimatorState{};
				if (a.generation != UINT32_MAX) {
					++a.generation;
					if (a.generation != 0)
						animatorFree.push_back(e.animatorSlot);
				}
			}
		}
		// Drop the mirror rows + retire the nodes.
		if (e.mergedNode != UINT32_MAX
				&& e.mergedNode < scene.hierarchy.size()) {
			RemoveMesh(scene, e.mergedNode);
			RemoveMaterial(scene, e.mergedNode);
			RetireNodeSlot(scene, e.mergedNode);
		}
		if (e.transformOwner) {
			RemoveMesh(e.transformOwner->scene, e.clusterNode);
			RetireNodeSlot(e.transformOwner->scene, e.clusterNode);
		}
		e.alive = false;
		if (e.generation != UINT32_MAX) {
			++e.generation;
			if (e.generation != 0)
				meshEntityFree.push_back(idx);
		}
		return true;
	}

	bool World::IsMeshEntityAlive(MeshEntityHandle handle) const
	{
		if (!handle)
			return false;
		const uint32_t idx = handle.Index();
		if (idx >= meshEntities.size())
			return false;
		const MeshEntity& e = meshEntities[idx];
		return e.alive && e.generation == handle.Generation();
	}

	NodeId World::MeshEntityNode(MeshEntityHandle handle) const
	{
		if (!handle)
			return UINT32_MAX;
		const uint32_t idx = handle.Index();
		if (idx >= meshEntities.size())
			return UINT32_MAX;
		const MeshEntity& e = meshEntities[idx];
		if (!e.alive || e.generation != handle.Generation())
			return UINT32_MAX;
		return e.mergedNode;
	}

	uint32_t World::MeshEntityMeshIndex(MeshEntityHandle handle) const
	{
		if (!IsMeshEntityAlive(handle))
			return UINT32_MAX;
		const MeshEntity& e =
			meshEntities[handle.Index()];
		return WorldIndexOfMesh(meshLocations, e.meshOwner, e.mesh);
	}

	// ---- material runtime state + material instances (the userdata
	// lane, docs/shader_library_design.md) ----

	bool World::SetMaterialUserData(uint32_t materialIndex, uint32_t userData)
	{
		if (materialIndex >= materialLocations.size())
			return false;
		if (materialUserData.size() < materialLocations.size())
			materialUserData.resize(materialLocations.size(), 0u);
		materialUserData[materialIndex] = userData;
		return true;
	}

	uint32_t World::MaterialUserData(uint32_t materialIndex) const
	{
		return materialIndex < materialUserData.size()
			? materialUserData[materialIndex] : 0u;
	}

	MaterialInstanceHandle World::CreateMaterialInstance(
		uint32_t baseMaterialIndex)
	{
		if (baseMaterialIndex >= materialLocations.size())
			return {};
		const MaterialLocation& loc = materialLocations[baseMaterialIndex];
		if (!loc.cluster || !loc.handle.valid())
			return {};
		uint32_t index;
		uint32_t generation;
		if (!materialInstanceFree.empty()) {
			index = materialInstanceFree.back();
			materialInstanceFree.pop_back();
			generation = materialInstances[index].generation;
		}
		else {
			index = uint32_t(materialInstances.size());
			materialInstances.emplace_back();
			generation = materialInstances.back().generation;
		}
		MaterialInstanceEntity& inst = materialInstances[index];
		inst.alive = true;
		inst.baseOwner = loc.cluster;
		inst.base = loc.handle;
		inst.userData = 0;
		inst.overridesUserData = false;
		return MakeEntityHandle<MaterialInstanceTag>(index, generation);
	}

	bool World::DestroyMaterialInstance(MaterialInstanceHandle handle)
	{
		if (!handle)
			return false;
		const uint32_t idx = handle.Index();
		if (idx >= materialInstances.size())
			return false;
		MaterialInstanceEntity& inst = materialInstances[idx];
		if (!inst.alive || inst.generation != handle.Generation())
			return false;
		// The SHARED base material is never touched. Bound entities'
		// stale handles (and the drawItems linkage) fail validation and
		// fall back to the base default — no dangling state.
		inst.alive = false;
		inst.baseOwner = nullptr;
		inst.base = {};
		inst.userData = 0;
		inst.overridesUserData = false;
		if (inst.generation != UINT32_MAX) {
			++inst.generation;
			if (inst.generation != 0)
				materialInstanceFree.push_back(idx);
		}
		return true;
	}

	bool World::IsMaterialInstanceAlive(MaterialInstanceHandle handle) const
	{
		if (!handle)
			return false;
		const uint32_t idx = handle.Index();
		return idx < materialInstances.size()
			&& materialInstances[idx].alive
			&& materialInstances[idx].generation == handle.Generation();
	}

	bool World::SetMeshMaterialInstance(MeshEntityHandle mesh,
		MaterialInstanceHandle instance)
	{
		if (!IsMeshEntityAlive(mesh))
			return false;
		// An EMPTY instance unbinds (back to the base default); a
		// non-empty one must be live.
		if (instance && !IsMaterialInstanceAlive(instance))
			return false;
		MeshEntity& e = meshEntities[mesh.Index()];
		e.materialInstance = instance;
		// The drawItems LINKAGE re-resolves at the next resolve (the
		// entity's mirror carries it); the world-dirty marking matches
		// every other mirror edit.
		dirtyCull = true;
		return true;
	}

	bool World::SetMaterialInstanceUserData(MaterialInstanceHandle instance,
		uint32_t userData)
	{
		if (!IsMaterialInstanceAlive(instance))
			return false;
		MaterialInstanceEntity& inst = materialInstances[instance.Index()];
		inst.userData = userData;
		inst.overridesUserData = true;
		return true;
	}

	bool World::ClearMaterialInstanceUserDataOverride(
		MaterialInstanceHandle instance)
	{
		if (!IsMaterialInstanceAlive(instance))
			return false;
		MaterialInstanceEntity& inst = materialInstances[instance.Index()];
		inst.overridesUserData = false;
		inst.userData = 0;
		return true;
	}

	uint32_t World::ResolvedItemUserData(const RenderItem& item) const
	{
		// The item's linkage is the raw HANDLE VALUE: a destroyed
		// instance — or a younger one that reused the slot — fails the
		// generation check and cleanly falls back to the base default.
		if (item.materialInstance != 0) {
			const uint32_t idx =
				uint32_t(item.materialInstance & 0xFFFFFFFFu);
			const uint32_t gen = uint32_t(item.materialInstance >> 32);
			if (idx < materialInstances.size()) {
				const MaterialInstanceEntity& inst = materialInstances[idx];
				if (inst.alive && inst.generation == gen
						&& inst.overridesUserData)
					return inst.userData;
			}
		}
		return MaterialUserData(item.material);
	}

	bool World::SetMaterialShaderFamily(uint32_t materialIndex,
		smvk::MaterialShaderFamily family)
	{
		if (materialIndex >= materialLocations.size())
			return false;
		if (materialShaderFamily.size() < materialLocations.size())
			materialShaderFamily.assign(materialLocations.size(),
				uint8_t(smvk::MaterialShaderFamily::StandardLit));
		materialShaderFamily[materialIndex] = uint8_t(family);
		return true;
	}

	smvk::MaterialShaderFamily World::ShaderFamilyOf(
		uint32_t materialIndex) const
	{
		const uint8_t raw = materialIndex < materialShaderFamily.size()
			? materialShaderFamily[materialIndex]
			: uint8_t(smvk::MaterialShaderFamily::StandardLit);
		return raw == uint8_t(smvk::MaterialShaderFamily::ProjectLit)
			? smvk::MaterialShaderFamily::ProjectLit
			: smvk::MaterialShaderFamily::StandardLit;
	}

	Animator World::AddAnimator(MeshEntityHandle mesh,
		touranim::Clip initialClip)
	{
		if (!IsMeshEntityAlive(mesh))
			return {};
		MeshEntity& e = meshEntities[mesh.Index()];
		if (!e.meshOwner || !e.meshOwner->hasSkeleton) {
			std::cerr << "[World] AddAnimator: the entity's mesh cluster"
				" carries no skeleton (a static-mesh entity — the v1"
				" single-skeleton-per-cluster contract)\n";
			return {};
		}
		if (e.animatorSlot != UINT32_MAX) {
			// One animator per entity (the attachment slot): replace.
			if (e.animatorSlot < animators.size()
					&& animators[e.animatorSlot].alive) {
				AnimatorEntity& old = animators[e.animatorSlot];
				if (old.state.target
						&& old.state.target->poseExternallyDriven)
					old.state.target->poseExternallyDriven = false;
				old.alive = false;
				old.state = AnimatorState{};
				if (old.generation != UINT32_MAX) {
					++old.generation;
					if (old.generation != 0)
						animatorFree.push_back(e.animatorSlot);
				}
			}
			e.animatorSlot = UINT32_MAX;
		}
		// The rest pose: the target cluster's CURRENT joint locals (the
		// bind pose on a fresh load — attach before other writers move
		// the joints if you need exact bind; untracked joints/channels
		// keep this pose forever).
		Cluster* target = e.meshOwner;
		AnimatorState state;
		state.mesh = mesh;
		state.target = target;
		state.restLocals.resize(target->skeleton.Count());
		for (uint32_t j = 0; j < target->skeleton.Count(); ++j) {
			const uint32_t node = target->skeleton.joints[j];
			state.restLocals[j] = node <
					target->scene.localTransform.size()
				? target->scene.localTransform[node]
				: glm::mat4(1.0f);
		}
		AnimatorPlay(state, std::move(initialClip), true);
		// The registry slot.
		uint32_t index;
		uint32_t generation;
		if (!animatorFree.empty()) {
			index = animatorFree.back();
			animatorFree.pop_back();
			generation = animators[index].generation;
		}
		else {
			index = uint32_t(animators.size());
			animators.emplace_back();
			generation = animators.back().generation;
		}
		AnimatorEntity& a = animators[index];
		a.alive = true;
		a.meshEntitySlot = mesh.Index();
		a.state = std::move(state);
		e.animatorSlot = index;
		return Animator(*this,
			MakeEntityHandle<AnimatorTag>(index, generation));
	}

	bool World::DestroyAnimator(AnimatorHandle handle)
	{
		if (!handle)
			return false;
		const uint32_t idx = handle.Index();
		if (idx >= animators.size())
			return false;
		AnimatorEntity& a = animators[idx];
		if (!a.alive || a.generation != handle.Generation())
			return false;
		if (a.state.target && a.state.target->poseExternallyDriven)
			a.state.target->poseExternallyDriven = false;
		// Detach from the entity (a later DestroyMesh must not touch
		// the freed slot).
		if (a.meshEntitySlot != UINT32_MAX
				&& a.meshEntitySlot < meshEntities.size()
				&& meshEntities[a.meshEntitySlot].animatorSlot == idx)
			meshEntities[a.meshEntitySlot].animatorSlot = UINT32_MAX;
		a.alive = false;
		a.state = AnimatorState{};
		if (a.generation != UINT32_MAX) {
			++a.generation;
			if (a.generation != 0)
				animatorFree.push_back(idx);
		}
		return true;
	}

	bool World::IsAnimatorAlive(AnimatorHandle handle) const
	{
		if (!handle)
			return false;
		const uint32_t idx = handle.Index();
		if (idx >= animators.size())
			return false;
		const AnimatorEntity& a = animators[idx];
		return a.alive && a.generation == handle.Generation();
	}

	AnimatorState* World::AnimatorStateOf(AnimatorHandle handle)
	{
		if (!handle)
			return nullptr;
		const uint32_t idx = handle.Index();
		if (idx >= animators.size())
			return nullptr;
		AnimatorEntity& a = animators[idx];
		if (!a.alive || a.generation != handle.Generation())
			return nullptr;
		return &a.state;
	}

	const AnimatorState* World::AnimatorStateOf(
		AnimatorHandle handle) const
	{
		if (!handle)
			return nullptr;
		const uint32_t idx = handle.Index();
		if (idx >= animators.size())
			return nullptr;
		const AnimatorEntity& a = animators[idx];
		if (!a.alive || a.generation != handle.Generation())
			return nullptr;
		return &a.state;
	}

	void World::Update(float dt)
	{
		if (animators.empty())
			return;
		// Reset the claims for THIS frame (the first animator per
		// cluster re-claims below; the renderer reads the flags after
		// Update — a sticky claim would freeze the driving animator's
		// own re-flush, and a dead animator's claim is re-taken by the
		// next animator on its next Update).
		for (const auto& clusterPtr : clusters)
			if (clusterPtr)
				clusterPtr->poseExternallyDriven = false;
		// Advance every animator (independent playback state — exact
		// for all entities) + flush the FIRST animator per target
		// cluster (the poseExternallyDriven flag IS the claim for this
		// frame: the first to reach a cluster writes its pose, later
		// animators on the SAME cluster keep their independent state
		// but do not fight the pose).
		for (AnimatorEntity& a : animators) {
			if (!a.alive || !a.state.target)
				continue;
			AdvanceAnimator(a.state, dt);
			Cluster& c = *a.state.target;
			if (a.state.jointLocals.empty() || c.poseExternallyDriven)
				continue;
			// The flush: the evaluated joint locals become the
			// cluster's pose (the existing global walk + palette build
			// consume it; the renderer skips its own clip advance on
			// driven clusters).
			const touranim::Skeleton& sk = c.skeleton;
			for (uint32_t j = 0; j < sk.Count(); ++j) {
				if (sk.joints[j] >= c.scene.localTransform.size())
					continue;
				c.scene.localTransform[sk.joints[j]] =
					a.state.jointLocals[j];
				markAsChanged(c.scene, int(sk.joints[j]));
			}
			recalculateGlobalTransforms(c.scene);
			c.poseExternallyDriven = true;
		}
	}

	std::span<const uint8_t> World::ReadBakeBytes(
		const std::string& vfsPath) const
	{
		// Dynamic cluster FIRST: runtime injections (the editor's
		// quick-bake) override cooked pak payloads, mirroring the loose-
		// file-overrides-pak overlay rule the sidecar era had.
		for (auto it = clusters.rbegin(); it != clusters.rend(); ++it) {
			const auto& c = *it;
			if (!c || !c->isDynamic)
				continue;
			if (const Cluster::BakePayload* p = c->FindBakePayload(vfsPath))
				return std::span<const uint8_t>(p->data(), p->size());
		}
		// Then pak clusters in registration order.
		for (const auto& c : clusters) {
			if (!c || c->isDynamic)
				continue;
			if (const Cluster::BakePayload* p = c->FindBakePayload(vfsPath))
				return std::span<const uint8_t>(p->data(), p->size());
		}
		return {};
	}

	void World::UnloadCluster(Cluster& cluster)
	{
		if (worldSceneInstances.Count() != 0)
			throw std::logic_error("cannot unload Cluster during direct World instance lifetime");
		// The dynamic cluster is runtime-created state, never pak
		// content: unloading it would drop every runtime entity — refuse.
		if (cluster.isDynamic)
			return;
		for (auto it = clusters.begin(); it != clusters.end(); ++it) {
			if (it->get() == &cluster) {
				gpuSceneClusterIds.erase(&cluster);
				// SHARED-ASSET PARKING: live external refs keep the
				// cluster's resource rows alive (an asset holder — app::
				// AssetStore — retained them). Detach instead of destroy:
				// the merged scene + every location table still point
				// into the cluster, so the full resolve rebuild runs over
				// the survivors exactly like a destroy (the scene
				// instance is GONE — no resurrection), but the cluster
				// object parks in retainedClusters until the last
				// ReleaseCluster destroys it.
				if (cluster.assetRefs > 0) {
					retainedClusters.push_back(std::move(*it));
					clusters.erase(it);
					ResolveAndFixup_clusters();
					return;
				}
				clusters.erase(it);
				// The merged scene + every location table still hold
				// pointers/indices into the destroyed cluster: rebuild
				// from the survivors. ResolveAndFixup is the only
				// rebuilder (load-time operation — not a hot path).
				ResolveAndFixup_clusters();
				return;
			}
		}
	}

	bool World::RetainCluster(Cluster& cluster)
	{
		// The dynamic cluster is runtime state, never an asset carrier.
		if (cluster.isDynamic)
			return false;
		for (const auto& c : clusters)
			if (c.get() == &cluster) {
				++cluster.assetRefs;
				return true;
			}
		for (const auto& c : retainedClusters)
			if (c.get() == &cluster) {
				++cluster.assetRefs;
				return true;
			}
		return false;
	}

	bool World::ReleaseCluster(Cluster& cluster)
	{
		if (cluster.assetRefs == 0)
			return false;
		if (--cluster.assetRefs > 0)
			return true;
		// Last external ref: a PARKED cluster's scene instance is
		// already gone — destroy it here (the mesh range frees ride
		// DestroyMesh's deferred retirement, the same contract as
		// UnloadCluster's destroy; no resolve is needed, a parked
		// cluster is outside the fold). A still-REGISTERED cluster
		// stays with its scene owner.
		for (auto it = retainedClusters.begin();
				it != retainedClusters.end(); ++it) {
			if (it->get() == &cluster) {
				retainedClusters.erase(it);
				return true;
			}
		}
		return true;
	}

	uint64_t World::InstantiateSceneHierarchy(
		const Scene& source, uint64_t sceneAssetId,
		const glm::dmat4& placement,
		std::vector<tour::asset::AssetLease> assets,
		std::string& err,
		const tour::scene::WorldSceneResourceMap* bindings)
	{
		err.clear();
		// World indices must point at already-resident render resources,
		// never source-local integer slots or expired Cluster resources.
		// The future page-backed AssetStore will populate these stable
		// locations without requiring a Cluster carrier.
		if (bindings) {
			for (const MeshRow& row : source.tables.meshes) {
				if (row.mesh >= bindings->mesh.size()
						|| !GetMesh(bindings->mesh[row.mesh])) {
					err = "local mesh has no resident World resource";
					return 0;
				}
			}
			for (const MaterialRow& row : source.tables.materials) {
				if (row.material >= bindings->material.size()
						|| !GetMaterial(bindings->material[row.material])) {
					err = "local material has no resident World resource";
					return 0;
				}
			}
			for (const LightRow& row : source.tables.lights) {
					if (row.light >= bindings->light.size()
						|| bindings->light[row.light] >= lightLocations.size()
						|| !lightLocations[bindings->light[row.light]].cluster) {
					err = "local light has no resident World resource";
					return 0;
				}
			}
			for (const CameraRow& row : source.tables.cameras) {
				if (row.camera >= bindings->camera.size()
						|| bindings->camera[row.camera] >= cameraLocations.size()
						|| !cameraLocations[bindings->camera[row.camera]].cluster) {
					err = "local camera has no resident World resource";
					return 0;
				}
			}
			for (const DecalRow& row : source.tables.decals) {
				if (row.decal >= bindings->decal.size()
						|| bindings->decal[row.decal] >= decalLocations.size()
						|| !decalLocations[bindings->decal[row.decal]].cluster) {
					err = "local decal has no resident World resource";
					return 0;
			}
			}
		}
		const uint64_t instance = worldSceneInstances.Instantiate(
			scene, regions, source, sceneAssetId, placement,
			std::move(assets), err, bindings);
		if (!instance) return 0;
		flags.resize(scene.hierarchy.size(), IteratorFlagBit_None);
		const auto* inst = worldSceneInstances.Find(instance);
		if (bindings) {
			const size_t oldDrawCount = drawItems.size();
			for (const MeshRow& sourceMesh : source.tables.meshes) {
				const NodeHandle nodeHandle =
					inst->sourceNodes[sourceMesh.node];
				const NodeId node = NodeHandleSlot(nodeHandle);
				if (!IsVisibleNode(scene, int(node))) continue;
				IteratorFlags f = static_cast<IteratorFlags>(
					IteratorFlagBit_Renderable)
					| static_cast<IteratorFlags>(IteratorFlagBit_StaticVertexFmt);
				if (IsStaticNode(scene, int(node)))
					f |= static_cast<IteratorFlags>(IteratorFlagBit_Static);
				if (IsCastsVolumetricShadowNode(scene, int(node)))
					f |= static_cast<IteratorFlags>(IteratorFlagBit_CastVolumetricShadow);
				const MaterialRow* matRow = FindMaterial(scene, node);
				if (matRow) {
					const Material* mat = GetMaterial(matRow->material);
					if (mat && mat->isTransparent())
						f |= static_cast<IteratorFlags>(IteratorFlagBit_Transparent);
					else {
						f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque);
						if (!mat || mat->castsShadows)
							f |= static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
						if (mat && mat->shadingModel == ShadingModel::Masked)
							f |= static_cast<IteratorFlags>(IteratorFlagBit_Masked);
					}
				} else {
					f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
						| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
				}
				flags[node] = f;
				drawItems.push_back(RenderItem{
					.node = node,
					.mesh = bindings->mesh[sourceMesh.mesh],
					.material = matRow ? matRow->material : kInvalidIndex,
					.flags = f,
					.materialInstance = 0,
				});
			}
			const auto byMaterial = [](const RenderItem& a,
						const RenderItem& b) {
				return a.material < b.material;
			};
			std::stable_sort(drawItems.begin() + oldDrawCount,
				drawItems.end(), byMaterial);
			std::inplace_merge(drawItems.begin(),
				drawItems.begin() + oldDrawCount,
				drawItems.end(), byMaterial);
			itemFlags.clear();
			itemFlags.reserve(drawItems.size());
			for (const RenderItem& item : drawItems)
				itemFlags.push_back(item.flags);
		}
		InvalidateLodCache();
		InvalidateStaticBvh();
		flagLists.clear();
		dirtyCull = true;
		++gpuSceneTopologyRevision;
		return instance;
	}

	bool World::RetireSceneHierarchy(uint64_t instanceId)
	{
		const auto* instance = worldSceneInstances.Find(instanceId);
		if (!instance) return false;
		std::vector<NodeId> ownedSlots;
		ownedSlots.reserve(instance->sourceNodes.size() + 1);
		for (const NodeHandle h : instance->sourceNodes)
			if (h != kNodeInvalidHandle)
				ownedSlots.push_back(NodeHandleSlot(h));
		if (instance->root != kNodeInvalidHandle)
			ownedSlots.push_back(NodeHandleSlot(instance->root));
		std::sort(ownedSlots.begin(), ownedSlots.end());
		if (!worldSceneInstances.Retire(scene, regions, instanceId))
			return false;
		for (const NodeId slot : ownedSlots)
			if (slot < flags.size()) flags[slot] = IteratorFlagBit_None;
		// Remove only this instance's draw records, never walk the entire
		// authored World graph or renumber another Scene's node slots.
		drawItems.erase(std::remove_if(drawItems.begin(), drawItems.end(),
			[&](const RenderItem& item) {
				return std::binary_search(ownedSlots.begin(), ownedSlots.end(),
					item.node);
			}), drawItems.end());
		itemFlags.clear();
		itemFlags.reserve(drawItems.size());
		for (const RenderItem& item : drawItems)
			itemFlags.push_back(item.flags);
		InvalidateLodCache();
		InvalidateStaticBvh();
		flagLists.clear();
		dirtyCull = true;
		++gpuSceneTopologyRevision;
		return true;
	}

	void World::RegisterCluster(std::unique_ptr<Cluster> cluster)
	{
		assert(cluster != nullptr);
		if (!cluster->isDynamic && worldSceneInstances.Count() != 0)
			throw std::logic_error("cannot register content Cluster during direct World instance lifetime");
		cluster->world = this;
		gpuSceneClusterIds[cluster.get()] = nextGpuSceneClusterId++;
		clusters.push_back(std::move(cluster));
	}

	std::unique_ptr<Cluster> World::UnregisterCluster(Cluster& cluster)
	{
		if (worldSceneInstances.Count() != 0)
			throw std::logic_error("cannot detach Cluster during direct World instance lifetime");
		// Same refusal as UnloadCluster: the dynamic cluster is runtime
		// state and stays with its world.
		if (cluster.isDynamic)
			return nullptr;
		for (auto it = clusters.begin(); it != clusters.end(); ++it) {
			if (it->get() == &cluster) {
				gpuSceneClusterIds.erase(&cluster);
				std::unique_ptr<Cluster> detached = std::move(*it);
				clusters.erase(it);
				detached->world = nullptr;
				// The detached cluster's nodes leave the merged scene:
				// rebuild over the survivors (same contract as
				// UnloadCluster; the detached cluster stays alive for
				// re-parenting into another world).
				ResolveAndFixup_clusters();
				return detached;
			}
		}
		// A PARKED cluster (retainedClusters) can be pulled out the
		// same way — it is already outside the fold, so no resolve is
		// needed; the asset refs stay (the cluster keeps its resource
		// rows wherever it lands next).
		for (auto it = retainedClusters.begin();
				it != retainedClusters.end(); ++it) {
			if (it->get() == &cluster) {
				std::unique_ptr<Cluster> detached = std::move(*it);
				retainedClusters.erase(it);
				detached->world = nullptr;
				return detached;
			}
		}
		return nullptr;
	}

	// Fold every registered cluster's Scene into the single World scene graph.
	// Each imported Scene keeps its resources in its owning Cluster's pools;
	// we only flatten the hierarchy/transforms/node->resource index maps here,
	// and remap the node->mesh/material indices to world-level resolution
	// slots that point back to the owning Cluster + handle.

	bool World::MarkClusterDirty(const smvk::Cluster& cluster)
	{
		for (const auto& registered : clusters) {
			if (registered.get() == &cluster) {
				clustersDirty_ = true;
				return true;
			}
		}
		return false;
	}

	bool World::ResolveDirtyClusters()
	{
		if (!clustersDirty_)
			return false;
		ResolveAndFixup_clusters();
		return true;
	}

	void World::ResolveAndFixup_clusters()
	{
		clustersDirty_ = false;
		// Never silently discard direct World-owned nodes while rebuilding
		// legacy Cluster copies. Complete the legacy renderer cutover before
		// combining both paths in one active World.
		if (worldSceneInstances.Count() != 0)
			throw std::logic_error("cannot resolve Cluster scenes while direct World scene instances are active");
		// Full legacy resolve replaces the node slots: reject stale game
		// handles after it, rather than accidentally mutating a reused slot.
		gameNodes_.clear();
		gameMeshBindings_.clear();
		// MATERIAL USERDATA RE-KEY CAPTURE (consumed by the re-key below
		// the material-table rebuild): the world material indices are
		// about to be rebuilt, so the per-material defaults are captured
		// keyed by the STABLE (owner cluster, pool handle) pair — they
		// survive resolves and cluster load/unload reordering exactly
		// like the entity bindings.
		std::vector<std::tuple<Cluster*, ClusterMaterialHandle, uint32_t>>
			oldMaterialUserData;
		{
			const size_t n = std::min(materialLocations.size(),
				materialUserData.size());
			oldMaterialUserData.reserve(n);
			for (size_t i = 0; i < n; ++i) {
				const MaterialLocation& loc = materialLocations[i];
				if (loc.cluster && loc.handle.valid())
					oldMaterialUserData.emplace_back(loc.cluster,
						loc.handle, materialUserData[i]);
			}
		}
		// MATERIAL SHADER-FAMILY RE-KEY CAPTURE (same stable-key shape —
		// the Phase B family lane rides the userdata lane's capture
		// discipline; only ProjectLit rows need carrying, StandardLit is
		// the zero default).
		std::vector<std::tuple<Cluster*, ClusterMaterialHandle, uint8_t>>
			oldMaterialShaderFamily;
		{
			const size_t n = std::min(materialLocations.size(),
				materialShaderFamily.size());
			oldMaterialShaderFamily.reserve(n);
			for (size_t i = 0; i < n; ++i) {
				if (materialShaderFamily[i]
						== uint8_t(smvk::MaterialShaderFamily::ProjectLit)) {
					const MaterialLocation& loc = materialLocations[i];
					if (loc.cluster && loc.handle.valid())
						oldMaterialShaderFamily.emplace_back(loc.cluster,
							loc.handle, materialShaderFamily[i]);
				}
			}
		}
		// The transitional cluster fold rebuilds every world node slot.
		// Never retain region handles across that rebuild: generations from
		// different Scene instances are not interchangeable (true for the
		// single-cluster bulk copy too - it swaps the Scene instance).
		regions = RegionRegistry{};
		// Fail BEFORE rebuilding if aggregate live nodes exceed the user
		// budget. The synthetic world root consumes one slot too.
		if (scene.nodeSlotBudget) {
			size_t needed = 1;
			for (const auto& c : clusters) {
				needed += tour::scene::LiveNodeCount(c->scene);
				if (needed > scene.nodeSlotBudget)
					tour::scene::CrashNodeBudget("World resolve", needed,
						scene.nodeSlotBudget);
			}
		}
		// Content-cluster identity (#228's layout-stability contract, FIXED):
		// the fast path applies when the world holds exactly ONE CONTENT
		// cluster - DYNAMIC clusters (EnsureDynamicCluster's spawn home)
		// are excluded from the count and fold by APPENDING after the
		// identity-copied primary. The primary's merged node ids therefore
		// NEVER move when a mesh-entity/animator/decal/audio spawn creates
		// the dynamic cluster mid-session (the old shape flipped to the
		// legacy +1-shift merge on the first spawn, moving every primary
		// node id and breaking raw-id holders). The dynamic cluster's own
		// ids ride clusterMergedNodes, which the entity layer already
		// survives; a second CONTENT cluster is a real topology event and
		// still takes the legacy merge.
		const Cluster* identityPrimary = nullptr;
		{
			size_t contentClusters = 0;
			for (const auto& c : clusters)
				if (c && !c->isDynamic) {
					++contentClusters;
					identityPrimary = c.get();
				}
			if (contentClusters != 1)
				identityPrimary = nullptr; // 0 or 2+: the legacy merge
		}
		const bool singleCluster = identityPrimary != nullptr;
		const size_t savedNodeSlotBudget = scene.nodeSlotBudget;
		if (singleCluster) {
			scene = identityPrimary->scene;
			// The bulk copy carries the CLUSTER scene's budget (unset for
			// cooked content); re-apply the world's user budget so the
			// overflow gate + the dynamic-spawn headroom survive the
			// resolve (ResetForResolve's job on the legacy path).
			if (savedNodeSlotBudget)
				scene.ReserveNodeSlots(savedNodeSlotBudget);
		}
		else if (scene.nodeSlotBudget)
			scene.ResetForResolve();
		else
			scene = Scene{};
		meshLocations.clear();
		materialLocations.clear();
		textureLocations.clear();
		lightLocations.clear();
		cameraLocations.clear();
		decalLocations.clear();
		meshLodInfos.clear();
		flags.clear();
		// The retained cluster-slot -> merged-node map (stable keys for
		// the entity layers; rebuilt with the world every resolve).
		clusterMergedNodes.assign(clusters.size(), {});
		// World material dedup (see the material-table remap below).
		std::unordered_map<uint64_t, uint32_t> worldMatByClusterMat;
		uint32_t clusterOrdinal = 0;
		// Authored-environment first-wins (see the per-cluster block):
		// the first cluster scene carrying an authored look wins the
		// world scene's resolved/default blocks.
		bool envTaken = false;
		// Default-camera first-wins (same contract): the first cluster
		// scene with an authored default camera wins the world slot.
		bool defaultCamTaken = false;
		// Active-environment first-wins (same contract): the first
		// cluster scene with a selected env block wins the world slot
		// (first scene loaded into the world wins). Remapped below.
		bool activeEnvTaken = false;
		// Render-settings first-wins (same contract as the env above —
		// the look + the render config are scene-global, one block):
		// the first cluster scene carrying an AUTHORED render block
		// wins the world slots (hasRenderSettings=false never wins, so
		// an unauthored cluster never shadows an authored one).
		bool renderTaken = false;

		// Append the synthetic root to the copied single-cluster scene.
		// Do not recycle a retired source slot (node ids must stay 1:1).
		std::vector<uint32_t> savedFreeList;
		if (singleCluster) {
			savedFreeList = std::move(scene.nodeFreeList);
			scene.nodeFreeList.clear();
		}
		const int root = addNode(scene, -1, 0);
		if (singleCluster)
			scene.nodeFreeList = std::move(savedFreeList);
		scene.localTransform[root] = glm::mat4(1.0f);
		scene.globalTransform[root] = glm::mat4(1.0f);
		if (!singleCluster) {
			scene.staticRoot.assign(scene.hierarchy.size(), 0);
			scene.staticNode.assign(scene.hierarchy.size(), 0);
			scene.noLightmap.assign(scene.hierarchy.size(), 0);
			scene.lightmap.assign(scene.hierarchy.size(), 0);
			scene.castsVolumetricShadow.assign(scene.hierarchy.size(), 0);
			scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
			scene.hidden.assign(scene.hierarchy.size(), 0);
		}

		for (auto& clusterPtr : clusters) {
			Cluster& cluster = *clusterPtr.get();
			Scene& cs = cluster.scene;

			// Slot-association map (Epoch 6b): cluster slot -> world
			// NodeId, UINT32_MAX for holes. The dense-era +nodeOffset
			// arithmetic became this explicit map — numerically
			// identical for a dense cluster scene (fresh imports/loads
			// allocate slots 0,1,2,...), correct when the cluster scene
			// carries holes (a cook-time static-merge retirement).
			const uint32_t clusterSlotCount =
				static_cast<uint32_t>(cs.hierarchy.size());
			std::vector<NodeId> nodeMap(clusterSlotCount, UINT32_MAX);
			// The identity fast path is PER-CLUSTER: the one content
			// primary keeps source slot ids (the bulk copy above already
			// placed its nodes); every other cluster - the dynamic spawn
			// home, or the second content cluster that disabled the fast
			// path - folds through the legacy append below.
			const bool isIdentityPrimary = identityPrimary == &cluster;
			if (isIdentityPrimary) {
				for (uint32_t s = 0; s < clusterSlotCount; ++s)
					if (IsLiveSlot(cs, s))
						nodeMap[s] = s;
			}

			// Pass 1: allocate a world slot per LIVE cluster node and
			// copy its payload (level as-is; links stay default until
			// pass 2). addNode appends sequentially on the freshly
			// wiped scene, so world ids match the old dense layout
			// exactly until the world scene itself grows holes.
			if (!isIdentityPrimary) for (uint32_t s = 0; s < clusterSlotCount; ++s) {
				if (!IsLiveSlot(cs, s))
					continue;
				const int dst = addNode(scene, -1, cs.hierarchy[s].level);
				assert(dst >= 0);
				nodeMap[s] = static_cast<NodeId>(dst);
				scene.localTransform[size_t(dst)] = cs.localTransform[s];
				scene.globalTransform[size_t(dst)] = cs.globalTransform[s];
				scene.localTransformDouble[size_t(dst)] = s < cs.localTransformDouble.size()
					? cs.localTransformDouble[s] : glm::dmat4(cs.localTransform[s]);
				scene.globalTransformDouble[size_t(dst)] = tour::scene::GlobalTransformDouble(cs, s);
				scene.localTransformFloatShadow[size_t(dst)] = scene.localTransform[size_t(dst)];
				// Flag tables: per-slot guarded copies (missing table ==
				// defaults; addNode already pushed zeros). staticNode is
				// rebuilt wholesale by RebuildStaticCache below.
				if (cs.staticRoot.size() > s)
					scene.staticRoot[size_t(dst)] = cs.staticRoot[s];
				if (cs.noLightmap.size() > s)
					scene.noLightmap[size_t(dst)] = cs.noLightmap[s];
				if (cs.lightmap.size() > s)
					scene.lightmap[size_t(dst)] = cs.lightmap[s];
				if (cs.ditherCloseup.size() > s)
					scene.ditherCloseup[size_t(dst)] = cs.ditherCloseup[s];
				if (cs.castsVolumetricShadow.size() > s)
					scene.castsVolumetricShadow[size_t(dst)] =
						cs.castsVolumetricShadow[s];
				if (cs.hidden.size() > s)
					scene.hidden[size_t(dst)] = cs.hidden[s];
			}

			// Pass 2: hierarchy links through the map. A live node's
			// links point at live nodes by the deletion discipline;
			// UINT32_MAX (or a stale link) falls back to -1.
			const auto mapLink = [&nodeMap](int link) -> int {
				if (link < 0 || size_t(link) >= nodeMap.size())
					return -1;
				const NodeId mapped = nodeMap[size_t(link)];
				return (mapped != UINT32_MAX) ? int(mapped) : -1;
			};
			// Retain the map (the entity layers' stable-key -> merged
			// node resolution; see World::clusterMergedNodes) + remap
			// the live audio-source entities' merged nodes — the
			// HANDLE stays valid across the rebuild, only its internal
			// node moves (the entity-model contract).
			clusterMergedNodes[clusterOrdinal] = nodeMap;
			for (AudioSourceEntity& e : audioEntities)
				if (e.alive && e.owner == &cluster
						&& e.clusterNode < nodeMap.size())
					e.mergedNode = nodeMap[e.clusterNode];
			if (!isIdentityPrimary) for (uint32_t s = 0; s < clusterSlotCount; ++s) {
				if (nodeMap[s] == UINT32_MAX)
					continue;
				const Hierarchy& sh = cs.hierarchy[s];
				Hierarchy& h = scene.hierarchy[nodeMap[s]];
				h.parent = mapLink(sh.parent);
				h.firstChild = mapLink(sh.firstChild);
				h.nextSibling = mapLink(sh.nextSibling);
				h.lastSibling = mapLink(sh.lastSibling);
			}

			// Reparent the cluster root under the world root. The root
			// is the cluster's FIRST LIVE slot (slot 0 of a dense scene
			// — the old nodeOffset node). EMPTY cluster scenes (a
			// payload-only dynamic cluster, a cook cluster with assets
			// but no nodes) contribute no hierarchy: every map entry is
			// UINT32_MAX — skip (nothing to reparent or dirty below).
			NodeId clusterRootNode = UINT32_MAX;
			for (uint32_t s = 0; s < clusterSlotCount; ++s)
				if (nodeMap[s] != UINT32_MAX) {
					clusterRootNode = nodeMap[s];
					break;
				}
			if (clusterRootNode != UINT32_MAX) {
				scene.hierarchy[clusterRootNode].parent = root;
				if (scene.hierarchy[root].firstChild == -1) {
					scene.hierarchy[root].firstChild = clusterRootNode;
				}
				else {
					const uint32_t prevFirst =
						scene.hierarchy[root].firstChild;
					scene.hierarchy[clusterRootNode].nextSibling = prevFirst;
					scene.hierarchy[root].firstChild = clusterRootNode;
				}
				scene.hierarchy[clusterRootNode].level += 1;
				// Expose each imported subtree as a registered ownership
				// boundary. This compatibility path still holds resources in
				// Cluster pools until the asset-owner migration is complete.
				regions.Register(scene, MakeNodeHandle(scene, clusterRootNode),
					cluster.isDynamic ? RegionKind::Runtime : RegionKind::Scene);
			}

			// Remap node metadata (names index into nodeNames after the
			// bulk pool append; uuids ride unchanged) per live slot.
			// The offset is the pool size AT THE APPEND: pass 1's addNode
			// calls above append auto-names to the pool (addNode names
			// every node from allocation), so the pre-pass-1 offset left
			// every merged name pointing into the auto-name block
			// (world_entity_tests: resolve/schemafold name-at-row-node).
			if (!isIdentityPrimary) {
			const uint32_t clusterNameOffset =
				static_cast<uint32_t>(scene.nodeNames.size());
			scene.nodeNames.insert(
				scene.nodeNames.end(),
				cs.nodeNames.begin(),
				cs.nodeNames.end()
			);
			for (uint32_t s = 0; s < clusterSlotCount; ++s) {
				if (nodeMap[s] == UINT32_MAX || s >= cs.metadata.size())
					continue;
				if (cs.metadata[s].name != 0xFFFFFFFFu)
					SetNodeNameIndex(scene, nodeMap[s],
						cs.metadata[s].name + clusterNameOffset);
				SetNodeUuidRaw(scene, nodeMap[s], cs.metadata[s].uuid);
			}
			}

			// Remap the mesh table: cluster-local handle index -> world
			// mesh index (one location slot per row — mesh slots stay
			// per-node, CPU-side only). Rows of retired slots cannot
			// exist through the mutators; the map guard is the backstop.
			for (const MeshRow& row : cs.tables.meshes) {
				if (row.node >= nodeMap.size()
					|| nodeMap[row.node] == UINT32_MAX)
					continue;
				const uint32_t worldNode = nodeMap[row.node];
				const uint32_t worldMeshIdx =
					static_cast<uint32_t>(meshLocations.size());

				ClusterStaticMeshHandle handle =
					cluster.staticMeshes.getHandle(row.mesh);

				meshLocations.push_back(
					MeshLocation{ .cluster = &cluster, .handle = handle }
				);
				// Cook-time LOD marker (LOD0; siblings resolve via SelectLodIndex).
				LodInfo lod;
				lod.groupId = row.mesh;
				if (const StringViewT uri = cluster.GetMeshUri(handle);
					!uri.empty()) {
					if (auto hit = cluster.GetMeshLod(uri))
						lod = *hit;
				}
				if (const StaticMesh* m = cluster.staticMeshes.get(handle))
					lod.triCount = m->geometry.indexCount / 3;
				meshLodInfos.push_back(lod);
				SetMesh(scene, worldNode, worldMeshIdx);
			}

			// Remap the material table: cluster-local handle index ->
			// world mat idx, deduplicated by (cluster, cluster-local
			// material): instanced scenes share a handful of materials
			// across hundreds of thousands of nodes (scanlands: 96
			// materials over 374k instances). One slot per instance
			// overflows the GPU materials buffer (renderer limits:
			// engine::renderer::limits::MaxMaterials) and shaders read
			// OOB past it (GPU fault / device loss on heavy views).
			// Material slots are GPU-indexed by materialId, so they
			// must stay addressable.
			for (const MaterialRow& row : cs.tables.materials) {
				if (row.node >= nodeMap.size()
					|| nodeMap[row.node] == UINT32_MAX)
					continue;
				const uint32_t worldNode = nodeMap[row.node];
				const uint64_t matKey =
					(static_cast<uint64_t>(clusterOrdinal) << 32) | row.material;
				auto matHit = worldMatByClusterMat.find(matKey);
				if (matHit != worldMatByClusterMat.end()) {
					SetMaterial(scene, worldNode, matHit->second);
					continue;
				}
				const uint32_t worldMatIdx =
					static_cast<uint32_t>(materialLocations.size());

				ClusterMaterialHandle handle =
					cluster.materials.getHandle(row.material);

				materialLocations.push_back(
					MaterialLocation{ .cluster = &cluster, .handle = handle }
				);
				worldMatByClusterMat[matKey] = worldMatIdx;
				SetMaterial(scene, worldNode, worldMatIdx);
			}

			// Remap the light table: cluster-local light pool index ->
			// world light index. lightLocations resolves back to Cluster +
			// handle exactly like meshLocations (cooked lights are pool
			// assets).
			for (const LightRow& row : cs.tables.lights) {
				if (row.node >= nodeMap.size()
					|| nodeMap[row.node] == UINT32_MAX)
					continue;
				const uint32_t worldNode = nodeMap[row.node];
				const uint32_t worldLightIdx =
					static_cast<uint32_t>(lightLocations.size());

				ClusterLightHandle handle =
					cluster.lights.getHandle(row.light);

				lightLocations.push_back(
					LightLocation{ .cluster = &cluster, .handle = handle }
				);
				SetLight(scene, worldNode, worldLightIdx);
			}

			// Remap the camera table: cluster-local camera pool index ->
			// world camera index. cameraLocations resolves back to Cluster
			// + handle exactly like lightLocations (authored cameras are
			// pool assets).
			for (const CameraRow& row : cs.tables.cameras) {
				if (row.node >= nodeMap.size()
					|| nodeMap[row.node] == UINT32_MAX)
					continue;
				const uint32_t worldNode = nodeMap[row.node];
				const uint32_t worldCamIdx =
					static_cast<uint32_t>(cameraLocations.size());

				ClusterCameraHandle handle =
					cluster.cameras.getHandle(row.camera);

				cameraLocations.push_back(
					CameraLocation{ .cluster = &cluster, .handle = handle }
				);
				SetCamera(scene, worldNode, worldCamIdx);
			}

			// Remap the decal table: cluster-local decal pool index ->
			// world decal index. decalLocations resolves back to Cluster +
			// handle exactly like lightLocations (authored/runtime decals
			// are pool assets; the shape rides the node transform).
			for (const DecalRow& row : cs.tables.decals) {
				if (row.node >= nodeMap.size()
					|| nodeMap[row.node] == UINT32_MAX)
					continue;
				const uint32_t worldNode = nodeMap[row.node];
				const uint32_t worldDecalIdx =
					static_cast<uint32_t>(decalLocations.size());

				ClusterDecalHandle handle =
					cluster.decals.getHandle(row.decal);

				decalLocations.push_back(
					DecalLocation{ .cluster = &cluster, .handle = handle }
				);
				SetDecal(scene, worldNode, worldDecalIdx);
			}

			// Authored environment: per-node rows offset-merge like the
			// other tables (values are inline structs — direct copy);
			// the resolved look + default block ride first-wins across
			// clusters (registration order: the owning scene first), so an
			// unauthored default never shadows another scene's authored look.
			for (const EnvRow& row : cs.tables.envs)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					SetEnv(scene, nodeMap[row.node], row.environment);
			// Volume settings ride the same remap (every block coexists;
			// no first-wins — coverage/priority resolve at use).
			for (const EnvVolumeRow& row : cs.tables.envVolumes)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					SetEnvVolume(scene, nodeMap[row.node], row.volume);
			// GI volumes / cubemap pins / export settings ride the
			// resolve NOW (Epoch 6 — the map era left them
			// cluster-scene-only with tour replay as the only
			// transport; the world scene is a complete representation
			// from here on). Payloads verbatim, nodes through the map;
			// export rows keep their editor/cook-only contract (never
			// packed into runtime content — the resolve carries them
			// for the editor's live view, the pak writer still skips
			// them).
			for (const GiVolumeRow& row : cs.tables.giVolumes)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					SetGiVolume(scene, nodeMap[row.node], row.volume);
			for (const CubemapRow& row : cs.tables.cubemaps)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					SetCubemap(scene, nodeMap[row.node], row.cubemap);
			for (const ExportRow& row : cs.tables.exports)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					SetExport(scene, nodeMap[row.node], row.settings);
			// Audio sources (the emitter association) ride the same
			// remap: payloads verbatim (the sounds list is asset
			// references), nodes through the map — the audio manager
			// reads the merged table and resolves each sound id/asset
			// path through the project layer at use.
			for (const AudioSourceRow& row : cs.tables.audioSources)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					SetAudioSource(scene, nodeMap[row.node], row.source);
			// Audio rooms (the environmental-reverb volume): payloads
			// verbatim, nodes through the map — the audio runtime
			// evaluates each room's shape against the MERGED global
			// transform at the listener, every update (never a cached
			// merged id — the map is rebuilt per resolve).
			for (const tour::scene::AudioRoomRow& row : cs.tables.audioRooms)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					tour::scene::SetAudioRoom(scene, nodeMap[row.node], row.room);
			for (const tour::scene::PhysicsBodyRow& row : cs.tables.physicsBodies)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					tour::scene::SetPhysicsBody(scene, nodeMap[row.node], row.body);
			for (const tour::scene::TriggerVolumeRow& row : cs.tables.triggerVolumes)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					tour::scene::SetTriggerVolume(scene, nodeMap[row.node], row.trigger);
			// Project-defined schema associations (node_schema.h): the
			// cooked value bytes ride VERBATIM (no decode at fold time
			// — the generated project::types own the decode), nodes
			// through the map. Game code iterates the merged table +
			// reads each row's node transform from the merged scene
			// (the instance-qualified identity).
			for (const SchemaAssocRow& row : cs.tables.schemaAssocs)
				if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
					SetSchemaAssoc(scene, nodeMap[row.node],
						row.schemaId, row.values);
			if (!envTaken && cs.envAuthored) {
				scene.environment = cs.environment;
				scene.envAuthored = true;
				scene.defaultEnvironment = cs.defaultEnvironment;
				envTaken = true;
			}
			// Render settings (the postfx LOOK — see
			// SceneRenderSettings): first-wins like the resolved env
			// above. Without this the world scene kept struct defaults
			// and the renderer's PushPostfxEnv seed never saw an
			// authored look at all (pak-hosted worlds lost the block;
			// the editor's borrowed single-world path never noticed —
			// it reads its own scene). (The GI/shadow quality knobs are
			// NOT part of this — host-owned session state; see
			// RenderQualityConfig.)
			if (!renderTaken && cs.hasRenderSettings) {
				scene.renderSettings = cs.renderSettings;
				scene.defaultRenderSettings = cs.defaultRenderSettings;
				scene.hasRenderSettings = true;
				renderTaken = true;
			}
			// Default camera (world settings' typed camera.default field):
			// cluster slot -> world slot through the map, first-wins
			// across clusters like the resolved env (the owning scene's
			// default stands; UINT32_MAX = none is a no-op).
			if (!defaultCamTaken && cs.defaultCameraNode != UINT32_MAX
				&& cs.defaultCameraNode < clusterSlotCount
				&& nodeMap[cs.defaultCameraNode] != UINT32_MAX) {
				scene.defaultCameraNode = nodeMap[cs.defaultCameraNode];
				defaultCamTaken = true;
			}
			// Active environment (tour env.current / editor selection):
			// same first-wins remap. The merged look resolves from the
			// winning id after the loop (see below); UINT32_MAX = none.
			if (!activeEnvTaken && cs.activeEnvironment != UINT32_MAX
				&& cs.activeEnvironment < clusterSlotCount
				&& nodeMap[cs.activeEnvironment] != UINT32_MAX) {
				scene.activeEnvironment = nodeMap[cs.activeEnvironment];
				activeEnvTaken = true;
			}

			if (clusterRootNode != UINT32_MAX)
				markAsChanged(scene, int(clusterRootNode));
			++clusterOrdinal;
		}

		// Active-environment pick: the winning id resolves the merged
		// look from the merged block table (first scene loaded wins).
		// Falls back to the first-wins authored/default copy above when
		// no scene selected a block (or the target went stale).
		if (scene.activeEnvironment != UINT32_MAX) {
			if (const EnvRow* row =
					FindEnv(scene, scene.activeEnvironment)) {
				scene.environment = row->environment;
				scene.envAuthored = true;
			}
			else
				scene.activeEnvironment = UINT32_MAX;
		}

		// Layered materials: remap cluster-local layer ids to world material
		// slots. Layer ids in MaterialValues are cluster-local pool indices
		// (cook/pak domain); the GPU buffer is indexed by world materialId, so
		// each referenced layer material needs a world slot — appended here
		// when no node references it directly (the dedup loop above only
		// creates slots for node-referenced materials).
		for (uint32_t clusterOrdinal = 0;
			clusterOrdinal < (uint32_t)clusters.size(); ++clusterOrdinal) {
			Cluster* cluster = clusters[clusterOrdinal].get();
			if (!cluster)
				continue;
			cluster->materials.forEachHandle(
				[&](ClusterMaterialHandle handle) {
				Material* mat = cluster->materials.get(handle);
				if (!mat || mat->values.layerMaterials.w == 0)
					return;
				for (int layer = 0; layer < 3; ++layer) {
					const int32_t localIdx = mat->values.layerMaterials[layer];
					if (localIdx < 0)
						continue;
					const uint64_t matKey =
						(static_cast<uint64_t>(clusterOrdinal) << 32)
						| static_cast<uint32_t>(localIdx);
					auto hit = worldMatByClusterMat.find(matKey);
					uint32_t worldMatIdx = 0;
					if (hit != worldMatByClusterMat.end()) {
						worldMatIdx = hit->second;
					}
					else {
						ClusterMaterialHandle layerHandle =
							cluster->materials.getHandle(
								static_cast<uint32_t>(localIdx));
						if (layerHandle.empty())
							continue;
						worldMatIdx = static_cast<uint32_t>(
							materialLocations.size());
						materialLocations.push_back(
							MaterialLocation{
								.cluster = cluster,
								.handle = layerHandle });
						worldMatByClusterMat[matKey] = worldMatIdx;
					}
					mat->values.layerMaterials[layer] =
						static_cast<int32_t>(worldMatIdx);
				}
			});
		}

		// MATERIAL USERDATA RE-KEY: re-emit the captured defaults into
		// the new index layout (values keyed by the stable (owner,
		// handle) pair; unset entries read 0 — the neutral default).
		materialUserData.assign(materialLocations.size(), 0u);
		for (size_t i = 0; i < materialLocations.size(); ++i) {
			const MaterialLocation& loc = materialLocations[i];
			if (!loc.cluster || !loc.handle.valid())
				continue;
			for (const auto& [c, h, v] : oldMaterialUserData) {
				if (c == loc.cluster && h == loc.handle) {
					materialUserData[i] = v;
					break;
				}
			}
		}

		// MATERIAL SHADER-FAMILY RE-KEY (same shape as the userdata
		// re-key above — the family assignments survive the rebuild
		// keyed by the stable (owner, handle) pair; unset entries read
		// StandardLit, the fail-safe stock pick).
		materialShaderFamily.assign(materialLocations.size(),
			uint8_t(smvk::MaterialShaderFamily::StandardLit));
		for (size_t i = 0; i < materialLocations.size(); ++i) {
			const MaterialLocation& loc = materialLocations[i];
			if (!loc.cluster || !loc.handle.valid())
				continue;
			for (const auto& [c, h, v] : oldMaterialShaderFamily) {
				if (c == loc.cluster && h == loc.handle) {
					materialShaderFamily[i] = v;
					break;
				}
			}
		}

		// The runtime MESH entities' mirrors: the resolve wiped the
		// merged scene, so each entity's durable transform node remaps
		// through its owner's map and the mesh + material rows are
		// RE-ESTABLISHED on it (the bindings are the stable (cluster,
		// pool handle) pairs — the world indices were just rebuilt).
		// Runs BEFORE the flags/drawItems build below (it used to run
		// at the tail, which left the freshly re-mirrored entity rows
		// outside drawItems until a SECOND resolve — spawned entities
		// never reached forEachRenderItem consumers at all): every
		// iterator flag, the compact draw list, the LOD chains and the
		// static BVH now see the entity mirrors exactly like cooked
		// content. The bound material-instance HANDLE VALUE joins into
		// entityInstanceByNode for the drawItems build (the per-item
		// userdata linkage; the VALUE stays live in the pool).
		std::unordered_map<NodeId, uint64_t> entityInstanceByNode;
		for (MeshEntity& e : meshEntities) {
			if (!e.alive || !e.transformOwner || !e.meshOwner)
				continue;
			e.mergedNode = UINT32_MAX;
			// The durable node's merged id (find the owner's ordinal).
			for (size_t ci = 0; ci < clusters.size(); ++ci) {
				if (clusters[ci].get() != e.transformOwner
						|| ci >= clusterMergedNodes.size())
					continue;
				const auto& merged = clusterMergedNodes[ci];
				if (e.clusterNode < merged.size()
						&& merged[e.clusterNode] != UINT32_MAX)
					e.mergedNode = merged[e.clusterNode];
				break;
			}
			if (e.mergedNode == UINT32_MAX
					|| e.mergedNode >= scene.hierarchy.size())
				continue;
			const uint32_t worldMeshIdx = WorldIndexOfMesh(
				meshLocations, e.meshOwner, e.mesh);
			if (worldMeshIdx != UINT32_MAX)
				SetMesh(scene, e.mergedNode, worldMeshIdx);
			if (e.material.valid()) {
				// The material's owner is whatever cluster held it at
				// spawn (cross-cluster materials ride the same table);
				// search by handle across owners (the common case is
				// the mesh's own cluster).
				uint32_t worldMatIdx = WorldIndexOfMaterial(
					materialLocations, e.meshOwner, e.material);
				if (worldMatIdx == UINT32_MAX) {
					for (const auto& loc : materialLocations)
						if (loc.handle == e.material) {
							worldMatIdx = uint32_t(&loc
								- materialLocations.data());
							break;
						}
				}
				if (worldMatIdx != UINT32_MAX)
					SetMaterial(scene, e.mergedNode, worldMatIdx);
			}
			if (e.materialInstance)
				entityInstanceByNode[e.mergedNode] =
					e.materialInstance.value;
		}
		// (The animators' target clusters persist through resolves —
		// their rest poses + cursors stand; the next Update re-flushes
		// the pose onto the rebuilt cluster scene.)

		RebuildStaticCache(scene);
		// Full-recalc marking at the merge boundary: root-level nodes
		// (the cooker's WorldSun is addNode(scene,-1,1)) fall outside
		// node 0's subtree - without this their world globals keep the
		// identity default and the sun decodes horizontal (az=0/el=0).
		MarkAllRootsChanged(scene);
		recalculateGlobalTransforms(scene);

		RefreshRuntimeSceneCaches();

		// (The mesh entities' mirrors were re-established EARLIER in
		// this resolve — before the flags/drawItems build, so their
		// rows take part in the iterator lists; see the moved block
		// above. The animators' target clusters persist through
		// resolves — their rest poses + cursors stand; the next Update
		// re-flushes the pose onto the rebuilt cluster scene.)
	}

	NodeHandle World::AddNode(const glm::mat4& localFrame)
	{
		const int node = addNode(scene, -1, 0);
		if (node < 0)
			return kNodeInvalidHandle;
		// Preserve the ABA guarantee even across the legacy full-scene
		// resolve: its replacement Scene may reuse the same slot number.
		uint32_t& highWater = gameSlotGeneration_[NodeId(node)];
		const uint32_t sceneGeneration = NodeGenerationOf(scene, NodeId(node));
		const uint32_t generation = std::max(highWater + 1u, sceneGeneration);
		if (generation >= tour::scene::kNodeGenMask) {
			RetireNodeSlot(scene, NodeId(node));
			return kNodeInvalidHandle;
		}
		highWater = generation;
		scene.nodeGeneration[size_t(node)] =
			tour::scene::kNodeSlotAliveBit | generation;
		const NodeHandle handle = MakeNodeHandle(scene, NodeId(node));
		gameNodes_.insert(handle);
		(void)tour::scene::SetLocalTransform(scene, handle, localFrame);
		scene.globalTransform[size_t(node)] = localFrame;
		markAsChanged(scene, node);
		RefreshRuntimeSceneCaches();
		return handle;
	}

	bool World::RemoveNode(NodeHandle handle)
	{
		if (!IsGameNode(handle))
			return false;
		const NodeId node = NodeHandleSlot(handle);
		// Only unowned game nodes can be removed through this entry point:
		// authored/cluster nodes are managed by their scene owners.
		for (const auto& maps : clusterMergedNodes)
			for (const NodeId source : maps)
				if (source == node)
					return false;
		for (const MeshEntity& entity : meshEntities)
			if (entity.alive && entity.mergedNode == node)
				return false;
		for (const AudioSourceEntity& entity : audioEntities)
			if (entity.alive && entity.mergedNode == node)
				return false;
		deleteSceneNodes(scene, { node });
		gameNodes_.erase(handle);
		gameMeshBindings_.erase(handle);
		RefreshRuntimeSceneCaches();
		return true;
	}

	// Rebuild runtime draw/cull caches after in-place node or component
	// edits without invoking the destructive cluster resolve.
	void World::RefreshRuntimeSceneCaches()
	{
		std::unordered_map<NodeId, uint64_t> entityInstanceByNode;
		for (const MeshEntity& entity : meshEntities)
			if (entity.alive && entity.mergedNode != UINT32_MAX
					&& entity.materialInstance)
				entityInstanceByNode[entity.mergedNode] =
					entity.materialInstance.value;
		// Build the per-node iterator flags from the resolved scene: renderable
		// nodes are those carrying a mesh; material drives Opaque vs Transparent.
		// Hidden subtrees (Clarisse-style visibility overrides) are excluded:
		// they keep None so no pass iterates them (see drawItems below).
		// TEST RIG (see the CastVolumetricShadow site below): parsed
		// once at the cook, before the loop.
		const bool sFogShadowMarkAllTestRig = [] {
			const char* e = std::getenv("TOURMALINE_FOG_SHADOW_MARK_ALL");
			return e && std::atoi(e) != 0;
		}();
		flags.assign(scene.hierarchy.size(), IteratorFlagBit_None);
		for (const MeshRow& row : scene.tables.meshes) {
			const NodeId node = row.node;
			if (node >= flags.size())
				continue;
			if (!IsVisibleNode(scene, int(node)))
				continue;

			IteratorFlags f = static_cast<IteratorFlags>(IteratorFlagBit_Renderable)
				| static_cast<IteratorFlags>(IteratorFlagBit_StaticVertexFmt);
			if (IsStaticNode(scene, (int)node))
				f |= static_cast<IteratorFlags>(IteratorFlagBit_Static);
			// The coarse fog-tile caster mark (scene-owned, material-
			// independent - see the bit's note): large occluders the
			// dedicated volumetric shadow pass renders. TEST RIG
			// (TOURMALINE_FOG_SHADOW_MARK_ALL=1, the headless A/B
			// override - the editor's per-node checkbox is the
			// authoring surface, this only exists so the tile machinery
			// is validatable without authored content): mark EVERY
			// opaque surface caster as a volumetric caster too.
			if (IsCastsVolumetricShadowNode(scene, int(node))
				|| (sFogShadowMarkAllTestRig
					&& FindMaterial(scene, node) != nullptr))
				f |= static_cast<IteratorFlags>(
					IteratorFlagBit_CastVolumetricShadow);

			// Material-driven classification. Falls back to Opaque if node has
			// no material (e.g. default).
			if (const MaterialRow* matRow = FindMaterial(scene, node)) {
				uint32_t matIdx = matRow->material;
				if (matIdx < materialLocations.size()) {
					const MaterialLocation& loc = materialLocations[matIdx];
					if (loc.cluster && loc.handle.valid()) {
						if (auto* mat = loc.cluster->materials.get(loc.handle)) {
							if (mat->isTransparent())
								f |= static_cast<IteratorFlags>(IteratorFlagBit_Transparent);
							else {
								f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque);
								// Cook-classified non-casters (decal overlays,
								// see SMVK_materials_shadows) drop out of the
								// shadow passes only; Opaque keeps them in the
								// depth prepass / main view.
								if (mat->castsShadows)
									f |= static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);

								// Masked (cutout) still casts shadow but via alpha-tested path.
								// Transparent (BLEND) does not cast: full-quad depth would
								// darken behind glass/foliage. Shadow passes additionally
								// filter Opaque so this flag is defense-in-depth.
								if (mat->shadingModel == ShadingModel::Masked)
									f |= static_cast<IteratorFlags>(IteratorFlagBit_Masked);
							}
						}
						else {
							f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
								| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
						}
					}
					else {
						f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
							| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
					}
				}
				else {
					f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
						| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
				}
			}
			else {
				f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
					| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
			}

			flags[node] = f;
		}

		// Compact draw list for the hot-pass iterator (forEachRenderItem):
		// one entry per renderable node with mesh + material indices
		// precomputed, material-sorted so per-material pipeline binds and
		// phase splits happen once per material per pass instead of per
		// node. stable_sort keeps hierarchy order within a material.
		drawItems.clear();
		drawItems.reserve(scene.tables.meshes.size());
		flagLists.clear();
		for (const MeshRow& row : scene.tables.meshes) {
			const NodeId node = row.node;
			if (node >= flags.size())
				continue;
			if (!IsVisibleNode(scene, int(node)))
				continue; // hidden override: not drawn, not picked, not baked-listed
			const MaterialRow* matRow = FindMaterial(scene, node);
			// The per-item material-instance linkage (the entity mirror
			// block above joins bound instances by merged node; static
			// scene nodes carry none in v1). The HANDLE VALUE, not a
			// slot index: a destroyed instance (or a younger one that
			// reused the slot) fails ResolvedItemUserData's generation
			// check and cleanly falls back to the base default.
			uint64_t instanceLink = 0;
			if (const auto it = entityInstanceByNode.find(node);
					it != entityInstanceByNode.end())
				instanceLink = it->second;
			drawItems.push_back(RenderItem{
				.node = node,
				.mesh = row.mesh,
				.material = matRow ? matRow->material : kInvalidIndex,
				.flags = flags[node],
				.materialInstance = instanceLink,
				});
		}
		std::stable_sort(drawItems.begin(), drawItems.end(),
			[](const RenderItem& a, const RenderItem& b) {
			return a.material < b.material;
		});
		// SoA flags mirror, rebuilt AFTER the sort so index i means the
		// same item in both arrays (the SIMD scan in ItemIndicesFor
		// walks this column).
		itemFlags.clear();
		itemFlags.reserve(drawItems.size());
		for (const RenderItem& item : drawItems)
			itemFlags.push_back(item.flags);

		RebuildLodChains();
		RebuildLightmapChartTable();
		RebuildStaticBvh();
		++gpuSceneTopologyRevision; // authored/runtime topology or binding remap
		dirtyCull = true;

	}

	StaticMesh* World::GetMesh(uint32_t meshIndex)
	{
		if (meshIndex >= meshLocations.size())
			return nullptr;

		const MeshLocation& loc = meshLocations[meshIndex];
		if (loc.cluster == nullptr || !loc.handle.valid())
			return nullptr;

		return loc.cluster->staticMeshes.get(loc.handle);
	}

	const StaticMesh* World::GetMesh(uint32_t meshIndex) const
	{
		if (meshIndex >= meshLocations.size())
			return nullptr;

		const MeshLocation& loc = meshLocations[meshIndex];
		if (loc.cluster == nullptr || !loc.handle.valid())
			return nullptr;

		return loc.cluster->staticMeshes.get(loc.handle);
	}

	Material* World::GetMaterial(uint32_t materialIndex)
	{
		if (materialIndex >= materialLocations.size())
			return nullptr;

		const MaterialLocation& loc = materialLocations[materialIndex];
		if (loc.cluster == nullptr || !loc.handle.valid())
			return nullptr;

		return loc.cluster->materials.get(loc.handle);
	}

	const Material* World::GetMaterial(uint32_t materialIndex) const
	{
		if (materialIndex >= materialLocations.size())
			return nullptr;

		const MaterialLocation& loc = materialLocations[materialIndex];
		if (loc.cluster == nullptr || !loc.handle.valid())
			return nullptr;

		return loc.cluster->materials.get(loc.handle);
	}

	const LodInfo* World::GetLodInfo(uint32_t meshIndex) const
	{
		if (meshIndex >= meshLodInfos.size())
			return nullptr;
		return &meshLodInfos[meshIndex];
	}

	// Resolve every world mesh's cooked "/lodN" chain once (called at the
	// end of ResolveAndFixup_clusters; hot paths below only index the
	// cache). Sibling URIs are composed into the frame arena and consumed
	// immediately; only resolved MeshLocations + errors are stored.
	// Shader LODs are NOT stamped here: the shading program is resolved
	// per (level, material) at draw time (see UseVertexLitShaderAt), so
	// materials program their own far-level shading via ShaderLodPolicy.
	void World::RebuildLodChains() {
		lodChains.clear();
		lodChains.resize(meshLocations.size());
		InvalidateLodCache();
		// A corrupt/missing AABB must not be advertised to GPU instance
		// culling as complete conservative bounds.
		const auto validBounds = [](const glm::vec3& mn, const glm::vec3& mx) {
			for (int a = 0; a < 3; ++a)
				if (!std::isfinite(mn[a]) || !std::isfinite(mx[a]) || mn[a] > mx[a])
					return false;
			return true;
		};
		for (uint32_t i = 0; i < (uint32_t)meshLocations.size(); ++i) {
			LodChain& chain = lodChains[i];
			const MeshLocation& lod0 = meshLocations[i];
			if (lod0.cluster == nullptr || !lod0.handle.valid())
				continue;
			LodChain::Level baseLevel{ lod0, 0.0f };
			if (const StaticMesh* baseMesh = lod0.cluster->staticMeshes.get(lod0.handle)) {
				baseLevel.boundsMin = baseMesh->geometry.aabbMin;
				baseLevel.boundsMax = baseMesh->geometry.aabbMax;
				baseLevel.boundsKnown = validBounds(baseLevel.boundsMin, baseLevel.boundsMax);
			}
			chain.levels.push_back(baseLevel);
			const StringViewT base =
				BaseMeshUriForLod(lod0.cluster->GetMeshUri(lod0.handle));
			if (base.empty())
				continue;
			for (uint32_t li = 1; li < 16; ++li) {
				const StringViewT uri = LodSiblingUri(base, li);
				bool found = false;
				float err = 0.0f;
				for (const auto& clusterPtr : clusters) {
					if (!clusterPtr)
						continue;
					if (auto hit = clusterPtr->GetMeshLod(uri)) {
						err = hit->simplifyError;
						found = true;
						break;
					}
				}
				if (!found)
					break;
				auto meshHit = ResolveMeshAcrossClusters(uri);
				if (!meshHit || meshHit->cluster == nullptr ||
					!meshHit->handle.valid()) {
					// Geometry streaming: metadata registered but no
					// pool mesh - a DEFERRED sibling. The old code broke
					// the chain here, which would strand every RESIDENT
					// level beyond the gap (LOD0 + pinned LOD3 with
					// LOD1/LOD2 deferred would leave LOD3 unreachable).
					// Append the deferred level (invalid mesh,
					// resident=false) and CONTINUE - the picker compacts
					// the resident subset, the demand pass sees the
					// full chain.
					LodChain::Level deferred{ MeshLocation{}, err, false };
					for (const auto& clusterPtr : clusters) {
						if (!clusterPtr) continue;
						if (const auto* record = clusterPtr->FindDeferredMeshLod(uri)) {
							deferred.boundsMin = record->src.aabbMin;
							deferred.boundsMax = record->src.aabbMax;
							deferred.boundsKnown = validBounds(deferred.boundsMin, deferred.boundsMax);
							break;
						}
					}
					chain.levels.push_back(deferred);
					chain.anyDeferred = true;
					continue;
				}
				LodChain::Level resident{ *meshHit, err, true };
				if (const StaticMesh* levelMesh = meshHit->cluster->staticMeshes.get(meshHit->handle)) {
					resident.boundsMin = levelMesh->geometry.aabbMin;
					resident.boundsMax = levelMesh->geometry.aabbMax;
					resident.boundsKnown = validBounds(resident.boundsMin, resident.boundsMax);
				}
				chain.levels.push_back(resident);
			}
		}
	}

	// ---------------------------------------------------------------------------
	// Per-node LOD decision cache (full rate by default, halfrate parity
	// opt-out; see world.h).
	// ---------------------------------------------------------------------------

	void World::InvalidateLodCache() {
		nodeLod_.clear();
		nodeLodStamp_.clear();
		nodeLodCountFrame_.clear();
		lodStatDraws_ = 0;
		lodStatSwitched_ = 0;
		lodTick_ = 0;
		lodLastFrameId_ = ~uint64_t(0);
	}

	uint32_t World::LodIndexForNode(NodeId node, uint32_t worldMesh,
		const LodFrameInputs& inputs) const {
		// Frame rollover: adopt this frame's inputs, tick the parity,
		// reset the per-frame decision stats (see the header block).
		if (inputs.frameId != lodLastFrameId_) {
			lodLastFrameId_ = inputs.frameId;
			lodInputs_ = inputs;
			++lodTick_;
			lodStatDraws_ = 0;
			lodStatSwitched_ = 0;
		}
		if (node < 0)
			return 0;
		const uint32_t n = node;
		if (n >= nodeLod_.size()) {
			nodeLod_.assign(scene.hierarchy.size(), 0);
			nodeLodStamp_.assign(scene.hierarchy.size(), 0);
		}
		if (n >= nodeLod_.size())
			return 0; // node outside the graph
		const bool computedThisFrame = nodeLodStamp_[n] == inputs.frameId;
		if (!computedThisFrame) {
			// Full rate (default): every node recomputes every frame.
			// Halfrate: parity split — half the nodes per frame.
			const bool refreshDue = lodFullRate
				|| (lodTick_ & 1) == (n & 1);
			const bool hasOld = nodeLodStamp_[n] != 0;
			if (refreshDue || !hasOld) {
				// Hysteresis anchors on the node's live level: the stale
				// cached value is the current state the band makes sticky.
				// The instance's own world scale turns the MESH-LOCAL
				// sibling errors into world errors at the pick (the
				// serialized errors are scale-free by design).
				const float instanceScale =
					(n < scene.globalTransform.size())
					? MaxScaleOfMatrix(scene.globalTransform[n]) : 1.0f;
				nodeLod_[n] = static_cast<uint8_t>(SelectLodIndex(worldMesh,
					LodDistanceForNode(node, worldMesh, lodInputs_.eye),
					lodInputs_.viewportHeight, lodInputs_.fovY,
					lodInputs_.pixelBudget, nodeLod_[n], instanceScale));
				nodeLodStamp_[n] = inputs.frameId;
				// Geometry streaming (the DESIRED half of the
				// desired/resident/drawn triad): over chains with
				// deferred siblings, run the SAME selection over the
				// FULL chain (deferred levels included) and record every
				// non-resident level the camera actually wants. This is
				// the streamer's entire demand signal - the sharpest
				// (lowest) miss per mesh wins, drained between frames by
				// ConsumeLodDemand. Chains without deferred siblings
				// skip this entirely (their hot path is unchanged).
				if (worldMesh < lodChains.size()
					&& lodChains[worldMesh].anyDeferred) {
					const LodChain& chain = lodChains[worldMesh];
					const uint32_t nl = (uint32_t)std::min<size_t>(
						chain.levels.size(), 16);
					if (nl > 1) {
						float allErrors[16];
						for (uint32_t l = 0; l < nl; ++l)
							allErrors[l] = chain.levels[l].error * instanceScale;
						const float dist =
							LodDistanceForNode(node, worldMesh, lodInputs_.eye);
						if (dist > 0.0f && lodInputs_.pixelBudget >= 0.0f) {
							const uint32_t desired =
								SelectLodIndexForDistance(allErrors, nl, dist,
									lodInputs_.viewportHeight, lodInputs_.fovY,
									lodInputs_.pixelBudget, nodeLod_[n],
									kLodSelectHysteresis);
							for (uint32_t l =
									(desired > kLodDemandPrefetchLevels
										? desired - kLodDemandPrefetchLevels : 0);
								l <= desired; ++l) {
								if (!chain.levels[l].resident) {
									auto it = lodDemand_.find(worldMesh);
									if (it == lodDemand_.end() || l < it->second)
										lodDemand_[worldMesh] = l;
									// keep walking the prefetch window:
									// the finest miss wins the entry
								}
								else if (lodTouchTracking_) {
									// RESIDENT but desired (typically the
									// promoted level the hysteresis band
									// has not switched the draws to yet):
									// TOUCH it - the eviction idle clock
									// must never expire a level the camera
									// still wants (the evict/re-promote
									// ping-pong otherwise cycles it every
									// idleFrames frames for nothing).
									lodDrawnSet_.insert(
										(uint64_t(worldMesh) << 32) | l);
								}
							}
						}
					}
				}
			}
		}
		// Per-frame decision stats: count the node ONCE per frame (the
		// first leg to ask - filter, prepass, shadow, direct, motion -
		// marks it; every later leg reads the cache and skips the count).
		// A separate mark from nodeLodStamp_: halfrate parity-skips do
		// NOT stamp the decision but still SERVE (and count) the stale
		// level this frame.
		if (nodeLodCountFrame_.size() != nodeLod_.size())
			nodeLodCountFrame_.assign(nodeLod_.size(), ~uint64_t(0));
		if (n < nodeLodCountFrame_.size()
			&& nodeLodCountFrame_[n] != inputs.frameId) {
			nodeLodCountFrame_[n] = inputs.frameId;
			++lodStatDraws_;
			if (nodeLod_[n] > 0)
				++lodStatSwitched_;
		}
		// Geometry streaming: record the SERVED level (computed or
		// cache-served) as a touch - the streamer's promote registry
		// only evicts what nothing drew AND nothing demanded within the
		// idle window. Packed mesh<<32|level; the set drains between
		// frames. Zero cost when streaming is off (tracking unarmed).
		if (lodTouchTracking_)
			lodDrawnSet_.insert(
				(uint64_t(worldMesh) << 32) | nodeLod_[n]);
		return nodeLod_[n];
	}

	float World::LodDistanceForNode(NodeId node, uint32_t worldMesh,
		const glm::vec3& eye) const {
		const auto originDistance = [&]() {
			if (node < 0 || (uint32_t)node >= scene.globalTransform.size())
				return 0.0f;
			return glm::length(
				glm::vec3(scene.globalTransform[(uint32_t)node][3]) - eye);
		};
		const StaticMesh* m = GetMesh(worldMesh);
		if (!m || node < 0 || (uint32_t)node >= scene.globalTransform.size())
			return originDistance();
		glm::vec3 c{ 0.0f };
		float r = 0.0f;
		MeshBoundsToWorld(scene.globalTransform[(uint32_t)node],
			m->geometry.aabbMin, m->geometry.aabbMax, c, r);
		if (!(r > 0.0f) || !std::isfinite(c.x + c.y + c.z)
			|| !std::isfinite(r))
			return originDistance();
		return std::max(0.0f, glm::length(c - eye) - r);
	}

	static void EnsureLodChainsCached(const World& w) {
		if (w.lodChains.size() != w.meshLocations.size())
			const_cast<World&>(w).RebuildLodChains();
	}

	uint32_t World::LodChainLength(uint32_t meshIndex) const {
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size())
			return 1;
		const size_t n = lodChains[meshIndex].levels.size();
		return n == 0 ? 1 : (uint32_t)n;
	}

	std::optional<World::MeshLocation> World::ResolveLodMesh(
		uint32_t meshIndex, uint32_t lodIndex) const {
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size())
			return std::nullopt;
		const LodChain& chain = lodChains[meshIndex];
		if (lodIndex >= chain.levels.size())
			return std::nullopt;
		const MeshLocation& loc = chain.levels[lodIndex].mesh;
		if (loc.cluster == nullptr || !loc.handle.valid())
			return std::nullopt;
		return loc;
	}

	StaticMesh* World::GetLodMesh(uint32_t meshIndex, uint32_t lodIndex) {
		auto hit = ResolveLodMesh(meshIndex, lodIndex);
		if (!hit || hit->cluster == nullptr || !hit->handle.valid())
			return GetMesh(meshIndex); // missing sibling: fall back to LOD0
		if (StaticMesh* m = hit->cluster->staticMeshes.get(hit->handle))
			return m;
		return GetMesh(meshIndex);
	}

	float World::LodErrorAt(uint32_t meshIndex, uint32_t lodIndex) const {
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size())
			return 0.0f;
		const LodChain& chain = lodChains[meshIndex];
		if (lodIndex >= chain.levels.size())
			return 0.0f;
		return chain.levels[lodIndex].error;
	}

	World::FarShading World::FarShadingAt(uint32_t meshIndex,
		uint32_t lodIndex, uint32_t materialIndex) const {
		EnsureLodChainsCached(*this);
		FarShading out{};
		if (meshIndex >= lodChains.size())
			return out;
		const LodChain& chain = lodChains[meshIndex];
		// Farthest level of a cooked chain only (length > 1); every other
		// level and out-of-range probes resolve default (full-lit).
		if (chain.levels.size() <= 1
			|| lodIndex >= chain.levels.size()
			|| lodIndex + 1 < chain.levels.size())
			return out;
		// Invalid materials resolve default (their draws are skipped
		// elsewhere, and default keeps every consumer on the base path).
		const Material* mat = GetMaterial(materialIndex);
		if (!mat)
			return out;
		// ProjectLit materials stay FULL-LIT at every level (design §9
		// "LOD"): both far-shading mechanisms shade through STOCK
		// modules — the cheap vertex-lit FS never executes the lit body
		// (the hooks would silently vanish at distance), and the
		// quality-specialized twins are the standard family's programs
		// (the family's own spec twins do not exist). Default keeps the
		// far level on the family's lit program (the direct pick
		// consults the family), so the effect survives to the farthest
		// LOD — the documented v1 cost.
		if (ShaderFamilyOf(materialIndex)
				== smvk::MaterialShaderFamily::ProjectLit)
			return out;
		if (mat->shaderLod.allowFarVertexLit) {
			out.vertexLit = true;
			return out;
		}
		out.quality = mat->shaderLod.farQuality;
		return out;
	}

	uint32_t World::SelectLodIndex(uint32_t meshIndex, float distance,
		float viewportHeightPx, float fovYRadians, float maxPixelError,
		uint32_t currentLod, float instanceWorldScale) const {
		if (!(distance > 0.0f) || !(maxPixelError >= 0.0f))
			return 0;
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size())
			return 0;
		const LodChain& chain = lodChains[meshIndex];
		if (chain.levels.size() <= 1)
			return 0;
		// SelectLodIndexForDistance walks a plain error array; the levels
		// are SoA-flavored (mesh/error/shader), so stage the errors on the
		// stack (chains are capped at 16 levels by RebuildLodChains). The
		// serialized errors are MESH-LOCAL (scale-free), so scale by the
		// INSTANCE's world scale here - per-instance exact, and immune to
		// the cross-run compounding a cook-time scaling pass had (a 0.008
		// root shrank the error 125x per re-cook and the picker served the
		// coarsest LOD centimeters from the camera).
		const float scale = (instanceWorldScale > 0.0f
			&& std::isfinite(instanceWorldScale)) ? instanceWorldScale : 1.0f;
		// Geometry streaming: select over the RESIDENT subset only - a
		// deferred middle sibling degrades to whichever resident
		// neighbor is in error budget (the DRAWN level of the
		// desired/resident/drawn triad; a deferred level can never be
		// selected, so every submitted draw references resident geometry
		// by construction). levelIdx maps the compacted slot back to the
		// true level index, and the hysteresis anchor (currentLod, a
		// true index - the drawn level is always resident) maps forward
		// to its compacted slot.
		float errors[16];
		uint32_t levelIdx[16];
		uint32_t nr = 0;
		uint32_t anchorSlot = 0;
		const uint32_t n = (uint32_t)std::min<size_t>(chain.levels.size(), 16);
		for (uint32_t i = 0; i < n; ++i) {
			if (!chain.levels[i].resident)
				continue;
			errors[nr] = chain.levels[i].error * scale;
			levelIdx[nr] = i;
			if (i == currentLod)
				anchorSlot = nr;
			++nr;
		}
		if (nr <= 1)
			return 0;
		return levelIdx[SelectLodIndexForDistance(errors, nr, distance,
			viewportHeightPx, fovYRadians, maxPixelError, anchorSlot,
			kLodSelectHysteresis)];
	}

	// ---------------------------------------------------------------------------
	// Geometry LOD streaming (Renderer/geometry_streamer.h): the demand
	// drain + the promote orchestration. See the world.h block.
	// ---------------------------------------------------------------------------

	void World::ConsumeLodDemand(
		std::vector<std::pair<uint32_t, uint32_t>>& out) {
		out.clear();
		out.reserve(lodDemand_.size());
		for (const auto& [mesh, level] : lodDemand_)
			out.emplace_back(mesh, level);
		lodDemand_.clear();
	}

	void World::ConsumeLodDrawn(
		std::vector<std::pair<uint32_t, uint32_t>>& out) {
		out.clear();
		out.reserve(lodDrawnSet_.size());
		for (const uint64_t k : lodDrawnSet_)
			out.emplace_back(uint32_t(k >> 32), uint32_t(k & 0xFFFFFFFFu));
		lodDrawnSet_.clear();
	}

	bool World::HasDeferredLods() const {
		for (const auto& clusterPtr : clusters) {
			if (clusterPtr && clusterPtr->DeferredLodCount() > 0)
				return true;
		}
		return false;
	}

	bool World::HasLodRecords() const {
		for (const auto& clusterPtr : clusters) {
			if (clusterPtr && clusterPtr->HasLodRecords())
				return true;
		}
		return false;
	}

	std::optional<Cluster::PromotedLod> World::PromoteLodLevel(
		uint32_t meshIndex, uint32_t lodIndex) {
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size() || meshIndex >= meshLocations.size())
			return std::nullopt;
		const LodChain& chain = lodChains[meshIndex];
		if (lodIndex >= chain.levels.size() || chain.levels[lodIndex].resident)
			return std::nullopt; // not deferred (or already promoted)
		// The sibling's uri derives from the LOD0 base exactly like
		// RebuildLodChains does (the "<base>/lodN" convention).
		const MeshLocation& lod0 = meshLocations[meshIndex];
		if (lod0.cluster == nullptr || !lod0.handle.valid())
			return std::nullopt;
		const StringViewT base =
			BaseMeshUriForLod(lod0.cluster->GetMeshUri(lod0.handle));
		if (base.empty())
			return std::nullopt;
		const StringViewT uri = LodSiblingUri(base, lodIndex);
		// STAGE 6 (the pager lane): a host-installed LOD page source
		// supplies the sibling's ENTRY BYTES (a pager page) — the
		// pinned whole image is then NOT needed for the promote. No
		// source / no page = the classic image-pin lane (unchanged).
		if (LodPageSourceRef()) {
			char uriBuf[1024];
			const size_t uriLen = uri.size() < sizeof(uriBuf) - 1
				? uri.size() : sizeof(uriBuf) - 1;
			std::memcpy(uriBuf, uri.data(), uriLen);
			uriBuf[uriLen] = '\0';
			std::optional<LodPagePin> pin =
				LodPageSourceRef()(uriBuf);
			if (pin && pin->data && pin->size) {
				std::optional<Cluster::PromotedLod> r;
				for (const auto& clusterPtr : clusters) {
					if (!clusterPtr)
						continue;
					r = clusterPtr->PromoteDeferredMeshLodFromPage(
						uriBuf, pin->data, pin->size);
					if (r)
						break;
				}
				if (pin->release)
					pin->release(); // after the synchronous upload
				if (r)
					return r;
			}
		}
		// Any cluster may hold the deferred record (the overlay rule) -
		// walk in registration order like ResolveMeshAcrossClusters.
		for (const auto& clusterPtr : clusters) {
			if (!clusterPtr)
				continue;
			if (auto r = clusterPtr->PromoteDeferredMeshLod(uri))
				return r;
		}
		return std::nullopt;
	}

	uint64_t World::EvictLodLevel(uint32_t meshIndex, uint32_t lodIndex) {
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size() || meshIndex >= meshLocations.size())
			return 0;
		const LodChain& chain = lodChains[meshIndex];
		// Only PROMOTED siblings evict (never the LOD0 base - it is the
		// scene's referenced mesh and the chain's level 0; the pinned
		// coarsest fallback has no streaming record at all, so it is
		// structurally safe too).
		if (lodIndex == 0 || lodIndex >= chain.levels.size()
			|| !chain.levels[lodIndex].resident)
			return 0;
		const MeshLocation& lod0 = meshLocations[meshIndex];
		if (lod0.cluster == nullptr || !lod0.handle.valid())
			return 0;
		const StringViewT base =
			BaseMeshUriForLod(lod0.cluster->GetMeshUri(lod0.handle));
		if (base.empty())
			return 0;
		const StringViewT uri = LodSiblingUri(base, lodIndex);
		for (const auto& clusterPtr : clusters) {
			if (!clusterPtr)
				continue;
			if (const uint64_t bytes = clusterPtr->EvictMeshLod(uri))
				return bytes;
		}
		return 0;
	}

	// Scene-wide lightmap chart transform table: every StaticMesh's
	// lmChartXforms concatenated in first-seen order over meshLocations
	// (LOD0) then LOD siblings, so the layout is sorted by world mesh
	// index, then LOD level, then chart index. Meshes without a lightmap
	// contribute nothing (their draws gate on lmChunk anyway).
	void World::RebuildLightmapChartTable() {
		lmChartTable.clear();
		lmChartBase_.clear();
		auto appendMesh = [&](const MeshLocation& loc) {
			if (loc.cluster == nullptr || !loc.handle.valid())
				return;
			const LmChartKey key{ loc.cluster, loc.handle.index() };
			if (lmChartBase_.contains(key))
				return; // already appended (LOD0 == chain level 0)
			const StaticMesh* m = loc.cluster->staticMeshes.get(loc.handle);
			if (!m || m->lmChartXforms.empty()
				|| m->lmChunk == 0xFFFFFFFFu)
				return;
			lmChartBase_.emplace(key,
				static_cast<uint32_t>(lmChartTable.size()));
			lmChartTable.insert(lmChartTable.end(),
				m->lmChartXforms.begin(), m->lmChartXforms.end());
		};
		for (uint32_t i = 0; i < (uint32_t)meshLocations.size(); ++i)
			appendMesh(meshLocations[i]);
		for (uint32_t i = 0; i < (uint32_t)lodChains.size(); ++i) {
			for (const LodChain::Level& lv : lodChains[i].levels)
				appendMesh(lv.mesh);
		}
		// A per-World counter restarts at 1 on every scene load. Reusing it
		// as a GPU cache key can retain the previous world's chart transforms.
		// Assign process-unique revisions on every rebuild instead.
		static std::atomic<uint64_t> nextChartRevision{0};
		lmChartVersion = nextChartRevision.fetch_add(1,
			std::memory_order_relaxed) + 1;
	}

	uint32_t World::ChartOffsetFor(MeshLocation loc) const {
		if (loc.cluster == nullptr || !loc.handle.valid())
			return 0;
		const auto it = lmChartBase_.find(
			LmChartKey{ loc.cluster, loc.handle.index() });
		return it != lmChartBase_.end() ? it->second : 0;
	}

	uint32_t World::ChartOffsetFor(uint32_t meshIndex, uint32_t lodIndex) const {
		EnsureLodChainsCached(*this);
		if (meshIndex < lodChains.size()
			&& lodIndex < lodChains[meshIndex].levels.size())
			return ChartOffsetFor(lodChains[meshIndex].levels[lodIndex].mesh);
		if (meshIndex < meshLocations.size())
			return ChartOffsetFor(meshLocations[meshIndex]);
		return 0;
	}

	std::optional<World::MeshLocation> World::ResolveMeshAcrossClusters(
		StringViewT uri) const
	{
		for (const auto& clusterPtr : clusters) {
			if (!clusterPtr)
				continue;
			if (auto hit = clusterPtr->FindMeshByUri(uri))
				return MeshLocation{ .cluster = clusterPtr.get(), .handle = *hit };
		}
		return std::nullopt;
	}

	std::optional<World::MaterialLocation> World::ResolveMaterialAcrossClusters(
		StringViewT uri) const
	{
		for (const auto& clusterPtr : clusters) {
			if (!clusterPtr)
				continue;
			if (auto hit = clusterPtr->FindMaterialByUri(uri))
				return MaterialLocation{ .cluster = clusterPtr.get(), .handle = *hit };
		}
		return std::nullopt;
	}

	// ---------------------------------------------------------------------------
	// Static BVH for CPU frustum culling (see world.h).
	// ---------------------------------------------------------------------------

	void World::InvalidateStaticBvh() {
		staticBvhValid_ = false;
		staticBvh_.nodes.clear();
		staticBvh_.primIndices.clear();
		staticBvh_.primToNode.clear();
	}

	namespace {
		// World-space AABB of a mesh under `model` (8-corner transform so
		// rotation/scale stay conservative).
		::tour::Aabb WorldAabbForMesh(const StaticMesh* mesh, const glm::mat4& model) {
			::tour::Aabb out = ::tour::Aabb::empty();
			if (!mesh)
				return out;
			const glm::vec3 localMin = mesh->geometry.aabbMin;
			const glm::vec3 localMax = mesh->geometry.aabbMax;
			const glm::vec3 corners[8] = {
				{ localMin.x, localMin.y, localMin.z },
				{ localMin.x, localMin.y, localMax.z },
				{ localMin.x, localMax.y, localMin.z },
				{ localMin.x, localMax.y, localMax.z },
				{ localMax.x, localMin.y, localMin.z },
				{ localMax.x, localMin.y, localMax.z },
				{ localMax.x, localMax.y, localMin.z },
				{ localMax.x, localMax.y, localMax.z },
			};
			for (const auto& c : corners) {
				glm::vec4 wp4 = model * glm::vec4(c, 1.0f);
				out.expand(glm::vec3(wp4));
			}
			return out;
		}
	} // namespace

	void World::RebuildStaticBvh() {
		InvalidateStaticBvh();
		EnsureLodChainsCached(*this);
		std::vector<Partition> prims;
		prims.reserve(scene.tables.meshes.size());
		for (const MeshRow& row : scene.tables.meshes) {
			const NodeId node = row.node;
			if (!IsStaticNode(scene, (int)node))
				continue;
			if (row.mesh >= meshLocations.size())
				continue;
			StaticMesh* base = GetMesh(row.mesh);
			if (!base)
				continue;
			const glm::mat4& model = scene.globalTransform[node];
			::tour::Aabb bounds = WorldAabbForMesh(base, model);
			// LOD-chain union: any LodIndexForNode selection stays inside.
			if (row.mesh < lodChains.size()) {
				const LodChain& chain = lodChains[row.mesh];
				for (size_t li = 1; li < chain.levels.size(); ++li) {
					const MeshLocation& loc = chain.levels[li].mesh;
					if (!loc.cluster || !loc.handle.valid())
						continue;
					if (const StaticMesh* m = loc.cluster->staticMeshes.get(loc.handle))
						bounds.expand(WorldAabbForMesh(m, model));
				}
			}
			const glm::vec3 c = bounds.centroid();
			prims.push_back(Partition{ bounds, c, node });
		}
		const uint32_t N = static_cast<uint32_t>(prims.size());
		staticBvh_.primIndices.resize(N);
		for (uint32_t i = 0; i < N; ++i)
			staticBvh_.primIndices[i] = i;
		staticBvh_.nodes.reserve(N * 2);
		staticBvh_.primToNode.clear();
		if (N > 0)
			buildRecursive(staticBvh_, prims, 0, N, /*maxLeafSize=*/4);
		staticBvh_.primToNode.resize(N);
		for (uint32_t slot = 0; slot < N; ++slot) {
			const uint32_t primId = staticBvh_.primIndices[slot];
			staticBvh_.primToNode[slot] = prims[primId].node;
		}
		staticBvhValid_ = true;
	}

	::tour::Aabb World::ComputeSceneBounds() const {
		::tour::Aabb out = ::tour::Aabb::empty();
		for (const MeshRow& row : scene.tables.meshes) {
			if (row.mesh >= meshLocations.size())
				continue;
			const MeshLocation& loc = meshLocations[row.mesh];
			if (loc.cluster == nullptr || !loc.handle.valid())
				continue;
			const StaticMesh* base = loc.cluster->staticMeshes.get(loc.handle);
			if (!base)
				continue;
			if (row.node >= scene.globalTransform.size())
				continue;
			out.expand(WorldAabbForMesh(base, scene.globalTransform[row.node]));
		}
		return out;
	}

	const ::tour::BvhTree& World::EnsureStaticBvh() const {
		if (!staticBvhValid_)
			const_cast<World*>(this)->RebuildStaticBvh();
		return staticBvh_;
	}

	std::vector<uint8_t> World::StaticVisibilityMask(
		const glm::vec4* planes, int planeCount) const {
		const ::tour::BvhTree& tree = EnsureStaticBvh();
		if (tree.nodes.empty() || tree.primIndices.empty() || !planes || planeCount <= 0)
			return {};
		std::vector<int> slots;
		slots.reserve(tree.primIndices.size());
		::tour::frustumCullBVH(tree, 0, planes, planeCount, slots);
		std::vector<uint8_t> mask(scene.hierarchy.size(), 0);
		// Dynamic (non-static) nodes pass through: the caller sphere-tests
		// them per item. Static nodes default to culled; visible ones are
		// stamped below.
		for (size_t i = 0; i < mask.size(); ++i) {
			if (!IsStaticNode(scene, (int)i))
				mask[i] = 1;
		}
		for (int slot : slots) {
			if (slot < 0 || (size_t)slot >= tree.primToNode.size())
				continue;
			const uint32_t node = tree.primToNode[(size_t)slot];
			if (node < mask.size())
				mask[node] = 1;
		}
		return mask;
	}

	// ---------------------------------------------------------------------------
	// SIMD flag-list cooking (see world.h flagLists).
	// ---------------------------------------------------------------------------

	namespace {

		// One-time AVX2 probe (leaf 7, EBX[5]; OSXSAVE required for the
		// OS to preserve the wide registers across context switches).
		// Explicit intrinsics compile on MSVC without /arch:AVX2, so the
		// probe gates the code path at runtime instead.
		bool CpuHasAvx2()
		{
			static const bool has = [] {
				unsigned regs[4];
#ifdef _WIN32
				__cpuid(reinterpret_cast<int*>(regs), 0);
				if (regs[0] < 7)
					return false;
				__cpuid(reinterpret_cast<int*>(regs), 1);
				const bool osxsave = (regs[2] & (1u << 27)) != 0;
				if (!osxsave)
					return false;
				__cpuidex(reinterpret_cast<int*>(regs), 7, 0);
				return (regs[1] & (1u << 5)) != 0;
#else
				// GCC: __get_cpuid(count, &a,&b,&c,&d); returns 0 for
				// unsupported leaves (covers the regs[0] < 7 guard).
				if (!__get_cpuid_count(7, 0, &regs[0], &regs[1], &regs[2], &regs[3]))
					return false;
				unsigned features[4];
				if (!__get_cpuid(1, &features[0], &features[1], &features[2], &features[3]))
					return false;
				const bool osxsave = (features[2] & (1u << 27)) != 0;
				if (!osxsave)
					return false;
				return (regs[1] & (1u << 5)) != 0;
#endif
			}();
			return has;
		}

		// GCC/Clang: the 8-wide AVX2 scan lives in one function with the
		// avx2 target attribute enabled (the runtime probe above gates the
		// call). MSVC compiles the explicit intrinsics without
		// /arch:AVX2, so the split helper stays non-Windows-only.
#ifndef _WIN32
		__attribute__((target("avx2")))
		void Avx2ItemIndices(std::vector<uint32_t>& indices,
			const std::vector<IteratorFlags>& itemFlags, size_t n,
			IteratorFlags requiredFlags)
		{
			const __m256i req = _mm256_set1_epi32(static_cast<int>(requiredFlags));
			size_t i = 0;
			for (; i + 8 <= n; i += 8) {
				const __m256i f = _mm256_loadu_si256(
					reinterpret_cast<const __m256i*>(&itemFlags[i]));
				const __m256i pass = _mm256_cmpeq_epi32(
					_mm256_and_si256(f, req), req);
				uint32_t mask = static_cast<uint32_t>(
					_mm256_movemask_ps(_mm256_castsi256_ps(pass)));
				while (mask != 0) {
					const uint32_t bit = std::countr_zero(mask);
					indices.push_back(static_cast<uint32_t>(i + bit));
					mask &= mask - 1;
				}
			}
			for (; i < n; ++i) {
				if ((itemFlags[i] & requiredFlags) == requiredFlags)
					indices.push_back(static_cast<uint32_t>(i));
			}
		}
#endif

	} // namespace

	const std::vector<uint32_t>& World::ItemIndicesFor(IteratorFlags requiredFlags) const
	{
		if (auto it = flagLists.find(requiredFlags); it != flagLists.end())
			return it->second;

		std::vector<uint32_t>& indices = flagLists[requiredFlags];
		const size_t n = itemFlags.size();
		indices.reserve(n);

		if (CpuHasAvx2()) {
			// 8-wide scan of the dense flags column: AND + CMPEQ against
			// the broadcast mask, movemask compresses the lane results
			// into a bitmask, then bit-scan emits only the passing
			// indices. Matches the scalar test exactly: all required bits
			// set; extra bits ignored.
			// GCC/Clang: the intrinsics need the avx2 target enabled for
			// this one function (the probe above gates it at runtime);
			// MSVC accepts the explicit intrinsics without /arch:AVX2.
#ifdef _WIN32
			const __m256i req = _mm256_set1_epi32(static_cast<int>(requiredFlags));
			size_t i = 0;
			for (; i + 8 <= n; i += 8) {
				const __m256i f = _mm256_loadu_si256(
					reinterpret_cast<const __m256i*>(&itemFlags[i]));
				const __m256i pass = _mm256_cmpeq_epi32(
					_mm256_and_si256(f, req), req);
				uint32_t mask = static_cast<uint32_t>(
					_mm256_movemask_ps(_mm256_castsi256_ps(pass)));
				while (mask != 0) {
					const uint32_t bit = std::countr_zero(mask);
					indices.push_back(static_cast<uint32_t>(i + bit));
					mask &= mask - 1;
				}
			}
			for (; i < n; ++i) {
				if ((itemFlags[i] & requiredFlags) == requiredFlags)
					indices.push_back(static_cast<uint32_t>(i));
			}
#else
			Avx2ItemIndices(indices, itemFlags, n, requiredFlags);
#endif
		}
		else {
			for (size_t i = 0; i < n; ++i) {
				if ((itemFlags[i] & requiredFlags) == requiredFlags)
					indices.push_back(static_cast<uint32_t>(i));
			}
		}
		return indices;
	}

} // namespace smvk
