#include <Engine/App/world.h>

#include <cassert>
#include <iostream>
#include <stdexcept>

#include <algorithm>
#include <bit>
#include <immintrin.h>
#include <intrin.h>

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
							else {
								f |= static_cast<IteratorFlags>(IteratorFlagBit_Opaque)
									| static_cast<IteratorFlags>(IteratorFlagBit_CastShadow);

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
	void World::RebuildLodChains() {
		lodChains.clear();
		lodChains.resize(meshLocations.size());
		InvalidateLodCache();
		for (uint32_t i = 0; i < (uint32_t)meshLocations.size(); ++i) {
			LodChain& chain = lodChains[i];
			const MeshLocation& lod0 = meshLocations[i];
			if (lod0.cluster == nullptr || !lod0.handle.valid())
				continue;
			chain.lods.push_back(lod0);
			chain.errors.push_back(0.0f);
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
				chain.lods.push_back(*meshHit);
				chain.errors.push_back(err);
			}
		}
	}

	// ---------------------------------------------------------------------------
	// Halfrate LOD cache (staggered halves; see world.h).
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
			const bool parityDue = (lodTick_ & 1) == (n & 1);
			const bool hasOld = nodeLodStamp_[n] != 0;
			if (parityDue || !hasOld) {
				nodeLod_[n] = static_cast<uint8_t>(SelectLodIndex(worldMesh,
					glm::length(glm::vec3(scene.globalTransform[n][3])
						- lodInputs_.eye),
					lodInputs_.viewportHeight, lodInputs_.fovY,
					lodInputs_.pixelBudget));
				nodeLodStamp_[n] = inputs.frameId;
			}
		}
		return nodeLod_[n];
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
				for (size_t li = 1; li < chain.lods.size(); ++li) {
					const MeshLocation& loc = chain.lods[li];
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
