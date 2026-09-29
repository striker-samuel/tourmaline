#pragma once
// Editor CRUD command stack (Maya-style): every scene edit is a command
// with an exact inverse, giving unlimited undo/redo. The stack IS the
// document: saving writes commands[0..cursor] to the .tour ASCII file
// (cluster-space ids), loading replays a .tour into the stack.
//
// World-space ids drive the live viewport; cluster-space ids (stable across
// the deterministic import) go into the file. Translation happens at the
// save/load boundary only.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace editor {

enum class EditVerb : uint8_t {
	Rename,
	Translate,
	Rotate, // degrees XYZ (inspector convention)
	Scale,
	AssignMaterial, // world material index
	SetMatAttr,     // shared material value
	SetNoLightmap,
	SetDitherCloseup, // subtree dither on camera closeup
	SetVisibility,  // Clarisse-style self hidden flag (hierarchical effect)
	SetStatic,      // static-subtree ROOT mark (frozen transforms,
	                // mega-mesh collapse eligibility; whole tree)
	Parent, // world parent id
 	CreateLight, // append a light node (world); params below. c.node =
	             // CREATED world id once forward-executed (UINT32_MAX
	             // before); c.node2 = parent world id
  CreateTerrain, // append a procedural terrain GROUP node + tilesX*tilesZ
  	             // tile mesh nodes (world). Group params ride vec4/number
  	             // (tour-shaped, immutable after creation): vec4 = (size
  	             // [m], res, height[m], freq[1/m]), number = noise seed;
  	             // node2 = tilesX | (tilesZ << 16). c.node = CREATED
  	             // group world id once forward-executed; material = world
  	             // material idx; afterVec = local position (group origin
  	             // corner); afterText = name. Post-creation edits address
  	             // it via tour kCreateTerrainRefBase (CreatedTerrainIndex).
 	TerrainPaint, // one height brush stamp on a created terrain group.
 	              // c.node = group world id; terrainOp = "raise"|"lower"|
 	              // "flatten"|"smooth"; vec4 = (worldX, worldZ, radius,
 	              // flattenTarget); number = strength. Exact undo rides
 	              // the captured before-patch (terrainBefore + box).
 	SetLightAttr, // light property edit; c.node = light node (world),
 	              // lightAttr selects the channel (see below)
 	Delete,      // remove an editor node from the LIVE scene (Maya
 	              // delete): mesh/material/light links and draw entries
 	              // drop so nothing draws, picks, bakes or cooks it;
 	              // the node id joins the deleted set (outliner filters
 	              // it). NO hiding and NO renumbering — pool resources
 	              // are never reclaimed, only the links; undo restores
 	              // the exact prior state. c.node = deleted node (a
 	              // terrain GROUP cascades over its tiles); the light's
 	              // world index rides number; per-node map links ride
 	              // delNodeMaps; tile draw entries ride delDrawItems
 	              // (packed quintuples). Tour: cluster-backed nodes
 	              // persist as Verb::Delete; editor-created nodes are
 	              // dropped via the creates' live-state emission.
  	CreateTransform, // append a plain transform node (world): no mesh,
  	              // no light, no pool entry — a Maya group locator.
  	              // c.node = CREATED world id once forward-executed
  	              // (UINT32_MAX before); c.node2 = parent world id;
  	              // afterText = name; afterVec = local position;
  	              // vec4 = local rotation degrees; svec = local scale
  	              // (glm::vec3, new EditCmd field). The reference
  	              // system uses these as displacement placeholders
  	              // (see editor_references.cpp): no other authoring
  	              // creates them today.
  	CreateCVolume, // append a convex GI volume node (world): a DAG
  	              // transform (translate = box center, scale = full
  	              // extents, axis-aligned — rotation ignored) + a
  	              // node-local SceneCvol definition (m_cvolForNode).
  	              // c.node = CREATED world id once forward-executed
  	              // (UINT32_MAX before); c.node2 = parent world id;
  	              // afterText = name; afterVec = local position;
  	              // svec = local size; priority/spacing/fades below.
  	              // Post-creation edits ride SetCvolAttr (like
  	              // lights: translate/rotate/scale/rename lower to
  	              // position/rotation/size/name channels).
  	SetCvolAttr, // cvolume property edit; c.node = cvolume node
  	              // (world), cvolAttr selects the channel (see below).
  	CreateCubemap, // append a cubemap capture node (world): a pinned
  	              // specular probe position + node-local SceneCubemap
  	              // definition (m_cubemapForNode). c.node = CREATED
  	              // world id once forward-executed (UINT32_MAX
  	              // before); c.node2 = parent world id; afterText =
  	              // name; afterVec = local position; number = radius.
  	SetCubemapAttr, // cubemap capture edit; c.node = capture node
  	              // (world), cubemapAttr selects the channel
  	              // ("position","radius","name").
  	CreateCamera, // append a camera node (world): DAG transform +
  	              // lens definition in the primary cluster's cameras
  	              // pool (world cameraLocations). c.node = CREATED
  	              // world id once forward-executed (UINT32_MAX
  	              // before); c.node2 = parent world id; afterText =
  	              // name; afterVec = local position; camRot = local
  	              // rotation degrees; camProjection 0/1, camFovY,
  	              // camNear, camFar, camOrthoHalfH = lens.
  	SetCamAttr, // camera property edit; c.node = camera node (world),
  	              // camAttr selects the channel (see below).
};

struct EditCmd {
	EditVerb verb = EditVerb::Rename;
	uint32_t node = UINT32_MAX;  // world node
	uint32_t node2 = UINT32_MAX; // Parent: new parent (world);
	                             // CreateLight: parent (world)
	uint32_t material = UINT32_MAX; // AssignMaterial: new mat (world idx)
	// SetMatAttr target + value:
	uint32_t matWorld = UINT32_MAX;
	std::string attr;
	glm::vec4 vec4{ 0.0f };
	double number = 0.0;
	// After-state (transforms as triple; rename text):
	std::string afterText;
	glm::vec3 afterVec{ 0.0f };
	// CreateTransform local rotation (degrees) + scale. Also reused by
	// ... nothing else: dedicated so transform creates stay exact.
	glm::vec3 svec{ 1.0f, 1.0f, 1.0f };
	// Before-state (exact inverse):
	std::string beforeText;
	glm::mat4 beforeMat{ 1.0f }; // transforms: full local matrix
	glm::vec4 beforeVec4{ 0.0f };
	double beforeNum = 0.0;
	bool beforeFlag = false, afterFlag = false;
	uint32_t beforeMaterial = UINT32_MAX; // AssignMaterial undo (world idx)
	uint32_t beforeParent = UINT32_MAX;   // Parent undo (world id)
	// CreateLight payload (live-world domain: type 1 = point, 2 = spot;
	// cone angles in DEGREES, editor convention; position in afterVec;
	// name in afterText; parent in node2):
	uint32_t lightType = 0;
	glm::vec3 lightColor{ 1.0f, 1.0f, 1.0f };
	float lightIntensity = 0.0f;
	float lightRange = 0.0f; // 0 = infinite
	float lightInnerDeg = 0.0f;
	float lightOuterDeg = 0.0f;
	bool lightStatic = false; // bake-only (kSceneLightFlag_Static)
	bool lightShadow = false; // spot shadow request (spots only)
 	// SetLightAttr channel ("color","intensity","range","inner","outer",
 	// "static","shadow","name"). Value rides vec4 (color) / number
 	// (scalars, degrees for cones) / afterFlag (static/shadow) /
 	// afterText (name); before-state rides beforeVec4 / beforeNum /
 	// beforeFlag / beforeText.
 	std::string lightAttr;
	// CreateCVolume payload (live-world domain; box center in
	// afterVec, full-extents size in svec, name in afterText,
	// parent in node2):
	uint32_t cvolPriority = 0; // higher = composited later = wins
	float cvolSpacing = 0.5f;  // dense probe grid spacing (m)
	float cvolInsideFade = 0.1f;  // blend width inside each face (m)
	float cvolOutsideFade = 0.1f; // blend width outside each face (m)
	// SetCvolAttr channel ("priority","spacing","inside","outside",
	// "position","rotation","size","name"). Value rides number
	// (scalars) / vec4 (triples: position, rotation-deg, size) /
	// afterText (name); before-state rides beforeNum / beforeVec4 /
	// beforeText.
	std::string cvolAttr;
	// CreateCubemap payload (live-world domain; pinned probe position
	// in afterVec, name in afterText, parent in node2):
	float cubemapRadius = 1.5f; // influence radius (m), fade = r/2
	// SetCubemapAttr channel ("position","radius","name"). Value rides
	// number (radius) / vec4 (position triple) / afterText (name);
	// before-state rides beforeNum / beforeVec4 / beforeText.
	std::string cubemapAttr;
	// CreateCamera payload (live-world domain; pose in afterVec/camRot,
	// name in afterText, parent in node2):
	glm::vec3 camRot{ 0.0f }; // local rotation degrees (editor convention)
	uint32_t camProjection = 0; // 0 = perspective, 1 = orthographic
	float camFovY = 50.0f;      // perspective vertical fov (deg)
	float camNear = 0.1f;
	float camFar = 1000.0f;
	float camOrthoHalfH = 10.0f; // ortho half-height (m)
	// SetCamAttr channel ("projection","fovY","orthoHalfH","near",
	// "far","name","position","rotation"). Value rides number (scalars,
	// projection 0/1) / vec4 (position/rotation triples, degrees for
	// rotation) / afterText (name); before-state rides beforeNum /
	// beforeVec4 / beforeText.
	std::string camAttr;
	// TerrainPaint payload: op name above; before-patch = the pre-stamp
	// sample box (row-major, x-major within rows), captured at first
	// forward execution and reused by undo. Box coords are inclusive
	// global-grid sample indices.
	std::string terrainOp;
	std::vector<float> terrainBefore;
	uint32_t terrainBoxX0 = 0, terrainBoxZ0 = 0;
	uint32_t terrainBoxW = 0, terrainBoxH = 0;
	// Delete payload: per-node scene-map links captured at first forward
	// execution — (node, hasMesh, mesh, hasMat, mat) quintuples (hasMesh/
	// hasMat 0/1; erased links read 0). Restored exactly by undo.
	std::vector<uint32_t> delNodeMaps;
	// Delete payload: cubemap capture def presence (radius in
	// beforeNum) — the cvol def uses beforeFlag/beforeVec4; a node
	// carries at most one of the two kinds.
	bool beforeCubemap = false;
	// Delete payload: camera def presence (world camera index in
	// beforeCamIdx); the pool entry stays alive like lights (links
	// only — restored exactly by undo).
	bool beforeCamera = false;
	double beforeCamIdx = -1.0;
	// Delete payload: captured tile draw entries as packed quintuples
	// (position, node, mesh, material, flags) — recorded at first
	// forward execution, reused by redos, restored exactly by undo.
	std::vector<uint32_t> delDrawItems;
};

class EditorApp; // command execution lives on the app (world access).

} // namespace editor
