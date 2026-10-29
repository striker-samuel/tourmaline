// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/App/static_merge.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include <glm/gtc/matrix_inverse.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Scene/scene.h>

namespace smvk {

std::vector<uint32_t> CollectStaticRenderNodes(const ::Scene& scene)
{
	std::vector<uint32_t> out;
	for (uint32_t i = 0; i < (uint32_t)scene.hierarchy.size(); ++i) {
		if (i >= scene.staticNode.size() || scene.staticNode[i] == 0)
			continue;
		if (scene.meshForNode.find(i) == scene.meshForNode.end())
			continue;
		out.push_back(i);
	}
	return out;
}

StaticMergeResult CollapseStaticSubtrees(
	Cluster& cluster,
	const StaticMergeOptions& opts)
{
	StaticMergeResult result;
	::Scene& scene = cluster.scene;

	if (scene.staticRoot.size() != scene.hierarchy.size())
		RebuildStaticCache(scene);

	const std::vector<uint32_t> nodes = CollectStaticRenderNodes(scene);
	if (nodes.empty()) {
		result.ok = true; // nothing to do
		return result;
	}

	// Group source nodes by material pool index (missing material gets its
	// own group keyed by UINT32_MAX).
	std::map<uint32_t, std::vector<uint32_t>> groups;
	for (uint32_t n : nodes) {
		uint32_t matKey = UINT32_MAX;
		auto mit = scene.materialForNode.find(n);
		if (mit != scene.materialForNode.end())
			matKey = mit->second;
		groups[matKey].push_back(n);
	}

	struct BakedGroup {
		uint32_t matKey = UINT32_MAX;
		std::vector<uint32_t> srcNodes;
		StaticMeshUpload upload;
	};
	std::vector<BakedGroup> baked;
	baked.reserve(groups.size());

	for (auto& [matKey, members] : groups) {
		BakedGroup g;
		g.matKey = matKey;
		g.srcNodes = members;
		g.upload.vertices.reserve(1024);
		g.upload.indices.reserve(3072);

		glm::vec3 mn(
			std::numeric_limits<float>::infinity());
		glm::vec3 mx(
			-std::numeric_limits<float>::infinity());

		for (uint32_t n : members) {
			auto meshIt = scene.meshForNode.find(n);
			if (meshIt == scene.meshForNode.end())
				continue;
			const ClusterStaticMeshHandle h =
				cluster.staticMeshes.getHandle(meshIt->second);
			if (!h.valid()) {
				result.err = "CollapseStaticSubtrees: stale mesh handle";
				return result;
			}
			const StaticMeshUpload* src = cluster.GetRetainedUpload(h);
			if (!src) {
				result.err = "CollapseStaticSubtrees: no retained upload "
					"(import with SetRetainUploads(true)/cook mode)";
				return result;
			}
			if (src->vertices.empty() || src->indices.empty())
				continue;

			const glm::mat4& global = scene.globalTransform[n];
			glm::mat3 normalMat(1.0f);
			const float det = glm::determinant(glm::mat3(global));
			if (std::isfinite(det) && fabsf(det) > 1e-12f)
				normalMat = glm::transpose(glm::inverse(glm::mat3(global)));

			const uint32_t base = (uint32_t)g.upload.vertices.size();
			for (const ImportStaticVertex& v : src->vertices) {
				ImportStaticVertex b = v;
				const glm::vec4 wp = global * glm::vec4(v.pos, 1.0f);
				b.pos = glm::vec3(wp);
				b.normal = glm::normalize(normalMat * v.normal);
				const glm::vec3 t = normalMat * glm::vec3(v.tangent);
				const float tl = glm::length(t);
				b.tangent = (tl > 1e-12f)
					? glm::vec4(t / tl, v.tangent.w)
					: glm::vec4(1.0f, 0.0f, 0.0f, v.tangent.w);
				g.upload.vertices.push_back(b);
				mn = glm::min(mn, b.pos);
				mx = glm::max(mx, b.pos);
			}
			for (uint32_t idx : src->indices)
				g.upload.indices.push_back(base + idx);
		}

		if (g.upload.vertices.empty() || g.upload.indices.empty())
			continue;
		g.upload.aabbMin = mn;
		g.upload.aabbMax = mx;
		baked.push_back(std::move(g));
	}

	if (baked.empty()) {
		result.ok = true;
		return result;
	}

	// Upload each group as one block. UploadStaticMesh assigns GPU regions
	// (or CPU-only geometry in cook mode); vertices are already world-baked
	// so the new nodes use identity transforms.
	int groupIdx = 0;
	for (auto& g : baked) {
		const ClusterStaticMeshHandle h = cluster.UploadStaticMesh(g.upload);
		if (!h.valid()) {
			result.err = "CollapseStaticSubtrees: UploadStaticMesh failed";
			return result;
		}
		const std::string uri = opts.uriPrefix +
			".mat." + std::to_string(g.matKey) +
			".g" + std::to_string(groupIdx);
		cluster.RegisterMeshUri(uri, h);
		{
			LodInfo lod;
			lod.groupId = h.index();
			lod.lodIndex = 0;
			lod.triCount = (uint32_t)(g.upload.indices.size() / 3);
			cluster.RegisterMeshLod(uri, lod);
		}

		const int node = addNode(scene, 0, 1);
		scene.localTransform[(size_t)node] = glm::mat4(1.0f);
		scene.globalTransform[(size_t)node] = glm::mat4(1.0f);
		if (scene.staticRoot.size() != scene.hierarchy.size()) {
			scene.staticRoot.assign(scene.hierarchy.size(), 0);
			scene.staticNode.assign(scene.hierarchy.size(), 0);
		}
		scene.meshForNode[(uint32_t)node] = h.index();
		if (g.matKey != UINT32_MAX)
			scene.materialForNode[(uint32_t)node] = g.matKey;
		{
			const int nameIdx = (int)scene.nodeNames.size();
			scene.nodeNames.push_back("StaticMerged");
			scene.nameForNode[(uint32_t)node] = (uint32_t)nameIdx;
		}
		// Merged blocks are frozen by construction.
		scene.staticRoot[(size_t)node] = 1;

		MergedBlock b;
		b.materialPoolIndex = g.matKey;
		b.hasMaterial = (g.matKey != UINT32_MAX);
		b.srcNodeCount = (uint32_t)g.srcNodes.size();
		b.node = node;
		b.meshPoolIndex = h.index();
		b.uri = uri;
		result.blocks.push_back(std::move(b));
		result.srcNodesConsumed += (uint32_t)g.srcNodes.size();
		++groupIdx;
	}

	RebuildStaticCache(scene);

	if (opts.deleteSources) {
		// Delete *after* all uploads: node indices in `nodes` are still
		// valid until the erase. New block nodes sort after every source
		// (addNode appends), so they are never in the delete set.
		std::vector<uint32_t> doomed = nodes;
		std::sort(doomed.begin(), doomed.end());
		deleteSceneNodes(scene, doomed);
		// Node ids of the new blocks shifted; repair them by URI lookup.
		for (auto& b : result.blocks) {
			b.node = -1;
			for (const auto& [node, meshIdx] : scene.meshForNode) {
				if (meshIdx == b.meshPoolIndex) {
					b.node = (int)node;
					break;
				}
			}
		}
		RebuildStaticCache(scene);
	}

	MarkAllRootsChanged(scene);
	recalculateGlobalTransforms(scene);

	result.ok = true;
	return result;
}

} // namespace smvk
