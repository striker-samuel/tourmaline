#include <Engine/App/world.h>

#include <cassert>
#include <iostream>
#include <stdexcept>

#include <Engine/Scene/scene.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Renderer/mesh.h>

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

	Cluster& World::LoadClusterFromPak(ResourceManager& ctx, const PakReader& reader)
	{
		std::string err;
		auto cluster = DeserializeCluster(ctx, reader, &err);
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
		meshLodInfos.clear();
		flags.clear();

		// World root node (parent = -1)
		const int root = addNode(scene, -1, 0);
		scene.localTransform[root] = glm::mat4(1.0f);
		scene.globalTransform[root] = glm::mat4(1.0f);
		scene.staticRoot.assign(scene.hierarchy.size(), 0);
		scene.staticNode.assign(scene.hierarchy.size(), 0);

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
			if (const std::string uri = cluster.GetMeshUri(handle);
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
			for (auto& [node, matIdx] : cs.materialForNode) {
				const uint32_t worldNode = node + nodeOffset;
				const uint32_t worldMatIdx =
					static_cast<uint32_t>(materialLocations.size());

				ClusterMaterialHandle handle =
					cluster.materials.getHandle(matIdx);

				materialLocations.push_back(
					MaterialLocation{ .cluster = &cluster, .handle = handle }
				);
				scene.materialForNode[worldNode] = worldMatIdx;
			}

		markAsChanged(scene, clusterRootNode);
	}

	RebuildStaticCache(scene);
	recalculateGlobalTransforms(scene);

		// Build the per-node iterator flags from the resolved scene: renderable
		// nodes are those carrying a mesh; material drives Opaque vs Transparent.
		flags.assign(scene.hierarchy.size(), IteratorFlagBit_None);
		for (auto& [node, meshIdx] : scene.meshForNode) {
			if (node >= flags.size())
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
							else
								f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque);

							// Masked (cutout) still casts shadow but via alpha-tested path.
							// Transparent (BLEND) does not cast: full-quad depth would
							// darken behind glass/foliage. Shadow passes additionally
							// filter Opaque so this flag is defense-in-depth.
							if (!mat->isTransparent())
								f |= static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);
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

		RebuildLodChains();
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
	// cache). URI probing + GetMeshUri's linear scan happen here, once per
	// scene build, instead of once per draw per pass per frame.
	void World::RebuildLodChains() {
		lodChains.clear();
		lodChains.resize(meshLocations.size());
		for (uint32_t i = 0; i < (uint32_t)meshLocations.size(); ++i) {
			LodChain& chain = lodChains[i];
			const MeshLocation& lod0 = meshLocations[i];
			if (lod0.cluster == nullptr || !lod0.handle.valid())
				continue;
			chain.lods.push_back(lod0);
			chain.errors.push_back(0.0f);
			const std::string base =
				BaseMeshUriForLod(lod0.cluster->GetMeshUri(lod0.handle));
			if (base.empty())
				continue;
			for (uint32_t li = 1; li < 16; ++li) {
				const std::string uri = LodSiblingUri(base, li);
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
				chain.lods.push_back(*meshHit);
				chain.errors.push_back(err);
			}
		}
	}

	static void EnsureLodChainsCached(const World& w) {
		if (w.lodChains.size() != w.meshLocations.size())
			const_cast<World&>(w).RebuildLodChains();
	}

	uint32_t World::LodChainLength(uint32_t meshIndex) const {
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size())
			return 1;
		const size_t n = lodChains[meshIndex].lods.size();
		return n == 0 ? 1 : (uint32_t)n;
	}

	std::optional<World::MeshLocation> World::ResolveLodMesh(
		uint32_t meshIndex, uint32_t lodIndex) const {
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size())
			return std::nullopt;
		const LodChain& chain = lodChains[meshIndex];
		if (lodIndex >= chain.lods.size())
			return std::nullopt;
		const MeshLocation& loc = chain.lods[lodIndex];
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
		if (lodIndex >= chain.errors.size())
			return 0.0f;
		return chain.errors[lodIndex];
	}

	uint32_t World::SelectLodIndex(uint32_t meshIndex, float distance,
		float viewportHeightPx, float fovYRadians, float maxPixelError) const {
		if (!(distance > 0.0f) || !(maxPixelError >= 0.0f))
			return 0;
		EnsureLodChainsCached(*this);
		if (meshIndex >= lodChains.size())
			return 0;
		const LodChain& chain = lodChains[meshIndex];
		if (chain.errors.size() <= 1)
			return 0;
		return SelectLodIndexForDistance(chain.errors.data(),
			(uint32_t)chain.errors.size(), distance,
			viewportHeightPx, fovYRadians, maxPixelError);
	}

	std::optional<World::MeshLocation> World::ResolveMeshAcrossClusters(
		const std::string& uri) const
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
		const std::string& uri) const
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
		dirtyCull = false;
	}

} // namespace smvk
