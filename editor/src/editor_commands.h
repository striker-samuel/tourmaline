// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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

// SceneAudioSource (the AudioSourceRow inline payload — the Delete
// capture + the duplication source need the full authored value here).
#include <Engine/Scene/scene_payloads.h>

namespace editor {

enum class EditVerb : uint8_t {
	Rename,
	Translate,
	Rotate, // degrees XYZ (inspector convention)
	Scale,
	AssignMaterial, // world material index
	SetMatAttr,     // shared material value
	SetNoLightmap,
	SetLightmap, // bake opt-in (default off; meshes opt in deliberately)
	SetDitherCloseup, // subtree dither on camera closeup
	SetVisibility,  // Clarisse-style self hidden flag (hierarchical effect)
	SetCastsVolumetricShadow, // the coarse fog-tile caster mark (self
	                          // flag, see Scene::castsVolumetricShadow)
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
  	CreateGiVolume, // append a GI volume node (world): a DAG
  	              // transform (translate = box center, scale = full
  	              // extents, axis-aligned — rotation ignored) + a
  	              // node-local SceneGiVolume definition (m_giVolumeForNode).
  	              // c.node = CREATED world id once forward-executed
  	              // (UINT32_MAX before); c.node2 = parent world id;
  	              // afterText = name; afterVec = local position;
  	              // svec = local size; priority/spacing/fades below.
  	              // Post-creation edits ride SetGiVolumeAttr (like
  	              // lights: translate/rotate/scale/rename lower to
  	              // position/rotation/size/name channels).
  	SetGiVolumeAttr, // GI volume property edit; c.node = GI volume node
  	              // (world), giVolumeAttr selects the channel (see below).
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
  	CreateAudioSource, // append an audio-source node (world): a plain
  	                  // DAG transform (translate = the emitter pose; the
  	                  // audio manager resolves the node's GLOBAL
  	                  // transform at use, so parenting applies like
  	                  // meshes) + a node-local SceneAudioSource payload
  	                  // in scene.tables.audioSources (the EnvVolumeRow
  	                  // inline-payload pattern — NO pool entry). c.node
  	                  // = CREATED world id once forward-executed
  	                  // (UINT32_MAX before); c.node2 = parent world id;
  	                  // afterText = name; afterVec = local position;
  	                  // audioDupFrom = a live node whose payload
  	                  // DuplicateSelection copies (UINT32_MAX = the
  	                  // default payload). Post-creation edits ride
  	                  // SetAudioAttr/AddAudioSound/RemoveAudioSound
  	                  // (kCreateAudioSourceRefBase addressing).
  	SetAudioAttr, // audio-source property edit; c.node = the audio
  	              // node (world), audioAttr selects the channel (see
  	              // below).
  	AddAudioSound, // sound-slot UPSERT (by slot NAME — the stable
  	               // identifier, reordering never breaks it): c.node =
  	               // the audio node (world); afterText = slot name;
  	               // number = gain; afterFlag = loop. The replaced
  	               // slot's before-state rides beforeText/beforeNum/
  	               // beforeFlag (see audioSlotHad below).
  	RemoveAudioSound, // sound-slot removal: c.node = the audio node
  	                 // (world); afterText = the slot name. The removed
  	                 // slot's value rides beforeText/beforeNum/
  	                 // beforeFlag — undo re-adds it EXACTLY.
  	CreateExportSettings, // append the scene's WorldSettings-style
  	              // EXPORT CONFIG node (world): a DAG transform node
  	              // (identity; no shape) + a node-local
  	              // SceneExportSettings payload in
  	              // scene.exportForNode. The AUTO-SEED singleton (one
  	              // per scene; not on the Create menu). c.node =
  	              // CREATED world id once forward-executed
  	              // (UINT32_MAX before); c.node2 = parent world id;
  	              // afterText = name.
  	SetExportAttr, // exportSettings payload edit; c.node = the
  	              // exportSettings node (world), exportAttr selects
  	              // the SceneExportSettings field (see below); value
  	              // rides number (scalars) / afterFlag (bools) /
  	              // afterText (strings).
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
	bool lightShadow = false; // cast-shadow request (spot tile / point cube faces)
 	// SetLightAttr channel ("color","intensity","range","inner","outer",
 	// "static","shadow","name"). Value rides vec4 (color) / number
 	// (scalars, degrees for cones) / afterFlag (static/shadow) /
 	// afterText (name); before-state rides beforeVec4 / beforeNum /
 	// beforeFlag / beforeText.
 	std::string lightAttr;
	// CreateGiVolume payload (live-world domain; box center in
	// afterVec, full-extents size in svec, name in afterText,
	// parent in node2):
	uint32_t giVolumePriority = 0; // higher = composited later = wins
	float giVolumeSpacing = 0.5f;  // dense probe grid spacing (m)
	float giVolumeInsideFade = 0.1f;  // blend width inside each face (m)
	float giVolumeOutsideFade = 0.1f; // blend width outside each face (m)
	// SetGiVolumeAttr channel ("priority","spacing","inside","outside",
	// "position","rotation","size","name"). Value rides number
	// (scalars) / vec4 (triples: position, rotation-deg, size) /
	// afterText (name); before-state rides beforeNum / beforeVec4 /
	// beforeText.
	std::string giVolumeAttr;
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
	// SetAudioAttr channel ("volume","spatial","playOnLoad",
	// "minDistance","maxDistance","name","position"). Value rides
	// number (scalars) / afterFlag (spatial/playOnLoad) / afterText
	// (name — the NODE's; the payload carries none) / vec4 (the
	// position triple); before-state rides beforeNum / beforeFlag /
	// beforeText / beforeVec4.
	std::string audioAttr;
	// Sound-slot channels (AddAudioSound/RemoveAudioSound): afterText
	// = the SLOT name (the stable identifier), audioPath = the asset
	// path (the tour's -source), number = gain, afterFlag = loop. The
	// exact-inverse before-state of the REPLACED/REMOVED slot rides
	// beforeText (assetPath), beforeNum (gain), beforeFlag (loop) +
	// audioSlotHad (true = a slot of that name existed — undo restores
	// the captured value; false = the add CREATED it, so undo REMOVES
	// it. RemoveAudioSound reuses the flag: true = a slot was erased).
	std::string audioPath;
	bool audioSlotHad = false;
	// (audioSlotHad is captured on EVERY forward execution — idempotent
	// under undo/redo: LIFO discipline guarantees the live state is
	// back to pre-command whenever forward re-runs, so the re-capture
	// reads the same values.)
	// CreateAudioSource duplication source (DuplicateSelection): when
	// != UINT32_MAX, ExecCreateAudioSource copies that node's LIVE
	// SceneAudioSource payload instead of planting the default — the
	// camera-duplicate precedent, minus the pool (the payload is
	// inline, so the copy IS the whole lens-equivalent).
	uint32_t audioDupFrom = UINT32_MAX;
	// Delete payload: the audio-source payload's presence + captured
	// value (the row drops with every other link; undo restores it
	// EXACTLY — the camera/cubemap capture precedent, minus the pool
	// index since there is none).
	bool beforeAudioSource = false;
	SceneAudioSource beforeAudio{};
	// SetExportAttr channel — a SceneExportSettings field name
	// ("meshopt","lods","lodRatios","lodError","lightmap",
	// "lmMetersPerTexel","lmLobes","lmAtlasWidth","lmRays",
	// "lmProxyRes","lmExcludeSmall","lmExcludeKw","cubemap",
	// "cubemapSize","cubemapRays","cubemapOctree","cubemapOctreeMax",
	// "cubemapOctreeMin","cubemapOctreeDepth","cubemapExtraRadius",
	// "meshlets","meshletMaxVerts","meshletMaxTris",
	// "meshletConeWeight","giVolumes","giWorld","autoKtx","ignoreVfs").
	// ("giWorld" is RETIRED — the .sggi octree bake is gone; the token
	// stays TOLERATED on the parse side so old tours load, but the
	// editor no longer authors it and ApplyExportAttr ignores it.)
	// Value rides number (scalars) / afterFlag (bools) / afterText
	// (strings); before-state rides beforeNum / beforeFlag /
	// beforeText.
	std::string exportAttr;
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
	// beforeNum) — the GI-volume def uses beforeFlag/beforeVec4; a node
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
