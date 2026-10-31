// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Portions Copyright (c) 2022 Packt
// Modified by Samuel Martin, 2026
// Licensed under the MIT License (the scene format derives from "Mastering
// Graphics Programming with Vulkan", Packt 2022). See the third-party
// notice in LICENSE for details.

#pragma once

#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/ext.hpp>

#include <Engine/Scene/scene_node_id.h>
#include <Engine/Scene/scene_tables.h>
#include <Engine/Scene/scene_payloads.h>

namespace tour::scene {

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

// NodeId: engine/include/Engine/Scene/scene_node_id.h (included above)
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
	// Cast-shadow request (authored in the editor light inspector). SPOTS
	// get one perspective tile in the local-light shadow atlas; POINTS get
	// six cube-face tiles (see spotshadow.h + shadows.hlsli's
	// GetPointShadowFactorTile). The runtime forward+ set honors it when
	// assigning the atlas tiles (kForwardPlusFlag_CastShadow). Imported
	// (glTF/pak) lights never carry it — cooked lights never cast today —
	// so existing content is unaffected; editor-created spots/points
	// default it on for the useful case.
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
	// Spot COOKIE (gobo; see the spot shading path): a texture asset ref
	// (a .texture file path or a plain image URI — the cluster pool
	// texture it names is projected through the cone; the light build
	// resolves it to the bindless index). Spots only; points ignore it.
	// Empty = none. Authored via setLightAttr cookie.
	std::string cookieUri;
	// Procedural light FUNCTION (per-pixel intensity modulation, see
	// kForwardPlusFunction_* + light.hlsli EvaluateLightFunction):
	// 0 none, 1 flicker (stepped candle-like time noise), 2 pulse
	// (sine breathing), 3 bands (moving diagonal bands across the spot
	// cone; points fall back to pulse). Speed scales time, scale is the
	// modulation depth (1 = dips to black, 0 = flat). Applies to points
	// and spots alike (the shader picks the spatial form for spots).
	uint32_t functionType = 0;
	float functionSpeed = 1.0f;
	float functionScale = 0.5f;
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

// Authored/runtime decal (tiled projected splat; see smvk::ForwardPlusDecal
// + forwardplus.cs.hlsl). Pool asset like SceneLight: the definition lives
// in the owning Cluster's decals pool, the SHAPE rides the node's transform
// — translation = projector center, rotation = projection frame (local -Z
// points INTO the receiving surface, local +Y is the texture V direction),
// scale = half extents (x, y in-plane, z depth reach). This struct carries
// the LOOK: cluster-pool texture refs (resolved to bindless at use like
// light cookies) + material knobs.
struct SceneDecal {
	// Texture refs (cluster texture pool URIs — a .texture asset path or
	// image URI). Albedo: RGBA, rgb linear color + a coverage mask.
	// Normal: tangent-space xy. Empty = channel off.
	std::string albedoUri;
	std::string normalUri;
	glm::vec3 tint{ 1.0f };   // linear multiplier on the albedo texture
	float opacity = 1.0f;
	float normalStrength = 0.85f;  // decal normal-map strength
	float roughnessTarget = 0.14f; // wet-glossy roughness target
	float metalTarget = 0.0f;
	float depthFadeStart = 0.6f;   // |local.z| fade start (box-face pop guard)
	uint32_t seed = 0;             // in-plane yaw RNG (Pcg32)
	std::string name;
};

// Default blood-splat texture URIs: the app host's demo content registers
// these into the dynamic cluster's texture pool (see app/src/DecalAssets);
// the renderer's J crosshair spawn references the same contract. Shared
// constants so the two sides cannot drift. (char arrays: both StringViewT
// and std::string construct from them directly.)
inline constexpr char kDecalBloodAlbedoUri[] = "decals/blood_albedo";
inline constexpr char kDecalBloodNormalUri[] = "decals/blood_normal";

// Builds a decal node's LOCAL transform from a placement frame: the
// node's local -Z = axisZ (the projection axis, pointing INTO the
// receiving surface), local +Y = upHint (the texture V direction),
// scale = half extents (x, y in-plane, z depth reach) — see SceneDecal.
// The up hint is orthogonalized against the frame's Z with a safe
// fallback (floor splats pass world up with axisZ = -Y).
inline glm::mat4 DecalNodeFrame(const glm::vec3& center, glm::vec3 axisZ,
	glm::vec3 upHint, const glm::vec3& half) {
	axisZ = glm::normalize(axisZ);
	const glm::vec3 z = -axisZ; // node +Z faces back toward the projector
	glm::vec3 y = upHint - z * glm::dot(upHint, z);
	if (glm::length(y) < 1e-4f) {
		y = glm::vec3(0.0f, 1.0f, 0.0f) - z * z.y;
		if (glm::length(y) < 1e-4f)
			y = glm::vec3(1.0f, 0.0f, 0.0f);
	}
	y = glm::normalize(y);
	const glm::vec3 x = glm::normalize(glm::cross(y, z));
	y = glm::normalize(glm::cross(z, x));
	glm::mat4 m(1.0f);
	m[0] = glm::vec4(x * half.x, 0.0f);
	m[1] = glm::vec4(y * half.y, 0.0f);
	m[2] = glm::vec4(z * half.z, 0.0f);
	m[3] = glm::vec4(center, 1.0f);
	return m;
}

// RENDER SETTINGS (the postfx LOOK): the render-grade knobs — bloom,
// tonemap, exposure, AO, SSR, motion blur, DOF, AA, cubemap IBL. Split
// out of SceneEnv (they rode the env block historically): the look is a
// GLOBAL scene grade, never zoned — env nodes/zone blocks own sky/sun/
// fog/volumetrics ONLY, this struct owns what the App's panels expose.
// Authored once per scene (Scene::defaultRenderSettings), resolved into
// Scene::renderSettings, saved forward tour (render.* keys) -> pak ->
// App (the renderer seeds its pass state from it once per scene load;
// panels/keys stay live afterwards). Headless TOURMALINE_* pins win over
// the seed (see ApplyHeadlessPostfxOverrides).
struct SceneRenderSettings {
	// Bloom (complete: enable + add scale + pre-exposure threshold; the
	// knee derives at 0.25x threshold, as the baker always assumed).
	bool bloomEnabled = true;
	float bloomIntensity = 0.6f;   // tonemap bloom-add scale
	float bloomThreshold = 800.0f; // pre-exposure HDR trigger
	// Tonemap operator: 0=Reinhard, 1=ACES, 2=Uncharted2, 3=GT7.
	uint32_t tonemapMode = 3;
	// Exposure (histogram meter + eye adaptation): auto flag, manual
	// exposure when auto is off, EV trim, adapt rates, absolute clamps.
	bool autoExposure = true;
	float manualExposure = 0.001f;
	float exposureEvBias = 0.0f;      // -4..+4
	float exposureSpeedToDark = 1.0f;
	float exposureSpeedToBright = 3.0f;
	float exposureMin = 0.00001f;
	float exposureMax = 0.05f;
	// AO (SSGI post chain): 0=None, 1=SSAO, 2=SAO + the shared knobs.
	int aoMode = 2;
	float aoRadius = 0.5f;
	float aoBias = 0.025f;
	float aoIntensity = 1.0f;
	float aoMaxDistance = 25.0f;
	int aoSsaoSamples = 16;
	float aoBlurSharpness = 8.0f;
	int aoSaoSamples = 11;
	int aoSaoSpiralTurns = 7;
	// SSR (SSGI reflections) + motion blur + DOF + AA + cubemap IBL.
	bool ssrEnabled = true;
	float ssrIntensity = 1.0f;
	bool motionBlurEnabled = true;
	bool dofEnabled = false;
	int aaMode = 3; // 0=Off, 1=Msaa2x, 2=Smaa1x, 3=SmaaT2x
	bool cubemapEnabled = true;
	float cubemapIntensity = 1.0f;
	// (The GI/shadow renderer knobs — the ambient source, the per-source
	// opt-ins, probe pre-mix, EVSM — are NOT here: they are QUALITY
	// settings, host-owned session state, never scene content; see
	// WorldRenderer.h's RenderQualityConfig.)
};

// Render-settings record (see saveRenderFloats/loadRenderFloats):
// 27 floats, the look block's fixed order (bloom 3 + tonemap 1 +
// exposure 7 + AO 9 + SSR 2 + motionblur/dof/aa 3 + cubemap 2).
// Booleans are stored as 0/1 floats; enum values are stored numerically.
inline constexpr int kSceneRenderFloatCount = 27;

struct Scene {

	std::vector<mat4> localTransform;  // indexed by node
	std::vector<mat4> globalTransform; // indexed by node

	std::vector<NodeId> changedAtThisFrame[MAX_NODE_LEVEL];

	std::vector<Hierarchy> hierarchy;

	// ---- stable node slots (Epoch 6b, docs/scene_tables_migration.md) ----
	// The hierarchy/transform/flag/metadata vectors above are SLOT
	// arrays: deletion retires a slot (free list + generation bump)
	// instead of compacting, so every surviving node's id stays put.
	// nodeGeneration: one word per slot — bit 31 = alive, low 12 bits
	// = the slot's generation (see scene_node_id.h). It may LAG
	// hierarchy.size(): a slot at or beyond its size was created by a
	// wholesale resize (loaders, hand-built rigs) and reads as LIVE —
	// only RetireNodeSlot ever marks dead, and it sizes the array
	// first. nodeFreeList: LIFO of retired slots; addNode pops it
	// before appending (a fresh scene allocates 0,1,2,... exactly like
	// the old dense ids).
	std::vector<uint32_t> nodeGeneration;
	std::vector<uint32_t> nodeFreeList;

	// ---- typed association tables (the xForNode migration target,
	// Epoch 3+, docs/scene_tables_migration.md) ----
	// AUTHORITATIVE for EVERY node association since Epoch 5: the old
	// xForNode maps are GONE — every consumer reads scene.tables
	// (meshes/materials/lights/cameras/decals/giVolumes/cubemaps/
	// exports/envs/envVolumes) through scene_tables.h.
	SceneTables tables;
	// Derived per-node index over `tables` — maintained by the
	// scene_tables.h mutators, rebuilt by RebuildNodeRows, never
	// serialized, never a second source of truth.
	std::vector<NodeRows> rowsForNode;
	// Dense per-node identity metadata (name index + uuid; replaced
	// nameForNode + uuidForNode — see NodeMetadata in scene_tables.h).
	std::vector<NodeMetadata> metadata;

	std::vector<std::string> nodeNames;
	std::vector<std::string> materialNames;
	// The scene's WORLD-FIELD GI binding (tour giAsset kind World): the
	// baked probe-octree artifact this scene uses. Empty = never baked
	// (the runtime sky-IBL ambient + the editor's honest "no baked
	// volume" warnings). Same convention as SceneGiVolume::bakeSource
	// (RANDOM MINTED VFS path — the record is the identity — + the
	// artifact's .meta GUID when known). giWorldInputHash: the bake-input
	// hash at bake time (the pak gate's freshness check).
	std::string giWorldSource;
	std::string giWorldGuid;
	std::string giWorldInputHash;
	// The scene's CUBEMAP CAPTURE-SET binding (tour giAsset kind
	// Captures): the record-bound cubemanifest (+ its ktx2 rows + the
	// .reach bookkeeping named from the same uuid). Empty = never baked
	// (the runtime analytic-sky specular fallback). giCapturesHash: the
	// set-level fold of the per-row bake-input hashes (the gate's fast
	// path; the rows carry the per-capture hashes).
	std::string giCapturesSource;
	std::string giCapturesGuid;
	std::string giCapturesHash;
	// Explicit ANIMATION references (the self-contained anim pipeline):
	// the VFS paths of the .tourAnim clips this scene's content was
	// imported with (the gltf.meta manifest's anim.<idx> rows, stamped by
	// the pak cooker alongside the embedded clip blobs). The runtime
	// resolves them through the content VFS — the mounted pak backend
	// serves the embedded bytes, so a pak alone carries everything a
	// skinned cluster needs (the old runtime discovery enumerated the
	// loose demo/ dir — a pak-dependent-on-loose-files hole). Empty =
	// no recorded clips (old paks fall back to the legacy paths).
	std::vector<std::string> animClipRefs;
	// Authored environment (see SceneEnv): the resolved ACTIVE look
	// (authoring units) — what renders. The editor resolves its current
	// source (default block or selected node block) into `environment`
	// on every change; the cook resolves the tour's active block into
	// the cluster scene; world/pak assembly keeps the first authored
	// one (see envAuthored). Consumers read this and NOTHING else.
	SceneEnv environment;
	// Provenance of `environment` (NOT content state): true when the
	// active look was authored (tour block / editor publish / cook
	// resolve) rather than default-constructed. Gates first-wins
	// resolution across merged scenes: an unauthored default must never
	// shadow another scene's authored look. Serialized as a u32 flag.
	bool envAuthored = false;
	// Authoring default block (the "Editor default" source). The tour
	// persists it; the editor resolves it into `environment` when no
	// node block is selected.
	SceneEnv defaultEnvironment;
	// Scene-level master switch for analytic environment zones (see
	// Rendergraph/envzones.h). Default ON: zones activate automatically
	// whenever the env table has rows; authors can turn the whole system
	// off (tour env.zonesEnabled -> pak v15 -> runtime). The renderer's
	// debug-panel toggle is a separate, non-persisted override.
	bool envZonesEnabled = true;
	// RENDER SETTINGS (see SceneRenderSettings): the resolved GLOBAL
	// look — what the renderer seeds from. Like `environment`, consumers
	// read `renderSettings` and nothing else; the editor resolves the
	// authored default into it (no per-node render blocks by design:
	// the look is a scene grade, never zoned).
	SceneRenderSettings renderSettings;
	// True when `renderSettings` was authored (tour render.* block /
	// editor publish) rather than default-constructed.
	bool hasRenderSettings = false;
	// Authoring default (the editor's Render settings panel edits this;
	// the tour persists it as render.* keys).
	SceneRenderSettings defaultRenderSettings;
	// DEFAULT CAMERA (world settings' typed camera field, tour
	// editorSettings camera.default -> cook -> pak v10): the authored
	// camera node the runtime App spawns at/looks through instead of the
	// generic bounds framing (editors orbit freely; the App honors the
	// author's framing). Cluster-space in cooked paks, world-space in
	// the live World scene (ResolveAndFixup remaps). UINT32_MAX = none
	// (generic framing stands). Not to be confused with the per-viewport
	// editor look-through binding (view state, never persisted).
	NodeId defaultCameraNode = UINT32_MAX;
	// ACTIVE ENVIRONMENT (tour editorSettings env.current -> cook -> pak
	// v12): the selected env block node. The UI edits the live world's
	// resolved environment directly; scenes may carry MANY env blocks
	// (envForNode) and this id says which one is active. Cluster-space in
	// cooked paks, world-space in the live World scene (ResolveAndFixup
	// remaps). UINT32_MAX = none (the default block renders). First valid
	// id across merged scenes wins (first scene loaded into the world
	// wins); the merged look resolves from it when present.
	NodeId activeEnvironment = UINT32_MAX;

	// Static-subtree specification. staticRoot[node] == 1 marks the root of
	// a subtree whose transforms are frozen after the initial build: those
	// nodes skip per-frame transform updates and BVH reconstruction, and are
	// eligible for collapse into a single mega-mesh block (see
	// Scene/static_merge.h). staticNode[] is the derived cache (1 = inside
	// any static subtree); rebuilt via RebuildStaticCache().
	std::vector<uint8_t> staticRoot;
	std::vector<uint8_t> staticNode;
	// True when this scene was DESERIALIZED from a blob that carried the
	// trailing static-subtree section (the serializer's v2+ layout — the
	// section is ALWAYS written, so a deserialized scene has it and an
	// in-memory/hand-built or pre-v2 scene does not). This is the
	// AUTHORED-vs-LEGACY discriminator for static metadata: an ALL-ZERO
	// staticRoot on a serialized scene is COOKED, intentional data (an
	// authored all-dynamic scene), NOT an absent record. Consumers deciding
	// "does this pak carry static marks at all" (the viewer's legacy
	// fallback) must test THIS flag — never any_of(staticRoot), which
	// misreads an authored all-dynamic scene as a legacy pak.
	bool staticRootsSerialized = false;

	// Lightmap opt-out (editor + cook): noLightmap[node] == 1 excludes the
	// node's subtree from lightmap charting/baking (see PakCooker --lightmap
	// and the glTF "smvk.noLightmap" node extras). Excluded geometry still
	// renders (probe/sky-lit fallback) and still occludes/casts in the bake
	// trace scene. Empty or size-mismatched == no exclusions. Carried through
	// merge/serialize paths alongside staticRoot.
	std::vector<uint8_t> noLightmap;
	// Lightmap opt-IN (editor + cook): lightmap[node] == 1 includes the
	// node's subtree in lightmap charting/baking (see cook --lightmap).
	// Default OFF (empty or size-mismatched == skip everything): unlike
	// the opt-out above, bake lightmaps are pay-per-mesh (atlas texels,
	// bake minutes, GPU bytes), so meshes opt in deliberately — typically
	// large static receivers, never foliage/props. A mesh needs BOTH
	// opt-in here AND no opt-out above to chart. NOTE: intentionally NOT
	// serialized (no scene-blob/file section): the flag is authoring-time
	// cook input only — the runtime keys on lmChunk, and tours are the
	// source of truth (replayed every cook). Carried through
	// merge/resize paths alongside noLightmap.
	std::vector<uint8_t> lightmap;
	// Dither on camera closeup (editor + cook): ditherCloseup[node] == 1
	// marks the node's subtree for a dithered fade when the camera gets
	// close (LOD-pop hiding for impostors/foliage). Subtree-inherited like
	// noLightmap. Stored, cooked and replayed end to end.
	//
	// GPU fade (per frame, per draw; see DitherCloseupStateFor): the CPU
	// walk (IsDitherCloseupNode) resolves the flag to a per-draw state —
	// Off / Fade / Hidden — measured from the camera to the FLAGGED
	// ancestor's world translate (the subtree's anchor). Hidden draws
	// skip the frame's draw lists outright (CPU cull); Fade draws carry a
	// per-draw flag (the gfx Draw ring's DrawFlagBit_DitherCloseup, or
	// PushConstants::padEnd_[0] on the direct legs) and every leg that
	// can discard screen-doors with CloseupDitherKeep (common.hlsli) —
	// the same ordered Bayer pattern as dithered transparency, so SMAA
	// T2x integrates both: the lit FS family (default_lit/_a2c/
	// gfxmesh_lit), the masked depth legs (shadow_body: the direct
	// prepass/shadow casters + the filtered seed/remainder twins) and
	// the G-buffer masked writer — the DEPTH dithers in step so the
	// holes see the real background, and the subtree's shadow fades
	// with it. Deliberately NOT dithered: the debug-view FSs (they show
	// the authored geometry solid), the motion-vector legs (the TAA
	// input is depth-derived; revisit if the fade ever smears under
	// T2x), and the editor's own direct passes. The band is fixed meters
	// (kDitherCloseup* below): fade spans the camera distance [hide,
	// solid] mapped keep 0..1.
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
	// Volumetric-shadow caster mark (the coarse fog tile, see
	// WorldRenderer's fog sun-shadow block): castsVolumetricShadow[node]
	// == 1 includes the node's mesh in the dedicated coarse shadow pass
	// that occludes the fog march's sun term (large occluders only -
	// buildings, substantial roofs, walls, cliffs; small props and
	// foliage stay off, they belong to the surface CSM's detail). A
	// SELF flag (per mesh instance, not subtree). Source default is
	// off; empty or size-mismatched == off everywhere. Carried through
	// the merge/serialize/world-resolve paths like the flag tables
	// above.
	std::vector<uint8_t> castsVolumetricShadow;
};

// ---- stable node slots (Epoch 6b) ----------------------------------------
// Alive-word layout: bit 31 = alive, low 12 bits = generation.
inline constexpr uint32_t kNodeSlotAliveBit = 0x80000000u;

// A slot beyond nodeGeneration.size() was created by a wholesale
// resize and is live (see the Scene member comment).
inline bool IsLiveSlot(const Scene& scene, uint32_t slot)
{
	return slot < scene.hierarchy.size()
		&& (slot >= scene.nodeGeneration.size()
			|| (scene.nodeGeneration[slot] & kNodeSlotAliveBit) != 0);
}
// Node-id validity: range + alive (a retired/reused slot answers false
// even though the index is in bounds — the whole point of 6b).
inline bool IsNodeAlive(const Scene& scene, NodeId node)
{
	return IsLiveSlot(scene, node);
}
// The slot's current generation (0 when untracked — matches a fresh slot).
inline uint32_t NodeGenerationOf(const Scene& scene, NodeId node)
{
	return (node < scene.nodeGeneration.size())
		? (scene.nodeGeneration[node] & kNodeGenMask) : 0u;
}
// Live node count (slots minus holes; O(n) — writers/validators, not
// hot paths).
uint32_t LiveNodeCount(const Scene& scene);

// Generational handle for LONG-LIVED references: the slot plus the
// generation it was minted at. Valid until the node retires — and
// STAYS invalid after the slot is reused (the ABA guard the bare id
// cannot give). See scene_node_id.h for the packing.
inline NodeHandle MakeNodeHandle(const Scene& scene, NodeId node)
{
	if (!IsNodeAlive(scene, node))
		return kNodeInvalidHandle;
	return PackNodeHandle(node, NodeGenerationOf(scene, node));
}
inline bool NodeHandleValid(const Scene& scene, NodeHandle handle)
{
	if (handle == kNodeInvalidHandle)
		return false;
	const uint32_t slot = NodeHandleSlot(handle);
	return IsLiveSlot(scene, slot)
		&& NodeGenerationOf(scene, slot) == NodeHandleGen(handle);
}

// Slot-id compaction map for the serializers (the cooked format stays
// DENSE by the locked decision): slotToDense[liveSlot] = its position
// among the live slots in ascending slot order, UINT32_MAX for holes.
// Ascending-slot == the old stable_partition compaction order, so a
// never-deleted scene writes byte-identical bytes to the pre-6b
// serializer.
std::vector<uint32_t> BuildSlotToDenseMap(const Scene& scene);

// Detach one node from its parent's child list (sibling links repaired,
// the node's parent/nextSibling cleared). No-op for root nodes. The
// retirement pair: UnlinkNodeFromParent then RetireNodeSlot.
void UnlinkNodeFromParent(Scene& scene, int node);

// Retire one node's slot: drops every association row (the rows-never-
// outlive-nodes discipline), clears the slot's per-node state (flags,
// metadata, transforms reset to identity, hierarchy entry reset), bumps
// the generation and pushes the slot to the free list. The node must
// already be detached (or doomed inside a deleted subtree). Ids of
// every OTHER node are untouched. Does NOT touch World-level caches
// (drawItems/flags/lod chains) — those are the caller's contract
// (re-resolve, or clean the entries like the editor's uncreate paths).
void RetireNodeSlot(Scene& scene, NodeId node);

int addNode(Scene& scene, int parent, int level);

void markAsChanged(Scene& scene, int node);
// Dynamic-only variant: skips static nodes so frozen subtrees never enter
// changedAtThisFrame after the initial build. Prefer this on the per-frame
// path; use markAsChanged for initial builds / collapse baking.
void markAsChangedDynamic(Scene& scene, int node);
// Full-recalc marking for load/merge/collapse boundaries: marks EVERY
// ROOT node (parent == -1) as changed - each root's subtree follows via
// markAsChanged. The old markAsChanged(scene, 0) idiom assumed node 0 is
// the only root; a deliberate root-level node (the cooker's WorldSun is
// addNode(scene, -1, 1)) falls outside node 0's subtree and kept an
// identity global transform (the horizon-sun bug: the WorldSun frame
// decoded az=0/el=0).
void MarkAllRootsChanged(Scene& scene);

inline bool IsStaticNode(const Scene& scene, int node)
{
	return node >= 0 && (size_t)node < scene.staticNode.size() && scene.staticNode[(size_t)node] != 0;
}

// Lightmap opt-in for a node or any ancestor (subtree inclusion).
// Empty or size-mismatched == skip (default off).
inline bool IsLightmapNode(const Scene& scene, int node)
{
	for (int n = node; n >= 0 && n < int(scene.hierarchy.size()); ) {
		if ((size_t)n < scene.lightmap.size() && scene.lightmap[(size_t)n] != 0)
			return true;
		n = scene.hierarchy[size_t(n)].parent;
	}
	return false;
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

// Dither-on-closeup fade band (meters from the camera to the flagged
// subtree's anchor - the flagged node's world translate). Mirrors the HLSL
// constants DITHER_CLOSEUP_HIDE_M / DITHER_CLOSEUP_SOLID_M (common.hlsli,
// CloseupDitherKeep): keep 0 at/below hide (fully hidden), keep 1 at/beyond
// solid (fully solid), the ordered Bayer screen-door between.
// Tuned TIGHT (was [1, 8] - the dissolve read too strong and started too
// early: 8 m away is doorway distance in these scenes, and the band covered
// most of a walk-up): nothing before 3 m, the dissolve only engages inside
// arm's reach, fully hidden only in the last half meter.
inline constexpr float kDitherCloseupHideM = 0.5f;
inline constexpr float kDitherCloseupSolidM = 3.0f;

// Per-frame closeup-dither state for one draw's node (see ditherCloseup):
//   Off    — beyond the band: draw normally, no per-draw flag
//   Fade   — inside the band: carry the per-draw flag (the Draw ring's
//            DrawFlagBit_DitherCloseup / PushConstants::padEnd_[0]) so the
//            discarding legs screen-door; route the draw into the masked
//            (alpha-tested) depth/shadow programs so the DEPTH dithers
//            with it — solid depth would leave black holes where the lit
//            pass discarded
//   Hidden — at/below the hide distance: skip the draw entirely (CPU
//            cull; no leg sees it this frame)
// The anchor is the FLAGGED ancestor's world translate (the nearest one —
// the tightest authoring intent wins), so the whole flagged subtree fades
// as one cluster around its anchor, exactly the LOD-pop-hiding use.
// Fast path: an empty flag table answers Off for every node (one branch —
// unflagged scenes pay nothing).
enum class DitherCloseupState { Off = 0, Fade = 1, Hidden = 2 };
inline DitherCloseupState DitherCloseupStateFor(const Scene& scene,
	int node, const glm::vec3& eye)
{
	if (scene.ditherCloseup.empty())
		return DitherCloseupState::Off;
	// Nearest flagged ancestor (or the node itself).
	int anchor = -1;
	for (int n = node; n >= 0 && n < int(scene.hierarchy.size()); ) {
		if ((size_t)n < scene.ditherCloseup.size()
			&& scene.ditherCloseup[(size_t)n] != 0) {
			anchor = n;
			break;
		}
		n = scene.hierarchy[size_t(n)].parent;
	}
	if (anchor < 0)
		return DitherCloseupState::Off;
	if (anchor >= int(scene.globalTransform.size()))
		return DitherCloseupState::Off;
	const glm::vec3 p(scene.globalTransform[size_t(anchor)][3]);
	if (!std::isfinite(p.x + p.y + p.z))
		return DitherCloseupState::Off;
	const float d = glm::distance(eye, p);
	if (d <= kDitherCloseupHideM)
		return DitherCloseupState::Hidden;
	if (d >= kDitherCloseupSolidM)
		return DitherCloseupState::Off;
	return DitherCloseupState::Fade;
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

// ---- Env zones (see SceneEnv + Scene::envForNode + EnvVolume) -----------
// A zone = an env node whose transform defines COVERAGE: translation +
// rotation = the placement, scale = half extents per LOCAL axis. The
// unit cube [-1,1]^3 maps onto the box; an EnvVolume entry (when present)
// pushes individual faces to infinity (kEnvInf* bits — half-spaces and
// slabs), while a legacy INFINITE scale component unbounds the whole
// axis. A disabled volume covers nothing. True when `p` lands inside
// the zone's (possibly unbounded) oriented box.
bool ZoneCovers(const Scene& scene, NodeId zone, const glm::vec3& p);
// The zone covering `p`: PRIORITY descending (the volume entry's priority
// when present, else the look block's zonePriority), then smallest
// bounded volume (innermost wins ties; an unbounded face/axis counts as
// unbounded volume and loses against bounded zones); the node id breaks
// remaining ties for determinism. UINT32_MAX = no zone covers the point
// (the resolved default environment stands).
NodeId ZoneAtPosition(const Scene& scene, const glm::vec3& p);
// Effective priority of an env node: the volume entry wins when present,
// else the look block's zonePriority (legacy zones).
float ZonePriority(const Scene& scene, NodeId zone, const SceneEnv& env);

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

// Volumetric-shadow caster mark (SELF flag - see the table's comment):
// the node's mesh joins the coarse fog shadow pass. Grows if needed.
inline void MarkCastsVolumetricShadow(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.castsVolumetricShadow.size() != scene.hierarchy.size())
		scene.castsVolumetricShadow.assign(scene.hierarchy.size(), 0);
	scene.castsVolumetricShadow[(size_t)node] = 1;
}
inline void ClearCastsVolumetricShadow(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.castsVolumetricShadow.size() != scene.hierarchy.size())
		scene.castsVolumetricShadow.assign(scene.hierarchy.size(), 0);
	scene.castsVolumetricShadow[(size_t)node] = 0;
}
// Self-flag predicate (empty/mismatched == off).
inline bool IsCastsVolumetricShadowNode(const Scene& scene, int node)
{
	return node >= 0
		&& (size_t)node < scene.castsVolumetricShadow.size()
		&& scene.castsVolumetricShadow[(size_t)node] != 0;
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

// Marks node as lightmap-opted-in (subtree). Grows the array if needed.
inline void MarkLightmap(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.lightmap.size() != scene.hierarchy.size())
		scene.lightmap.assign(scene.hierarchy.size(), 0);
	scene.lightmap[(size_t)node] = 1;
}
inline void ClearLightmap(Scene& scene, int node)
{
	if (node < 0 || (size_t)node >= scene.hierarchy.size())
		return;
	if (scene.lightmap.size() != scene.hierarchy.size())
		scene.lightmap.assign(scene.hierarchy.size(), 0);
	scene.lightmap[(size_t)node] = 0;
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

// Node name via the dense metadata (nameForNode is gone — see
// NodeMetadata). Empty string = unnamed.
inline std::string getNodeName(const Scene& scene, int node)
{
	const uint32_t strID = GetNodeNameIndex(scene, NodeId(node));
	return (strID != 0xFFFFFFFFu) ? scene.nodeNames[strID] : std::string();
}

// Names append to the pool (the old setNodeName contract: every call
// mints a new pool entry; callers that re-name in place rely on the
// append-only pool never shrinking).
inline void setNodeName(Scene& scene, int node, const std::string& name)
{
	uint32_t stringID = (uint32_t)scene.nodeNames.size();
	scene.nodeNames.push_back(name);
	SetNodeNameIndex(scene, NodeId(node), stringID);
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

// Delete a collection of nodes (their whole subtrees) from a scene
// graph. Epoch 6b: deletion RETIRES slots — the arrays keep their
// length, every surviving node's id stays put, the freed slots re-enter
// circulation through addNode with a bumped generation. Rows of the
// deleted nodes drop; rows of survivors keep their node ids verbatim
// (no remap, no shift helpers — those died with the compaction).
void deleteSceneNodes(Scene& scene, const std::vector<uint32_t>& nodesToDelete);

} // namespace tour::scene

// GLOBAL compat aliases (the migration bridge - retire with the call
// sites; the boundary scanner guards against NEW globals, these are
// the tracked pre-existing set).
using glm::mat4;
using tour::scene::Hierarchy;
using tour::scene::Scene;
using tour::scene::SceneCamera;
using tour::scene::SceneDecal;
using tour::scene::SceneLight;
using tour::scene::SceneRenderSettings;
using tour::scene::MAX_NODE_LEVEL;
using tour::scene::kDecalBloodAlbedoUri;
using tour::scene::kDecalBloodNormalUri;
using tour::scene::kDitherCloseupHideM;
using tour::scene::kDitherCloseupSolidM;
using tour::scene::kNodeSlotAliveBit;
using tour::scene::kSceneRenderFloatCount;
using tour::scene::addNode;
using tour::scene::BuildSlotToDenseMap;
using tour::scene::ClearCastsVolumetricShadow;
using tour::scene::ClearDitherCloseup;
using tour::scene::ClearHidden;
using tour::scene::ClearLightmap;
using tour::scene::ClearNoLightmap;
using tour::scene::ClearSubtreeStatic;
using tour::scene::DecalNodeFrame;
using tour::scene::deleteSceneNodes;
using tour::scene::DitherCloseupState;
using tour::scene::DitherCloseupStateFor;
using tour::scene::dumpSceneToDot;
using tour::scene::dumpTransforms;
using tour::scene::findNodeByName;
using tour::scene::getNodeLevel;
using tour::scene::getNodeName;
using tour::scene::IsCastsVolumetricShadowNode;
using tour::scene::IsDitherCloseupNode;
using tour::scene::IsHiddenNode;
using tour::scene::IsLightmapNode;
using tour::scene::IsLiveSlot;
using tour::scene::IsNodeAlive;
using tour::scene::IsNoLightmapNode;
using tour::scene::IsStaticNode;
using tour::scene::IsStaticSubtreeRoot;
using tour::scene::IsVisibleNode;
using tour::scene::LiveNodeCount;
using tour::scene::loadScene;
using tour::scene::MakeNodeHandle;
using tour::scene::MarkAllRootsChanged;
using tour::scene::markAsChanged;
using tour::scene::markAsChangedDynamic;
using tour::scene::MarkCastsVolumetricShadow;
using tour::scene::MarkDitherCloseup;
using tour::scene::MarkHidden;
using tour::scene::MarkLightmap;
using tour::scene::MarkNoLightmap;
using tour::scene::MarkSubtreeStatic;
using tour::scene::mergeScenes;
using tour::scene::NodeGenerationOf;
using tour::scene::NodeHandleValid;
using tour::scene::printChangedNodes;
using tour::scene::RebuildStaticCache;
using tour::scene::recalculateGlobalTransforms;
using tour::scene::recalculateGlobalTransformsDynamic;
using tour::scene::RetireNodeSlot;
using tour::scene::saveScene;
using tour::scene::setNodeName;
using tour::scene::UnlinkNodeFromParent;
using tour::scene::ZoneAtPosition;
using tour::scene::ZoneCovers;
using tour::scene::ZonePriority;
