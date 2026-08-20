#pragma once

#include <filesystem>
#include <vector>

#include <tiny_gltf.h>

#include <Engine/App/cluster.h>

struct GltfToSceneImporter
{
	GltfToSceneImporter(
		smvk::Cluster& inCluster,
		const tinygltf::Model& inModel,
		std::filesystem::path inGltfModelPath
	);
	void import(int gltfNodeIndex) {
		importNodeRecursive(gltfNodeIndex, 0, 1);
	}
private:
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
	smvk::Cluster& cluster;
	const tinygltf::Model& model;
	std::filesystem::path gltfModelPath;
	std::vector<smvk::ClusterMaterialHandle> materialCache;
	std::vector<std::vector<smvk::ClusterStaticMeshHandle>> meshCache;
};


bool LoadModelFromAsciiFile(
	tinygltf::Model& model,
	const char* filename
);

inline void GltfToCluster(
	smvk::VulkanResourceManager&,
	const tinygltf::Model& inModel,
	std::filesystem::path inGltfModelPath,
	smvk::Cluster& out
) {
	GltfToSceneImporter importer{
		out,
		inModel,
		inGltfModelPath
	};

	assert(out.scene.hierarchy.empty());

	// Cluster-scene root (index 0, parent = -1). Imported glTF scene roots
	// attach underneath it so addNode() always sees a valid parent index.
	const int root = addNode(out.scene, -1, 0);

	out.scene.localTransform[root] = glm::mat4(1.0f);
	out.scene.globalTransform[root] = glm::mat4(1.0f);

	// Collect the glTF top-level node indices.
	std::vector<int> gltfRoots;
	if (inModel.defaultScene >= 0 && inModel.defaultScene < (int)inModel.scenes.size()) {
		gltfRoots = inModel.scenes[inModel.defaultScene].nodes;
	}
	else if (!inModel.scenes.empty()) {
		gltfRoots = inModel.scenes.front().nodes;
	}
	else {
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
		importer.import(gltfNode);
		//importer.importNodeRecursive(gltfNode, root, 1);

	markAsChanged(out.scene, root);
	recalculateGlobalTransforms(out.scene);
}

