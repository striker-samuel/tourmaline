// Portions Copyright (c) 2022 Packt
// Modified by Samuel Martin, 2026
// Licensed under the MIT License. See external/Mastering-Graphics-Programming-with-Vulkan/LICENSE.txt for details.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/ext.hpp>

#include <Engine/Renderer/light.h>

using glm::mat4;

// we do not define std::vector<Node*> Children - this is already present in the aiNode from assimp

constexpr const int MAX_NODE_LEVEL = 16;


//struct SceneLight {
//
//	glm::vec4 color = glm::vec4(1.0f);
//	float position[3] = { 0.0f,0.0f,0.0f };
//	int32_t flags = 0;
//	float direction[3] = { 0.0f,0.0f,0.0f };
//	float intensity = .0f;
//	int32_t type = LIGHT_TYPE_POINT;
//
//	float innerAngle = glm::radians(10.0f);
//	float outerAngle = glm::radians(20.0f);
//};
//

// This was lifted from https://github.com/PacktPublishing/Mastering-Graphics-Programming-with-Vulkan

struct Hierarchy {
	// parent for this node (or -1 for root)
	int parent = -1;
	// first child for a node (or -1)
	int firstChild = -1;
	// next sibling for a node (or -1)
	int nextSibling = -1;
	// last added node (or -1)
	int lastSibling = -1;
	// cached node level
	int level = 0;
};

/* This scene is converted into a descriptorSet(s) in MultiRenderer class
   This structure is also used as a storage type in SceneExporter tool
 */
struct Scene {
	// local transformations for each node and global transforms
	// + an array of 'dirty/changed' local transforms
	std::vector<mat4> localTransform;  // indexed by node
	std::vector<mat4> globalTransform; // indexed by node

	// list of nodes that need their global transforms recalculated
	std::vector<int> changedAtThisFrame[MAX_NODE_LEVEL];

	// Hierarchy component
	std::vector<Hierarchy> hierarchy;

	// MeshData component: which MeshData belongs to which node (Node -> MeshData)
	std::unordered_map<uint32_t, uint32_t> meshForNode;

	// Material component: which material belongs to which node (Node -> Material)
	std::unordered_map<uint32_t, uint32_t> materialForNode;

	// Node name component: which name is assigned to the node (Node -> Name)
	std::unordered_map<uint32_t, uint32_t> nameForNode;

	// Lets add light support to the scene def
	std::unordered_map<uint32_t, uint32_t> lightForNode;

//	std::vector<struct Light> lights;

	// List of scene node names
	std::vector<std::string> nodeNames;

	// Debug list of material names
	std::vector<std::string> materialNames;
};

int addNode(Scene& scene, int parent, int level);

void markAsChanged(Scene& scene, int node);

int findNodeByName(const Scene& scene, const std::string& name);

inline std::string getNodeName(const Scene& scene, int node)
{
	int strID = scene.nameForNode.contains(node) ? scene.nameForNode.at(node) : -1;
	return (strID > -1) ? scene.nodeNames[strID] : std::string();
}

inline void setNodeName(Scene& scene, int node, const std::string& name)
{
	uint32_t stringID = (uint32_t)scene.nodeNames.size();
	scene.nodeNames.push_back(name);
	scene.nameForNode[node] = stringID;
}

int getNodeLevel(const Scene& scene, int n);

bool recalculateGlobalTransforms(Scene& scene);

void loadScene(const char* fileName, Scene& scene);
void saveScene(const char* fileName, const Scene& scene);

void dumpTransforms(const char* fileName, const Scene& scene);
void printChangedNodes(const Scene& scene);

void dumpSceneToDot(const char* fileName, const Scene& scene, int* visited = nullptr);

void mergeScenes(
	Scene& scene,
	const std::vector<Scene*>& scenes,
	const std::vector<glm::mat4>& rootTransforms,
	const std::vector<uint32_t>& meshCounts,
	bool mergeMeshes = true,
	bool mergeMaterials = true,
	bool mergeLigths = true
);

// Delete a collection of nodes from a scenegraph
void deleteSceneNodes(Scene& scene, const std::vector<uint32_t>& nodesToDelete);



