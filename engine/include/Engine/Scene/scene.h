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
// Cooked punctual light (KHR_lights_punctual import). Definitions live in
// the owning Cluster's lights pool (like meshes); the definition itself is
// node-local (color/intensity/range/cones) and world position/direction
// resolve from the node's global transform at use time so coordFix and
// parent transforms apply exactly like meshes.
struct SceneLight {
	enum class Type : uint32_t { Directional = 0, Point = 1, Spot = 2 };
	// Static-light flag (authored via glTF extras "smvk.staticLight" on
	// the light node, or a "STATIC_" light-name prefix; see gltf.cpp).
	// A static light is a BAKE source: PakCooker/GiBaker harvest it into
	// the lightmap/probe light set (CollectStaticBakeLights) and the
	// runtime forward+ set EXCLUDES it — its direct + bounced
	// contribution is carried by the baked sources for covered pixels
	// (same contract as the CLI bake spots; rendering it at runtime too
	// would double the direct term). Directionals: the first static one
	// also seeds the bake sun.
	static constexpr uint32_t kSceneLightFlag_Static = 1 << 0;
	// Cast-shadow request (authored in the editor light inspector; spots
	// ONLY — the spot-shadow atlas has no point-light path, points ignore
	// it). The runtime forward+ set honors it when assigning the
	// kMaxSpotShadows tiles (kForwardPlusFlag_CastShadow). Imported
	// (glTF/pak) lights never carry it — cooked lights never cast today —
	// so existing content is unaffected; editor-created spots default it
	// on for the useful case.
	static constexpr uint32_t kSceneLightFlag_CastShadow = 1 << 1;
	Type type = Type::Point;
	glm::vec3 color{ 1.0f, 1.0f, 1.0f };
	// Units match the runtime shaders (see Renderer/light.h):
	// directional = lux, point/spot = candela.
	float intensity = 0.0f;
	float range = 0.0f; // cutoff distance in meters; 0 = infinite
	float innerConeAngle = 0.0f; // radians, spot only
	float outerConeAngle = 0.0f; // radians, spot only
	uint32_t flags = 0; // kSceneLightFlag_*
	std::string name;
};

struct Scene {

	std::vector<mat4> localTransform;  // indexed by node
	std::vector<mat4> globalTransform; // indexed by node

	std::vector<NodeId> changedAtThisFrame[MAX_NODE_LEVEL];

	std::vector<Hierarchy> hierarchy;
	std::unordered_map<NodeId, uint32_t> meshForNode;
	std::unordered_map<NodeId, uint32_t> materialForNode;
	std::unordered_map<NodeId, uint32_t> nameForNode;
	// Node -> Cluster light pool index (cooked KHR_lights_punctual). The
	// definition lives in the owning Cluster's lights pool — the same
	// indirection meshForNode uses — and the world transform resolves
	// from globalTransform[node] at use time.
	std::unordered_map<NodeId, uint32_t> lightForNode;

	std::vector<std::string> nodeNames;
	std::vector<std::string> materialNames;

	// Static-subtree specification. staticRoot[node] == 1 marks the root of
	// a subtree whose transforms are frozen after the initial build: those
	// nodes skip per-frame transform updates and BVH reconstruction, and are
	// eligible for collapse into a single mega-mesh block (see
	// Scene/static_merge.h). staticNode[] is the derived cache (1 = inside
	// any static subtree); rebuilt via RebuildStaticCache().
	std::vector<uint8_t> staticRoot;
	std::vector<uint8_t> staticNode;

	// Lightmap opt-out (editor + cook): noLightmap[node] == 1 excludes the
	// node's subtree from lightmap charting/baking (see PakCooker --lightmap
	// and the glTF "smvk.noLightmap" node extras). Excluded geometry still
	// renders (probe/sky-lit fallback) and still occludes/casts in the bake
	// trace scene. Empty or size-mismatched == no exclusions. Carried through
	// merge/serialize paths alongside staticRoot.
	std::vector<uint8_t> noLightmap;
	// Dither on camera closeup (editor + cook): ditherCloseup[node] == 1
	// marks the node's subtree for a dithered fade when the camera gets
	// close (LOD-pop hiding for impostors/foliage). Subtree-inherited like
	// noLightmap. Stored, cooked and replayed end to end; the GPU dither
	// itself is TODO (no per-node channel in the draw ring yet — the
	// distance term will ride the per-draw data when that lands).
	// Empty or size-mismatched == off. Carried through merge/serialize
	// paths alongside staticRoot.
	std::vector<uint8_t> ditherCloseup;
	// Visibility override (Clarisse-style, editor + cook): hidden[node] == 1
	// hides the node's subtree. The flag is SELF state; the EFFECTIVE
	// state is hierarchical — a node is effectively visible only when
	// neither it nor any ancestor is marked hidden (see IsVisibleNode).
	// Source default is visible (all-zero); shot overrides set hidden.
	// Empty or size-mismatched == all visible. Carried through
	// merge/serialize/world-resolve paths alongside noLightmap.
	std::vector<uint8_t> hidden;
};

int addNode(Scene& scene, int parent, int level);

void markAsChanged(Scene& scene, int node);
// Dynamic-only variant: skips static nodes so frozen subtrees never enter
// changedAtThisFrame after the initial build. Prefer this on the per-frame
// path; use markAsChanged for initial builds / collapse baking.
void markAsChangedDynamic(Scene& scene, int node);

inline bool IsStaticNode(const Scene& scene, int node)
{
	return node >= 0 && (size_t)node < scene.staticNode.size() && scene.staticNode[(size_t)node] != 0;
}

// Lightmap opt-out for a node or any ancestor (subtree exclusion).
inline bool IsNoLightmapNode(const Scene& scene, int node)
{
	for (int n = node; n >= 0 && n < int(scene.hierarchy.size()); ) {
		if ((size_t)n < scene.noLightmap.size() && scene.noLightmap[(size_t)n] != 0)
			return true;
		n = scene.hierarchy[size_t(n)].parent;
	}
	return false;
}

// Dither-on-closeup for a node or any ancestor (subtree flag).
inline bool IsDitherCloseupNode(const Scene& scene, int node)
{
	for (int n = node; n >= 0 && n < int(scene.hierarchy.size()); ) {
		if ((size_t)n < scene.ditherCloseup.size() && scene.ditherCloseup[(size_t)n] != 0)
			return true;
		n = scene.hierarchy[size_t(n)].parent;
	}
	return false;
}

// Visibility (Clarisse-style hierarchical override): true when the node
// itself is marked hidden. Effective visibility walks ancestors — a
// hidden parent hides the whole subtree (see IsVisibleNode).
inline bool IsHiddenNode(const Scene& scene, int node)
{
	return node >= 0 && (size_t)node < scene.hidden.size() &&
		scene.hidden[(size_t)node] != 0;
}

// Effective visibility: visible only when neither the node nor any
// ancestor carries the hidden flag. Size-mismatched/empty == visible.
inline bool IsVisibleNode(const Scene& scene, int node)
{
	for (int n = node; n >= 0 && n < int(scene.hierarchy.size()); ) {
		if ((size_t)n < scene.hidden.size() && scene.hidden[(size_t)n] != 0)
			return false;
		n = scene.hierarchy[size_t(n)].parent;
	}
	return true;
}

// Marks node hidden (self flag; subtree effect via IsVisibleNode).
// Grows the array if needed.
inline void MarkHidden(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.hidden.size() != scene.hierarchy.size())
		scene.hidden.assign(scene.hierarchy.size(), 0);
	scene.hidden[(size_t)node] = 1;
}
inline void ClearHidden(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.hidden.size() != scene.hierarchy.size())
		scene.hidden.assign(scene.hierarchy.size(), 0);
	scene.hidden[(size_t)node] = 0;
}

// Marks node for dither-on-closeup (subtree). Grows the array if needed.
inline void MarkDitherCloseup(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.ditherCloseup.size() != scene.hierarchy.size())
		scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
	scene.ditherCloseup[(size_t)node] = 1;
}
inline void ClearDitherCloseup(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.ditherCloseup.size() != scene.hierarchy.size())
		scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
	scene.ditherCloseup[(size_t)node] = 0;
}

// Marks node as lightmap-excluded (subtree). Grows the array if needed.
inline void MarkNoLightmap(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.noLightmap.size() != scene.hierarchy.size())
		scene.noLightmap.assign(scene.hierarchy.size(), 0);
	scene.noLightmap[(size_t)node] = 1;
}
inline void ClearNoLightmap(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.noLightmap.size() != scene.hierarchy.size())
		scene.noLightmap.assign(scene.hierarchy.size(), 0);
	scene.noLightmap[(size_t)node] = 0;
}

inline bool IsStaticSubtreeRoot(const Scene& scene, int node)
{
	return node >= 0 && (size_t)node < scene.staticRoot.size() && scene.staticRoot[(size_t)node] != 0;
}

// Marks node as a static-subtree root and rebuilds the staticNode cache.
void MarkSubtreeStatic(Scene& scene, int node);
// Clears the root mark and rebuilds the cache.
void ClearSubtreeStatic(Scene& scene, int node);
// Re-derives staticNode[] from staticRoot[] via hierarchy walk.
void RebuildStaticCache(Scene& scene);

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
// Dynamic-only variant: recomputes globals for non-static changed nodes.
// Static nodes keep their baked globals (already correct from the initial
// full build). Returns true if any dynamic node was updated.
bool recalculateGlobalTransformsDynamic(Scene& scene);

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



