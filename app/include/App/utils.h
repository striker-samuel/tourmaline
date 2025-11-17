//
//// ------------------------------------
//// If theres only one I probably could go with argv
//std::vector<std::string> gltf_meshes_paths = {
//	"models/Sponza/sponza.gltf" // Index 0
//};
//
//Scene getSceneFromGltf(const GltfModel& gltf_model) {
//
//	Scene scene;
//
//	// adding top parent to apply scale here
//
//	// top parent
//	int root = addNode(scene, -1, 0);
//
//	int meshIndex = 0;
//	for (auto& mesh_data : gltf_model.mesh_data) {
//		int node = addNode(scene, root, 1); // child of root
//		scene.meshForNode[node] = meshIndex;
//
//		// TODO: set real material index from mesh_data
//		scene.materialForNode[node] = 0;
//
//		meshIndex++;
//	}
//
//	scene.localTransform[0] = glm::scale(glm::mat4(1.0f), glm::vec3(0.008f)/*, glm::vec3(0.008f)*/);
//	markAsChanged(scene, 0);
//	return scene;
//}