// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Authored node PAYLOAD definitions (the scene-tables migration,
// Epoch 5): the inline payload structs the typed association rows
// carry (scene_tables.h) + their binary-record constants. Split out
// of scene.h so the row types can reference them without a cycle;
// scene.h includes this file, so every existing consumer is
// unaffected. These are the AUTHORING-side structs (std::string
// members included) - the COOKED representation (Epoch 7) is a
// separate layout with StringRefs into a string arena, never these
// objects as bytes.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// Authored GI volume (editor GI volume node / tour createGiVolume):
// a box-shaped dense-probe region (gi_baker box-spec shape). The SHAPE
// rides the node's transform (translate = box center, scale = full
// extents, ROTATION = the box axes — an OBB: the bake rotates the six
// box planes, CvBoxPlanesRotated); this carries the bake params.
// Node-local like SceneLight (no pool); resolved against
// globalTransform[node] at use so parenting applies like meshes.

namespace tour::scene {

struct SceneGiVolume {
	uint32_t priority = 0;   // higher = composited later = wins
	float spacing = 0.5f;    // preferred dense probe grid spacing (m;
	                         // the LEGACY fixed layout bakes this when
	                         // neither gates nor a seed are authored)
	float insideFade = 0.1f; // blend width inside each face (m)
	float outsideFade = 0.1f;// blend width outside each face (m)
	// ---- Automatic grid layout (ChooseGiVolumeGridLayout; all default
	// OFF so every pre-optimization volume bakes bit-exact). The GATES
	// bound the bake-time density search; the SEED is the editor's
	// "Optimize grid layout" result (persisted so rebakes are
	// deterministic and the artist can hand-tune). Seed wins over gates.
	float spacingLow = 0.0f;  // density gate low (m; 0 = gates off)
	float spacingHigh = 0.0f; // density gate high (m; 0 = gates off)
	float optSpacing = 0.0f;  // seeded layout spacing (m; 0 = no seed)
	float optPhase[3] = { 0.0f, 0.0f, 0.0f }; // seeded lattice phase
	// The BAKED ARTIFACT binding (the mesh-reference convention applied
	// to bakes): the .givolume this definition's bake produced, recorded by
	// the bake owner at bake time (tour giAsset kind GiVolume). Empty =
	// never baked — the definition exists but carries no field. Path is
	// a VFS path (POSIX separators — RANDOM MINTED, a fresh guid per
	// bake: the record IS the identity, the name carries nothing to go
	// stale); guid is the artifact's .meta GUID ("" = path-only bind).
	// bakeInputHash: the bake-input hash AT BAKE TIME (Engine/GI/
	// bake_inputs.h) — the pak gate's freshness check (a mismatch = the
	// scene changed since the bake = REFUSE the artifact).
	std::string bakeSource;
	std::string bakeGuid;
	std::string bakeInputHash;
};

// EXPORT SETTINGS (the cook/export configuration node — the scene's
// WorldSettings): the bake knobs the cook runs with, authored on ONE
// singleton DAG node (createNode -at exportSettings, auto-seeded by the
// editor; edited in the inspector; persisted as setExportAttr tour
// verbs). This REPLACED the tour `cook` block + the export-window
// session state: the settings travel with the SCENE, and Cook.exe
// consumes them off the replayed scene (CLI flags still win — the same
// explicit-override contract the cook block had). NOT serialized into
// paks (bake-time only — the runtime never reads them; like SceneGiVolume,
// the payload is tour/editor/cook state). Node-local like SceneGiVolume;
// the FIRST exportSettings node in scene id order is THE one (the
// editor seeds exactly one; hand-authored duplicates warn + first
// wins). Field set mirrors the cook CLI flags one-for-one.
struct SceneExportSettings {
	bool meshopt = true;
	bool lods = false;
	std::string lodRatios = "0.75,0.5,0.3,0.15"; // LOD chain ratios (csv)
	float lodError = 0.01f;                  // simplification target error
	bool lightmap = false;
	// Chart density request, METERS PER TEXEL (0.25 = one texel per 25 cm).
	float lmMetersPerTexel = 0.25f;
	uint32_t lmLobes = 12;
	uint32_t lmAtlasWidth = 1024; // FIXED atlas width; height grows
	uint32_t lmRays = 256;
	// Voxel-remesh scene proxy resolution (0 = legacy per-mesh path).
	int lmProxyRes = 256;
	float lmExcludeSmall = 0.0f;
	std::string lmExcludeKw; // extra --lm-exclude keywords (csv)
	// NOTE: no sun/sky fields here — the ENVIRONMENT BLOCKS (SceneEnv,
	// env.default.* editorSet keys) own the sun and sky (lux-native);
	// every bake stage reads the resolved env block (EnvForBake).
	// Preconvolved specular cubemaps (the cook --cubemap stage; the
	// capture set is NAMELESS — keyed on the scene stem).
	bool cubemap = false;
	uint32_t cubemapSize = 128;
	uint32_t cubemapRays = 128;
	// Placement DENSITY (the octree knobs — Cook.exe's --cubemap-octree*
	// set): sparse adaptive placement instead of a uniform lattice.
	bool cubemapOctree = false;
	float cubemapOctreeMax = 8.0f;
	float cubemapOctreeMin = 2.0f;
	uint32_t cubemapOctreeDepth = 4;
	float cubemapExtraRadius = 5.5f;
	// Meshlet cook (per-meshlet build over the cooked topology). The
	// size limits are the ENGINE's kMeshletMaxVertices/Triangles.
	// DEFAULT ON: the meshlet sets are a .geometry-level artifact (the
	// cooked table the pak assembles blindly) and the engine's filtered
	// main view is built on them — a meshlet-less .geometry registers
	// zero GfxRenderer clusters and drops the whole scene to the direct
	// draw path (the slow + visibly broken fallback).
	bool meshlets = true;
	uint32_t meshletMaxVerts = 64;
	uint32_t meshletMaxTris = 64;
	float meshletConeWeight = 0.0f;
	// GI bakes (on-demand Bake GI only — cooks never bake GI): authored
	// convex volumes (the GI volume nodes). (The giWorld flag — the
	// adaptive world field — is RETIRED with the .sggi octree: stage 3
	// of the octree retirement; old tours' giWorld tokens load as
	// acknowledged-and-ignored.)
	bool giVolumes = true;
	// Weave-cloth transmission (sponza's bat convention: MASK cutout
	// cloth passes light tinted per texel through the weave — the tinted
	// light pools behind the banners). AUTHORED HERE (the scene owns the
	// bake config — the invalidation hashes fold it) so Cook + GiBaker +
	// the pak gate derive ONE value; the CLI flags stay as explicit
	// overrides (an overridden run's records mismatch the gate and the
	// pak refuses — author the node instead).
	bool maskTransmission = false;
	// Auto-convert non-KTX textures to KTX2 at cook time.
	bool autoKtx = true;
	// SKIP the loose VFS bake-file resolution at pak assembly: when on,
	// Pak embeds ONLY what the scene itself carries (meshes, materials,
	// textures — everything cooks straight into the pak, blobs by path)
	// and resolves NOTHING loose: no gi/cubemaps/<stem>.cubemanifest +
	// KTX rows, no giAsset binding resolution (the tour's HARD baked-GI
	// records — world field + convex volumes — stay unembedded; the
	// scene renders probe-less + sky IBL ambient; bakes re-embed by
	// turning this off). The canonical use: a MINIMAL variant pak (the
	// same scene, no baked-GI payloads) without moving the loose bake
	// files aside — the bindings key on the SCENE, so renaming the out
	// pak does not dodge them. The App-side --no-vfs flag is the runtime
	// mirror (proves the pak is self-contained); this is the cook-side
	// "do not even resolve it" switch. In-process cooks (the editor)
	// honor it the same way.
	bool ignoreVfs = false;
};

// Env-volume inf-mask bits (per-face infinity over the node's transform
// box, which stays the shape): a set bit pushes that face to -/+infinity.
// Both bits on one axis = the whole axis unbounded (the legacy INF-scale
// slab); neither = the box faces. All six = infinite everywhere (Unreal
// bUnbound). Single-sided bits give half-spaces: e.g. a box with its top
// face at y=0 plus YNEG covers y <= 0 (underwater); bottom at y=50 plus
// YPOS covers y >= 50.
inline constexpr uint32_t kEnvInfNegX = 1u << 0;
inline constexpr uint32_t kEnvInfPosX = 1u << 1;
inline constexpr uint32_t kEnvInfNegY = 1u << 2;
inline constexpr uint32_t kEnvInfPosY = 1u << 3;
inline constexpr uint32_t kEnvInfNegZ = 1u << 4;
inline constexpr uint32_t kEnvInfPosZ = 1u << 5;

// Authored env-volume settings (Unreal PostProcessVolume-style blending
// over the transform-box bounds): node-local like SceneGiVolume, resolved
// against globalTransform[node] at use. The LOOK stays in the node's
// SceneEnv block; this carries the SHAPE-side knobs (bounds/priority/
// blending). Absent entry = legacy zone (full box, priority from the
// look block, weight 1, radius 0, enabled).
struct EnvVolume {
	uint32_t infMask = 0;      // kEnvInf* bits
	float priority = 0.0f;     // higher wins (same units as zonePriority)
	float blendWeight = 1.0f;  // 0..1 global multiplier (Unreal BlendWeight)
	float blendRadius = 0.0f;  // soft edge outside the faces, meters (Unreal BlendRadius)
	bool enabled = true;       // Unreal bEnabled
};

// Authored cubemap capture (editor cubemap node / tour createCubemap):
// an artist-PINNED specular probe position. The POSITION rides the
// node's transform (translate, world-resolved); this carries the
// influence radius. Baked by PakCooker's --cubemap stage as a 'pin'
// row (sorted first at load — a pinned probe wins weight ties, e.g.
// the capture exactly at a chrome ball). Node-local like SceneGiVolume.
struct SceneCubemap {
	float radius = 5.5f;   // influence radius (m); fade = radius/2
};

// One SOUND SLOT of an authored audio source (the Sounds list in the
// inspector). A slot is an ASSET REFERENCE, never a playing voice —
// clips are shared by many emitters and playback belongs to runtime
// state (the audio manager's voices). `name` is the slot's display
// label ("Bells"); `assetPath` is the authoring-time reference (a
// project-relative .wav path — the COOK resolves it to the stable
// sound content id 0x4xxx and the cooked scene stores the ID, never
// the string). The id lands on `soundId` at cook time; authored
// scenes carry soundId 0 (unresolved — the runtime never sees them).
struct AudioSoundRef {
	std::string name;      // "Bells" (display/undo label)
	std::string assetPath; // "content/audio/bells.wav" (authoring ref)
	uint64_t soundId = 0;  // cooked content id (0 = not yet resolved)
	float gain = 1.0f;     // per-slot gain multiplier
	bool loop = false;     // per-slot loop default
};

// Authored audio source (the AudioSource node — the emitter
// association). The EMITTER POSE rides the node's transform (position
// + orientation from globalTransform[node], exactly like SceneLight/
// SceneCamera); this carries the emitter's properties + its sound
// slots. Node-local like SceneGiVolume (an inline-payload row, no
// pool): sources are per-node definitions, and the SAME clip can back
// many sources (the sharing lives in the sound assets, not here).
struct SceneAudioSource {
	float volume = 1.0f;        // master emitter gain
	bool spatial = true;        // 3D attenuation + pan (else 2D full-mix)
	bool playOnLoad = false;    // start the default slot at scene load
	float minDistance = 1.0f;   // full-volume radius (m)
	float maxDistance = 30.0f;  // silent radius (m)
	std::vector<AudioSoundRef> sounds; // the sound slots (asset refs)
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
// node data below (givolume/cubemap): the resolved ACTIVE look lives in
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
	// Postfx look: SPLIT OUT — see SceneRenderSettings (the render-grade
	// knobs are a global scene look, never zoned; env blocks own
	// sky/sun/fog/volumetrics only).
	// ZONE priority (only meaningful on per-node env blocks): when several
	// zones cover a point, higher priority wins; ties go to the smallest
	// bounded volume (innermost). 0 = the default precedence.
	float zonePriority = 0.0f;
	std::string skyboxUri;     // stored (skybox sampling is TODO)
};

// Authored-environment binary record (see saveEnvFloats/loadEnvFloats
// in Scene.cpp): 16 base floats + zonePriority, fixed order. Shrunk when
// the postfx block split into SceneRenderSettings (old files fail the
// read loudly — the sanctioned no-conversions break, AGENTS.md).
// Booleans are stored as 0/1 floats; enum values are stored numerically.
inline constexpr int kSceneEnvFloatCount = 17;

} // namespace tour::scene

// GLOBAL compat aliases (the migration bridge - retire with the call
// sites; the boundary scanner guards against NEW globals, these are
// the tracked pre-existing set).
using tour::scene::SceneGiVolume;
using tour::scene::SceneExportSettings;
using tour::scene::kEnvInfNegX;
using tour::scene::kEnvInfPosX;
using tour::scene::kEnvInfNegY;
using tour::scene::kEnvInfPosY;
using tour::scene::kEnvInfNegZ;
using tour::scene::kEnvInfPosZ;
using tour::scene::EnvVolume;
using tour::scene::SceneCubemap;
using tour::scene::SceneEnv;
using tour::scene::kSceneEnvFloatCount;
using tour::scene::SceneAudioSource;
using tour::scene::AudioSoundRef;
