#include <Engine/App/world.h>

#include <cassert>
#include <iostream>
#include <stdexcept>

#include <algorithm>
#include <bit>
#include <cmath>
#include <immintrin.h>
#include <intrin.h>

#include <Engine/Scene/scene.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>

namespace smvk {

	// Cluster ctor lives here so the App module owns the Cluster lifecycle
	// (its Scene + resource pools). The mesh upload/destroy methods and the
	// destructor stay in mesh.cpp; this constructor just wires the resource
	// context and the back-pointer to the owning World.
	Cluster::Cluster(ResourceManager* ctx)
		: ctx_(ctx)
	{
	}

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
		std::shared_ptr<const std::vector<uint8_t>> imagePin)
	{
		std::string err;
		auto cluster = DeserializeCluster(ctx, reader, &err, sceneIndex,
			streamer, std::move(imagePin));
		if (!cluster)
			throw std::runtime_error("LoadClusterFromPak failed: " + err);
		Cluster& ref = *cluster;
		RegisterCluster(std::move(cluster));
		return ref;
	}

	void World::UnloadCluster(Cluster& cluster)
	{
		for (auto it = clusters.begin(); it != clusters.end(); ++it) {
			if (it->get() == &cluster) {
				clusters.erase(it);
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
		for (auto it = clusters.begin(); it != clusters.end(); ++it) {
			if (it->get() == &cluster) {
				std::unique_ptr<Cluster> detached = std::move(*it);
				clusters.erase(it);
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
	void World::ResolveAndFixup_clusters()
	{
		scene = Scene{};
		meshLocations.clear();
		materialLocations.clear();
		textureLocations.clear();
		lightLocations.clear();
		cameraLocations.clear();
		meshLodInfos.clear();
		flags.clear();
		// World material dedup (see the materialForNode remap below).
		std::unordered_map<uint64_t, uint32_t> worldMatByClusterMat;
		uint32_t clusterOrdinal = 0;
		// Authored-environment first-wins (see the per-cluster block):
		// the first cluster scene carrying an authored look wins the
		// world scene's resolved/default blocks.
		bool envTaken = false;

		// World root node (parent = -1)
		const int root = addNode(scene, -1, 0);
		scene.localTransform[root] = glm::mat4(1.0f);
		scene.globalTransform[root] = glm::mat4(1.0f);
		scene.staticRoot.assign(scene.hierarchy.size(), 0);
		scene.staticNode.assign(scene.hierarchy.size(), 0);
		scene.noLightmap.assign(scene.hierarchy.size(), 0);
		scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
		scene.hidden.assign(scene.hierarchy.size(), 0);

		for (auto& clusterPtr : clusters) {
			Cluster& cluster = *clusterPtr.get();
			Scene& cs = cluster.scene;

			const uint32_t nodeOffset = static_cast<uint32_t>(scene.hierarchy.size());
			const uint32_t nameOffset = static_cast<uint32_t>(scene.nodeNames.size());

			// Append the cluster's hierarchy, transforms and names.
			scene.hierarchy.insert(
				scene.hierarchy.end(),
				cs.hierarchy.begin(),
				cs.hierarchy.end()
			);
			scene.localTransform.insert(
				scene.localTransform.end(),
				cs.localTransform.begin(),
				cs.localTransform.end()
			);
			scene.globalTransform.insert(
				scene.globalTransform.end(),
				cs.globalTransform.begin(),
				cs.globalTransform.end()
			);
		scene.nodeNames.insert(
			scene.nodeNames.end(),
			cs.nodeNames.begin(),
			cs.nodeNames.end()
		);
		const uint32_t nodeCount =
			static_cast<uint32_t>(cs.hierarchy.size());
		// Carry the static-subtree spec (missing == all dynamic).
		if (cs.staticRoot.size() == cs.hierarchy.size()) {
			scene.staticRoot.insert(
				scene.staticRoot.end(),
				cs.staticRoot.begin(),
				cs.staticRoot.end()
			);
		}
		else {
			scene.staticRoot.insert(
				scene.staticRoot.end(), nodeCount, 0);
		}
		// Carry the lightmap-exclusion flags (missing == bake everything).
		if (cs.noLightmap.size() == cs.hierarchy.size()) {
			scene.noLightmap.insert(
				scene.noLightmap.end(),
				cs.noLightmap.begin(),
				cs.noLightmap.end()
			);
		}
		else {
			scene.noLightmap.insert(
				scene.noLightmap.end(), nodeCount, 0);
		}
		// Carry the dither-on-closeup flags (missing == off).
		if (cs.ditherCloseup.size() == cs.hierarchy.size()) {
			scene.ditherCloseup.insert(
				scene.ditherCloseup.end(),
				cs.ditherCloseup.begin(),
				cs.ditherCloseup.end()
			);
		}
		else {
			scene.ditherCloseup.insert(
				scene.ditherCloseup.end(), nodeCount, 0);
		}
		// Carry visibility (hidden) flags (missing == all visible).
		if (cs.hidden.size() == cs.hierarchy.size()) {
			scene.hidden.insert(
				scene.hidden.end(),
				cs.hidden.begin(),
				cs.hidden.end()
			);
		}
		else {
			scene.hidden.insert(
				scene.hidden.end(), nodeCount, 0);
		}
		scene.staticNode.insert(scene.staticNode.end(), nodeCount, 0);

			// Shift parent/firstChild/nextSibling/lastSibling indices so they
			// point into the world node array. The cluster root's parent (which
			// was -1) gets reparented under the world root.
			for (uint32_t i = 0; i < nodeCount; ++i) {
				Hierarchy& h = scene.hierarchy[nodeOffset + i];
				if (h.parent > -1)
					h.parent = h.parent + nodeOffset;
				if (h.firstChild > -1)
					h.firstChild = h.firstChild + nodeOffset;
				if (h.nextSibling > -1)
					h.nextSibling = h.nextSibling + nodeOffset;
				if (h.lastSibling > -1)
					h.lastSibling = h.lastSibling + nodeOffset;
			}

			// Reparent the cluster root under the world root.
			const uint32_t clusterRootNode = nodeOffset;
			scene.hierarchy[clusterRootNode].parent = root;
			if (scene.hierarchy[root].firstChild == -1) {
				scene.hierarchy[root].firstChild = clusterRootNode;
			}
			else {
				const uint32_t prevFirst = scene.hierarchy[root].firstChild;
				scene.hierarchy[clusterRootNode].nextSibling = prevFirst;
				scene.hierarchy[root].firstChild = clusterRootNode;
			}
			scene.hierarchy[clusterRootNode].level += 1;

			// Remap nameForNode indices (they index into nodeNames).
			for (auto& [node, nameIdx] : cs.nameForNode) {
				scene.nameForNode[node + nodeOffset] = nameIdx + nameOffset;
			}

		// Remap meshForNode: cluster-local handle index -> world mesh index.
		for (auto& [node, meshIdx] : cs.meshForNode) {
			const uint32_t worldNode = node + nodeOffset;
			const uint32_t worldMeshIdx =
				static_cast<uint32_t>(meshLocations.size());

			ClusterStaticMeshHandle handle =
				cluster.staticMeshes.getHandle(meshIdx);

			meshLocations.push_back(
				MeshLocation{ .cluster = &cluster, .handle = handle }
			);
			// Cook-time LOD marker (LOD0; siblings resolve via SelectLodIndex).
			LodInfo lod;
			lod.groupId = meshIdx;
			if (const StringViewT uri = cluster.GetMeshUri(handle);
				!uri.empty()) {
				if (auto hit = cluster.GetMeshLod(uri))
					lod = *hit;
			}
			if (const StaticMesh* m = cluster.staticMeshes.get(handle))
				lod.triCount = m->geometry.indexCount / 3;
			meshLodInfos.push_back(lod);
			scene.meshForNode[worldNode] = worldMeshIdx;
		}

		// Remap materialForNode: cluster-local handle index -> world mat idx.
		// Deduplicated by (cluster, cluster-local material): instanced
		// scenes share a handful of materials across hundreds of thousands
		// of nodes (scanlands: 96 materials over 374k instances). One slot
		// per instance overflows the GPU materials buffer (MAX_MATERIALS)
		// and shaders read OOB past it (GPU fault / device loss on heavy
		// views). Mesh slots stay per-node (CPU-side only); material slots
		// are GPU-indexed by materialId, so they must stay addressable.
		for (auto& [node, matIdx] : cs.materialForNode) {
			const uint32_t worldNode = node + nodeOffset;
			const uint64_t matKey =
				(static_cast<uint64_t>(clusterOrdinal) << 32) | matIdx;
			auto matHit = worldMatByClusterMat.find(matKey);
			if (matHit != worldMatByClusterMat.end()) {
				scene.materialForNode[worldNode] = matHit->second;
				continue;
			}
			const uint32_t worldMatIdx =
				static_cast<uint32_t>(materialLocations.size());

			ClusterMaterialHandle handle =
				cluster.materials.getHandle(matIdx);

			materialLocations.push_back(
				MaterialLocation{ .cluster = &cluster, .handle = handle }
			);
			worldMatByClusterMat[matKey] = worldMatIdx;
			scene.materialForNode[worldNode] = worldMatIdx;
		}

		// Remap lightForNode: cluster-local light pool index -> world
		// light index. lightLocations resolves back to Cluster + handle
		// exactly like meshLocations (cooked lights are pool assets).
		for (auto& [node, lightIdx] : cs.lightForNode) {
			const uint32_t worldNode = node + nodeOffset;
			const uint32_t worldLightIdx =
				static_cast<uint32_t>(lightLocations.size());

			ClusterLightHandle handle =
				cluster.lights.getHandle(lightIdx);

			lightLocations.push_back(
				LightLocation{ .cluster = &cluster, .handle = handle }
			);
			scene.lightForNode[worldNode] = worldLightIdx;
		}

		// Remap cameraForNode: cluster-local camera pool index -> world
		// camera index. cameraLocations resolves back to Cluster + handle
		// exactly like lightLocations (authored cameras are pool assets).
		for (auto& [node, camIdx] : cs.cameraForNode) {
			const uint32_t worldNode = node + nodeOffset;
			const uint32_t worldCamIdx =
				static_cast<uint32_t>(cameraLocations.size());

			ClusterCameraHandle handle =
				cluster.cameras.getHandle(camIdx);

			cameraLocations.push_back(
				CameraLocation{ .cluster = &cluster, .handle = handle }
			);
			scene.cameraForNode[worldNode] = worldCamIdx;
		}

		// Authored environment: per-node blocks offset-merge like the
		// other node maps (values are inline structs — direct copy);
		// the resolved look + default block ride first-wins across
		// clusters (registration order: the owning scene first), so an
		// unauthored default never shadows another scene's authored look.
		for (auto& [node, env] : cs.envForNode)
			scene.envForNode[node + nodeOffset] = env;
		if (!envTaken && cs.hasEnvironment) {
			scene.environment = cs.environment;
			scene.hasEnvironment = true;
			scene.defaultEnvironment = cs.defaultEnvironment;
			envTaken = true;
		}

		markAsChanged(scene, clusterRootNode);
		++clusterOrdinal;
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
	recalculateGlobalTransforms(scene);

	// Build the per-node iterator flags from the resolved scene: renderable
	// nodes are those carrying a mesh; material drives Opaque vs Transparent.
	// Hidden subtrees (Clarisse-style visibility overrides) are excluded:
	// they keep None so no pass iterates them (see drawItems below).
	flags.assign(scene.hierarchy.size(), IteratorFlagBit_None);
	for (auto& [node, meshIdx] : scene.meshForNode) {
		if (node >= flags.size())
			continue;
		if (!IsVisibleNode(scene, int(node)))
			continue;

		IteratorFlags f = static_cast<IteratorFlags>(IteratorFlagBit_Renderable)
			| static_cast<IteratorFlags>(IteratorFlagBit_StaticVertexFmt);
		if (IsStaticNode(scene, (int)node))
			f |= static_cast<IteratorFlags>(IteratorFlagBit_Static);

			// Material-driven classification. Falls back to Opaque if node has
			// no material (e.g. default).
			auto matIt = scene.materialForNode.find(node);
			if (matIt != scene.materialForNode.end()) {
				uint32_t matIdx = matIt->second;
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
						} else {
							f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
								| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
						}
					} else {
						f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
							| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
					}
				} else {
					f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
						| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
				}
			} else {
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
	drawItems.reserve(scene.meshForNode.size());
	flagLists.clear();
	for (auto& [node, meshIdx] : scene.meshForNode) {
		if (node >= flags.size())
			continue;
		if (!IsVisibleNode(scene, int(node)))
			continue; // hidden override: not drawn, not picked, not baked-listed
		const auto matIt = scene.materialForNode.find(node);
			drawItems.push_back(RenderItem{
				.node = node,
				.mesh = meshIdx,
				.material = (matIt != scene.materialForNode.end())
					? matIt->second : kInvalidIndex,
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
	// Shader LODs: the farthest level of a cooked chain (length > 1) is
	// stamped LodShader::VertexLit — same coarsest geometry, cheap
	// per-vertex shading. Uncooked chains stay fully lit.
	void World::RebuildLodChains() {
		lodChains.clear();
		lodChains.resize(meshLocations.size());
		InvalidateLodCache();
		for (uint32_t i = 0; i < (uint32_t)meshLocations.size(); ++i) {
			LodChain& chain = lodChains[i];
			const MeshLocation& lod0 = meshLocations[i];
			if (lod0.cluster == nullptr || !lod0.handle.valid())
				continue;
			chain.levels.push_back(LodChain::Level{ lod0, 0.0f, LodShader::Lit });
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
					!meshHit->handle.valid())
					break;
				chain.levels.push_back(
					LodChain::Level{ *meshHit, err, LodShader::Lit });
			}
			if (chain.levels.size() > 1)
				chain.levels.back().shader = LodShader::VertexLit;
		}
	}

	// ---------------------------------------------------------------------------
	// Per-node LOD decision cache (full rate by default, halfrate parity
	// opt-out; see world.h).
	// ---------------------------------------------------------------------------

	void World::InvalidateLodCache() {
		nodeLod_.clear();
		nodeLodStamp_.clear();
		lodTick_ = 0;
		lodLastFrameId_ = ~uint64_t(0);
	}

	uint32_t World::LodIndexForNode(NodeId node, uint32_t worldMesh,
		const LodFrameInputs& inputs) const {
		// Frame rollover: adopt this frame's inputs, tick the parity.
		if (inputs.frameId != lodLastFrameId_) {
			lodLastFrameId_ = inputs.frameId;
			lodInputs_ = inputs;
			++lodTick_;
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
				nodeLod_[n] = static_cast<uint8_t>(SelectLodIndex(worldMesh,
					LodDistanceForNode(node, worldMesh, lodInputs_.eye),
					lodInputs_.viewportHeight, lodInputs_.fovY,
					lodInputs_.pixelBudget, nodeLod_[n]));
				nodeLodStamp_[n] = inputs.frameId;
			}
		}
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

	LodShader World::LodShaderAt(uint32_t meshIndex, uint32_t lodIndex) const {
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size())
			return LodShader::Lit;
		const LodChain& chain = lodChains[meshIndex];
		if (lodIndex >= chain.levels.size())
			return LodShader::Lit;
		return chain.levels[lodIndex].shader;
	}

	uint32_t World::SelectLodIndex(uint32_t meshIndex, float distance,
		float viewportHeightPx, float fovYRadians, float maxPixelError,
		uint32_t currentLod) const {
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
		// stack (chains are capped at 16 levels by RebuildLodChains).
		float errors[16];
		const uint32_t n = (uint32_t)std::min<size_t>(chain.levels.size(), 16);
		for (uint32_t i = 0; i < n; ++i)
			errors[i] = chain.levels[i].error;
		return SelectLodIndexForDistance(errors, n, distance,
			viewportHeightPx, fovYRadians, maxPixelError, currentLod,
			kLodSelectHysteresis);
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

	// Stub culling: everything is considered visible until a real culler exists.
	void World::Cull(CullConfiguration config)
	{
		for (uint32_t i = 0; i < flags.size(); ++i) {
			flags[i] |= static_cast<IteratorFlags>(IteratorFlagBit_PassedCullTest);
		}
		// Keep the compact draw list in sync with flags[] (its entries
		// carry a copy; see forEachRenderItem) and drop the cooked index
		// lists — the flag bits they were built from just changed.
		for (size_t i = 0; i < drawItems.size() && i < itemFlags.size(); ++i) {
			drawItems[i].flags |= static_cast<IteratorFlags>(IteratorFlagBit_PassedCullTest);
			itemFlags[i] = drawItems[i].flags;
		}
		flagLists.clear();
		dirtyCull = false;
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
				{localMin.x, localMin.y, localMin.z},
				{localMin.x, localMin.y, localMax.z},
				{localMin.x, localMax.y, localMin.z},
				{localMin.x, localMax.y, localMax.z},
				{localMax.x, localMin.y, localMin.z},
				{localMax.x, localMin.y, localMax.z},
				{localMax.x, localMax.y, localMin.z},
				{localMax.x, localMax.y, localMax.z},
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
		prims.reserve(scene.meshForNode.size());
		for (const auto& [node, worldMeshIdx] : scene.meshForNode) {
			if (!IsStaticNode(scene, (int)node))
				continue;
			if (worldMeshIdx >= meshLocations.size())
				continue;
			StaticMesh* base = GetMesh(worldMeshIdx);
			if (!base)
				continue;
			const glm::mat4& model = scene.globalTransform[node];
			::Aabb bounds = WorldAabbForMesh(base, model);
			// LOD-chain union: any LodIndexForNode selection stays inside.
			if (worldMeshIdx < lodChains.size()) {
				const LodChain& chain = lodChains[worldMeshIdx];
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
		for (const auto& [node, worldMeshIdx] : scene.meshForNode) {
			if (worldMeshIdx >= meshLocations.size())
				continue;
			const MeshLocation& loc = meshLocations[worldMeshIdx];
			if (loc.cluster == nullptr || !loc.handle.valid())
				continue;
			const StaticMesh* base = loc.cluster->staticMeshes.get(loc.handle);
			if (!base)
				continue;
			if (node >= scene.globalTransform.size())
				continue;
			out.expand(WorldAabbForMesh(base, scene.globalTransform[node]));
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
				int regs[4];
				__cpuid(regs, 0);
				if (regs[0] < 7)
					return false;
				__cpuid(regs, 1);
				const bool osxsave = (regs[2] & (1u << 27)) != 0;
				if (!osxsave)
					return false;
				__cpuidex(regs, 7, 0);
				return (regs[1] & (1u << 5)) != 0;
			}();
			return has;
		}

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
		else {
			for (size_t i = 0; i < n; ++i) {
				if ((itemFlags[i] & requiredFlags) == requiredFlags)
					indices.push_back(static_cast<uint32_t>(i));
			}
		}
		return indices;
	}

} // namespace smvk
