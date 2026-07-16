#pragma once
#include <Engine/Scene/scene.h>


struct Level : Scene {
	MeshManager* m_meshManagerPimpl = nullptr;
	BvhTree tree = {};
	BvhTree static_tree = {};
	bool tree_done = false;
	//std::map<uint32_t, MeshHandle> m_meshForNode;
	//Level(MeshManager* meshManager) : m_meshManagerPimpl(meshManager) {};
	~Level() {}

	void Upload(
		std::filesystem::path gltf_model_path
	) {
		tinygltf::Model model;
		LoadModelFromAsciiFile(model, gltf_model_path.string().c_str());
		GltfToSceneImporter importer(*m_meshManagerPimpl, *this, model, std::move(gltf_model_path));
		importer.importNodeRecursive(/*rootNodeIdx*/0, /*parentSceneNode=*/-1, /*level=*/0);
		// This is needed or else scene will not carry transforms effectively
		markAsChanged(*this, 0);
		recalculateGlobalTransforms(*this);
	}

	//template<typename Func>
	//	void forEachNode(Func&& func) {
	//		for (int node = 0; hierarchy.size(); node++) {
	//			func(node);
	//		}
	//	}
};
