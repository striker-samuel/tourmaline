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

// Authored camera (editor camera node / tour createCamera): a DAG
// transform (position + orientation, engine space) + a node-local lens
// definition. The lens lives in the owning Cluster's cameras pool when
// pool-backed (cook path, like SceneLight) or in the editor world's
// cameraLocations; the pose always resolves from the node's global
// transform at use time. Forward = node -Z in world.
struct SceneCamera {
	enum class Projection : uint32_t { Perspective = 0, Orthographic = 1 };
	Projection projection = Projection::Perspective;
	float fovYDeg = 50.0f;     // perspective vertical field of view
	float orthoHalfH = 10.0f;  // orthographic half-height (meters)
	float nearClip = 0.1f;
	float farClip = 1000.0f;
	std::string name;
};

// Authored convex GI volume (editor cvolume node / tour createCvolume):
// a box-shaped dense-probe region (gi_baker box-spec shape). The SHAPE
// rides the node's transform (translate = box center, scale = full
// extents, ROTATION = the box axes — an OBB: the bake rotates the six
// box planes, CvBoxPlanesRotated); this carries the bake params.
// Node-local like SceneLight (no pool); resolved against
// globalTransform[node] at use so parenting applies like meshes.
struct SceneCvol {
	uint32_t priority = 0;   // higher = composited later = wins
	float spacing = 0.5f;    // dense probe grid spacing (m)
	float insideFade = 0.1f; // blend width inside each face (m)
	float outsideFade = 0.1f;// blend width outside each face (m)
};

// Authored cubemap capture (editor cubemap node / tour createCubemap):
// an artist-PINNED specular probe position. The POSITION rides the
// node's transform (translate, world-resolved); this carries the
// influence radius. Baked by PakCooker's --cubemap stage as a 'pin'
// row (sorted first at load — a pinned probe wins weight ties, e.g.
// the capture exactly at a chrome ball). Node-local like SceneCvol.
struct SceneCubemap {
	float radius = 1.5f;   // influence radius (m); fade = radius/2
};

// Authored environment block (editor LIGHTING authoring): LUX-NATIVE
// intensities — sun 10000 lux / sky 600 default, the same units the
// runtime EnvironmentSettings and every bake (GI/lightmap/cubemap) use,
// so ONE authored value drives the viewport preview, the pak sun, the
// bakes and the App with no conversions (each consumer maps to its own
// needs at use: the editor's LDR preview scales down, the HDR paths
// take lux raw). No legacy migration: tours authored before the lux
// switch carry scene-unit values that read dark — re-author them.
// Exposure 0.1..4, fog 0..1. Scene-attached like the other authored
// node data below (cvol/cubemap): the resolved ACTIVE look lives in
// Scene::environment; the default block + per-node alternatives live
// beside it; the "which block renders" selection stays editor view
// state (like the active camera) and is resolved into Scene::environment.
struct SceneEnv {
	glm::vec3 skyColor{ 0.35f, 0.5f, 0.7f };
	float skyIntensity = 600.0f;   // lux-scale (the runtime sky default)
	glm::vec3 sunColor{ 1.0f, 0.96f, 0.92f };
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f; // lux (the runtime sun default)
	float exposure = 1.0f;
	// Fog quality tiers (authored; the App renders the selected one):
	//   0 = off (no fog at all)
	//   1 = analytic (exp2 distance fog — the cheap fallback tier)
	//   2 = volumetric (the raymarched GI-lit pass — the default; falls
	//       back to the analytic tier at runtime when the pass cannot
	//       run — pass off/density 0/target missing — so fog never
	//       silently disappears)
	int fogQuality = 2;
	// Analytic tier settings (fogMode 1 = exp2 in the frame env).
	float fogDensity = 0.0f;
	glm::vec3 fogColor{ 0.6f, 0.7f, 0.8f };
	// Volumetric fog (the App's raymarched pass — Engine/Rendergraph/
	// volumetric.h): the SAME settings the App's panel exposes, authored
	// here and saved forward tour -> pak -> App (the App seeds its pass
	// state from the resolved env once per scene load; the panel stays
	// live afterwards). volumetrics is the master: 0 = off, else the
	// scattering intensity (the App's intensity slider, 0..4).
	float volumetrics = 0.0f;            // scattering intensity (0 = off)
	uint32_t volumetricSteps = 16;       // march steps 4..128
	float volumetricMaxDistance = 80.0f; // ray clamp (m)
	float volumetricDensity = 0.015f;    // extinction (1/m)
	float volumetricAnisotropy = 0.4f;   // HG g, -0.9..0.9 (forward+)
	float volumetricSun = 1.0f;          // sun single-scatter strength
	float volumetricGi = 1.0f;           // probe ambient strength
	bool volumetricUseGi = true;         // sample SG-GI probes (else sky IBL)
	bool volumetricUseSun = true;        // analytic sun scatter
	float volumetricJitter = 1.0f;       // IGN dither 0..1 (banding guard)
	float volumetricHeightFalloff = 0.0f; // height falloff (1/m, 0 = off)
	float volumetricHeightBase = 0.0f;    // reference height (m)
	std::string skyboxUri;     // stored (skybox sampling is TODO)
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
	// Node -> Cluster camera pool index (authored camera node). The lens
	// definition lives in the owning Cluster's cameras pool; the pose
	// resolves from globalTransform[node] at use time (viewport
	// look-through, framing). Forward = node -Z in world, engine space.
	std::unordered_map<NodeId, uint32_t> cameraForNode;
	// Node -> authored convex GI volume params (tour createCvolume /
	// editor cvolume node). The definition is node-local (a few floats
	// — no pool indirection); the box shape rides the node's transform.
	// Recorded by tour replay; harvested as bake inputs by GiBaker
	// (loose sidecars, on demand) AND PakCooker's --cvolume stage
	// (baked + embedded into the pak for the runtime App).
	std::unordered_map<NodeId, SceneCvol> cvolForNode;
	// Node -> authored cubemap capture params (tour createCubemap /
	// editor cubemap node): pinned specular probe positions harvested
	// by the cook's --cubemap stage. Position = node translate.
	std::unordered_map<NodeId, SceneCubemap> cubemapForNode;
	// Authored environment (see SceneEnv): the resolved ACTIVE look
	// (authoring units) — what renders. The editor resolves its current
	// source (default block or selected node block) into `environment`
	// on every change; the cook resolves the tour's active block into
	// the cluster scene; world/pak assembly keeps the first authored
	// one (see hasEnvironment). Consumers read this and NOTHING else.
	SceneEnv environment;
	// True when `environment` was authored (tour block / editor publish /
	// cook resolve) rather than default-constructed. Gates first-wins
	// resolution across merged scenes: an unauthored default must never
	// shadow another scene's authored look.
	bool hasEnvironment = false;
	// Authoring default block (the "Editor default" source). The tour
	// persists it; the editor resolves it into `environment` when no
	// node block is selected.
	SceneEnv defaultEnvironment;
	// Node -> authored environment block (tour env.n / editor env node).
	// The definition is node-local like SceneCvol; the active block is
	// resolved into `environment` by whoever owns the selection (the
	// editor; the cook at bake time).
	std::unordered_map<NodeId, SceneEnv> envForNode;
	// DEFAULT CAMERA (world settings' typed camera field, tour
	// editorSettings camera.default -> cook -> pak v10): the authored
	// camera node the runtime App spawns at/looks through instead of the
	// generic bounds framing (editors orbit freely; the App honors the
	// author's framing). Cluster-space in cooked paks, world-space in
	// the live World scene (ResolveAndFixup remaps). UINT32_MAX = none
	// (generic framing stands). Not to be confused with the per-viewport
	// editor look-through binding (view state, never persisted).
	NodeId defaultCameraNode = UINT32_MAX;
	// Node -> file-stable identity (32-hex tour UUID). Assigned by the
	// editor for nodes a Delete line addresses (persisted as `uuid`
	// lines ahead of the deletes) and recorded back by replay and
	// cook: deletes resolve through it, stable across re-imports that
	// shift numeric ids. Empty = the node has none.
	std::unordered_map<NodeId, std::string> uuidForNode;

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



