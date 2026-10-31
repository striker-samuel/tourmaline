// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/App/world.h>

#include <cassert>
#include <iostream>
#include <stdexcept>

#include <algorithm>
#include <bit>
#include <cmath>
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
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>

namespace smvk {

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
		uint32_t sceneIndex, TextureStreamer* streamer,
		std::shared_ptr<const std::vector<uint8_t>> imagePin,
		const ExternalTextureResolver* externalTexture)
	{
		std::string err;
		auto cluster = DeserializeCluster(ctx, reader, &err, sceneIndex,
			streamer, std::move(imagePin), externalTexture);
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
		if (blobs.size() != 1 + size_t(nH) * size_t(nH) + size_t(nS) * size_t(nS)) {
			err = "Terrain blob count mismatch (want 1+16+256)";
			return false;
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
		dyn.scene.localTransform[size_t(cnode)] = frame;
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
		scene.localTransform[size_t(mnode)] = frame;
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
		// The dynamic cluster is runtime-created state, never pak
		// content: unloading it would drop every runtime entity — refuse.
		if (cluster.isDynamic)
			return;
		for (auto it = clusters.begin(); it != clusters.end(); ++it) {
			if (it->get() == &cluster) {
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

	void World::RegisterCluster(std::unique_ptr<Cluster> cluster)
	{
		assert(cluster != nullptr);
		cluster->world = this;
		clusters.push_back(std::move(cluster));
	}

	std::unique_ptr<Cluster> World::UnregisterCluster(Cluster& cluster)
	{
		// Same refusal as UnloadCluster: the dynamic cluster is runtime
		// state and stays with its world.
		if (cluster.isDynamic)
			return nullptr;
		for (auto it = clusters.begin(); it != clusters.end(); ++it) {
			if (it->get() == &cluster) {
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
		return nullptr;
	}

	// Fold every registered cluster's Scene into the single World scene graph.
	// Each imported Scene keeps its resources in its owning Cluster's pools;
	// we only flatten the hierarchy/transforms/node->resource index maps here,
	// and remap the node->mesh/material indices to world-level resolution
	// slots that point back to the owning Cluster + handle.
	void World::ResolveAndFixup_clusters()
	{
		scene = Scene{};
		meshLocations.clear();
		materialLocations.clear();
		textureLocations.clear();
		lightLocations.clear();
		cameraLocations.clear();
		decalLocations.clear();
		meshLodInfos.clear();
		flags.clear();
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

		// World root node (parent = -1)
		const int root = addNode(scene, -1, 0);
		scene.localTransform[root] = glm::mat4(1.0f);
		scene.globalTransform[root] = glm::mat4(1.0f);
		scene.staticRoot.assign(scene.hierarchy.size(), 0);
		scene.staticNode.assign(scene.hierarchy.size(), 0);
		scene.noLightmap.assign(scene.hierarchy.size(), 0);
		scene.lightmap.assign(scene.hierarchy.size(), 0);
		scene.castsVolumetricShadow.assign(scene.hierarchy.size(), 0);
		scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
		scene.hidden.assign(scene.hierarchy.size(), 0);

		for (auto& clusterPtr : clusters) {
			Cluster& cluster = *clusterPtr.get();
			Scene& cs = cluster.scene;

			const uint32_t nameOffset = static_cast<uint32_t>(scene.nodeNames.size());

			// Slot-association map (Epoch 6b): cluster slot -> world
			// NodeId, UINT32_MAX for holes. The dense-era +nodeOffset
			// arithmetic became this explicit map — numerically
			// identical for a dense cluster scene (fresh imports/loads
			// allocate slots 0,1,2,...), correct when the cluster scene
			// carries holes (a cook-time static-merge retirement).
			const uint32_t clusterSlotCount =
				static_cast<uint32_t>(cs.hierarchy.size());
			std::vector<NodeId> nodeMap(clusterSlotCount, UINT32_MAX);

			// Pass 1: allocate a world slot per LIVE cluster node and
			// copy its payload (level as-is; links stay default until
			// pass 2). addNode appends sequentially on the freshly
			// wiped scene, so world ids match the old dense layout
			// exactly until the world scene itself grows holes.
			for (uint32_t s = 0; s < clusterSlotCount; ++s) {
				if (!IsLiveSlot(cs, s))
					continue;
				const int dst = addNode(scene, -1, cs.hierarchy[s].level);
				assert(dst >= 0);
				nodeMap[s] = static_cast<NodeId>(dst);
				scene.localTransform[size_t(dst)] = cs.localTransform[s];
				scene.globalTransform[size_t(dst)] = cs.globalTransform[s];
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
			for (uint32_t s = 0; s < clusterSlotCount; ++s) {
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
			}

			// Remap node metadata (names index into nodeNames after the
			// bulk pool append; uuids ride unchanged) per live slot.
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
						cs.metadata[s].name + nameOffset);
				SetNodeUuidRaw(scene, nodeMap[s], cs.metadata[s].uuid);
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

		RebuildStaticCache(scene);
		// Full-recalc marking at the merge boundary: root-level nodes
		// (the cooker's WorldSun is addNode(scene,-1,1)) fall outside
		// node 0's subtree - without this their world globals keep the
		// identity default and the sun decodes horizontal (az=0/el=0).
		MarkAllRootsChanged(scene);
		recalculateGlobalTransforms(scene);

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
			drawItems.push_back(RenderItem{
				.node = node,
				.mesh = row.mesh,
				.material = matRow ? matRow->material : kInvalidIndex,
				.flags = flags[node],
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
		for (uint32_t i = 0; i < (uint32_t)meshLocations.size(); ++i) {
			LodChain& chain = lodChains[i];
			const MeshLocation& lod0 = meshLocations[i];
			if (lod0.cluster == nullptr || !lod0.handle.valid())
				continue;
			chain.levels.push_back(LodChain::Level{ lod0, 0.0f });
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
					chain.levels.push_back(LodChain::Level{ MeshLocation{}, err, false });
					chain.anyDeferred = true;
					continue;
				}
				chain.levels.push_back(LodChain::Level{ *meshHit, err, true });
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
		++lmChartVersion;
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
		::Aabb WorldAabbForMesh(const StaticMesh* mesh, const glm::mat4& model) {
			::Aabb out = ::Aabb::empty();
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
		std::vector<::Partition> prims;
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
			::Aabb bounds = WorldAabbForMesh(base, model);
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
			prims.push_back(::Partition{ bounds, c, node });
		}
		const uint32_t N = static_cast<uint32_t>(prims.size());
		staticBvh_.primIndices.resize(N);
		for (uint32_t i = 0; i < N; ++i)
			staticBvh_.primIndices[i] = i;
		staticBvh_.nodes.reserve(N * 2);
		staticBvh_.primToNode.clear();
		if (N > 0)
			::buildRecursive(staticBvh_, prims, 0, N, /*maxLeafSize=*/4);
		staticBvh_.primToNode.resize(N);
		for (uint32_t slot = 0; slot < N; ++slot) {
			const uint32_t primId = staticBvh_.primIndices[slot];
			staticBvh_.primToNode[slot] = prims[primId].node;
		}
		staticBvhValid_ = true;
	}

	::Aabb World::ComputeSceneBounds() const {
		::Aabb out = ::Aabb::empty();
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

	const ::BvhTree& World::EnsureStaticBvh() const {
		if (!staticBvhValid_)
			const_cast<World*>(this)->RebuildStaticBvh();
		return staticBvh_;
	}

	std::vector<uint8_t> World::StaticVisibilityMask(
		const glm::vec4* planes, int planeCount) const {
		const ::BvhTree& tree = EnsureStaticBvh();
		if (tree.nodes.empty() || tree.primIndices.empty() || !planes || planeCount <= 0)
			return {};
		std::vector<int> slots;
		slots.reserve(tree.primIndices.size());
		::frustumCullBVH(tree, 0, planes, planeCount, slots);
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
