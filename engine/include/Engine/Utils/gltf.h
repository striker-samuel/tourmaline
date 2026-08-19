#pragma once

#include <filesystem>
#include <vector>

#include <tiny_gltf.h>

#include <Engine/App/cluster.h>

struct GltfToSceneImporter
{
	smvk::Cluster& cluster;

	const tinygltf::Model& model;
	std::filesystem::path gltfModelPath;

	// glTF-local index -> resource owned by this Cluster.
	std::vector<smvk::ClusterMaterialHandle> materialCache;

	// glTF mesh index -> primitive index -> Cluster mesh handle.
	std::vector<std::vector<smvk::ClusterStaticMeshHandle>> meshCache;

	GltfToSceneImporter(
		smvk::Cluster& inCluster,
		const tinygltf::Model& inModel,
		std::filesystem::path inGltfModelPath
	)
		: cluster(inCluster)
		, model(inModel)
		, gltfModelPath(std::move(inGltfModelPath))
		, materialCache(inModel.materials.size())
	{
		meshCache.resize(inModel.meshes.size());

		for (size_t i = 0; i < inModel.meshes.size(); ++i) {
			meshCache[i].resize(
				inModel.meshes[i].primitives.size()
			);
		}
	}

	smvk::ClusterMaterialHandle
		getOrCreateMaterial(int gltfMaterialIndex);

	smvk::ClusterStaticMeshHandle
		getOrCreateMesh(
			int gltfMeshIndex,
			int primitiveIndex
		);

	void importNodeRecursive(
		int gltfNodeIndex,
		int parentSceneNode,
		int level
	);
};


bool LoadModelFromAsciiFile(
	tinygltf::Model& model,
	const char* filename
);

inline void GltfToCluster(
	smvk::VulkanResourceManager& /*context*/,
	const tinygltf::Model& inModel,
	std::filesystem::path inGltfModelPath,
	smvk::Cluster& out
) {
	GltfToSceneImporter importer{
		out,
		inModel,
		inGltfModelPath
	};

	// Cluster-scene root (index 0, parent = -1). Imported glTF scene roots
	// attach underneath it so addNode() always sees a valid parent index.
	const int root = addNode(out.scene, -1, 0);
	out.scene.localTransform[root] = glm::mat4(1.0f);
	out.scene.globalTransform[root] = glm::mat4(1.0f);

	// Collect the glTF top-level node indices.
	std::vector<int> gltfRoots;
	if (inModel.defaultScene >= 0 && inModel.defaultScene < (int)inModel.scenes.size()) {
		gltfRoots = inModel.scenes[inModel.defaultScene].nodes;
	} else if (!inModel.scenes.empty()) {
		gltfRoots = inModel.scenes.front().nodes;
	} else {
		// Fallback: a node is a root if it isn't referenced as anyone's child.
		std::vector<bool> referenced(inModel.nodes.size());
		for (const auto& n : inModel.nodes)
			for (int c : n.children)
				if (c >= 0 && c < (int)inModel.nodes.size())
					referenced[c] = true;
		for (int i = 0; i < (int)inModel.nodes.size(); ++i)
			if (!referenced[i]) gltfRoots.push_back(i);
	}

	for (int gltfNode : gltfRoots)
		importer.importNodeRecursive(gltfNode, root, 1);

	markAsChanged(out.scene, root);
	recalculateGlobalTransforms(out.scene);
}

