// Portions Copyright (c) 2022 Packt
// Modified by Samuel Martin, 2026
// Licensed under the MIT License. See external/Mastering-Graphics-Programming-with-Vulkan/LICENSE.txt for details.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/ext.hpp>

using glm::mat4;

constexpr const int MAX_NODE_LEVEL = 16;

enum NodeFlags {
    STATIC_TRANSFORM = 1u << 1,
};


// This was lifted from https://github.com/PacktPublishing/Mastering-Graphics-Programming-with-Vulkan
// But cahnges has been made trought so its not 1:1 to the original
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

using NodeId = uint32_t;
struct Scene {

	std::vector<mat4> localTransform;  // indexed by node
	std::vector<mat4> globalTransform; // indexed by node

	std::vector<NodeId> changedAtThisFrame[MAX_NODE_LEVEL];

	std::vector<Hierarchy> hierarchy;
	std::unordered_map<NodeId, uint32_t> meshForNode;
	std::unordered_map<NodeId, uint32_t> materialForNode;
	std::unordered_map<NodeId, uint32_t> nameForNode;
	std::unordered_map<NodeId, uint32_t> lightForNode;

	std::vector<std::string> nodeNames;
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



