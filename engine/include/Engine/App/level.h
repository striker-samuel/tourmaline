#pragma once

#include <Engine/Renderer/cluster.h>
#include <Engine/Utils/gltf.h> 
#include <Engine/Utils/bvh.h>

struct Level : Scene {
	~Level() {}
	smvk::Cluster* cluster;
	BvhTree tree = {};
	BvhTree static_tree = {};
	bool tree_done = false;

	void Upload(
		std::filesystem::path gltf_model_path
	) {
		tinygltf::Model model;
		LoadModelFromAsciiFile(model, gltf_model_path.string().c_str());
		GltfToSceneImporter importer(
			*this,
			*cluster,
			model,
			std::move(gltf_model_path
			)
		);
		importer.importNodeRecursive(/*rootNodeIdx*/0, /*parentSceneNode=*/-1, /*level=*/0);
		// This is needed or else scene will not carry transforms effectively
		markAsChanged(*this, 0);
		recalculateGlobalTransforms(*this);

		cluster->ctx_.ProcessDeferrredTasks();
	}
};

