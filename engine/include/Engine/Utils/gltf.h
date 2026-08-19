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

void GltfToCluster(
	smvk::VulkanResourceManager& context,
	const tinygltf::Model& inModel,
	std::filesystem::path inGltfModelPath,
	smvk::Cluster out
) {
	GltfToSceneImporter importer{
		out,
		inModel,
		inGltfModelPath
	};
	importer.importNodeRecursive(0, 1, 0);
	markAsChanged(out.scene, 0);
	recalculateGlobalTransforms(out.scene);
}

