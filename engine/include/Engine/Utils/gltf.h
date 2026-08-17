#pragma once

#include <filesystem>
#include <tiny_gltf.h>

#include <Engine/Scene/Scene.h>
#include <Engine/Renderer/cluster.h>

struct GltfToSceneImporter {
	Scene& scene;
	smvk::Cluster& manager_;

	const tinygltf::Model& model; 
	std::filesystem::path gltfModelPath; // full path to .gltf/.glb on disk
	static constexpr uint32_t kInvalidMat = 0xFFFFFFFFu;
	std::vector<uint32_t> materialCache;

	GltfToSceneImporter
	(
		Scene& inScene
		, smvk::Cluster& manager 
		, const tinygltf::Model& inModel
		, std::filesystem::path inGltfModelPath
	)
		: scene(inScene)
		, manager_(manager)
		, model(inModel)
		, gltfModelPath(std::move(inGltfModelPath))
		, materialCache(inModel.materials.size(), kInvalidMat)
	{}

	uint32_t getOrCreateMaterial(int gltfMaterialIndex);
	uint32_t getOrCreateMesh(tinygltf::Primitive primitive);
	void importNodeRecursive(int gltfNodeIndex, int parentSceneNode, int level);
};

bool LoadModelFromAsciiFile(tinygltf::Model& model, const char* filename);
