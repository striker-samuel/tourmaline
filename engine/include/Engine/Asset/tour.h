// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// .tour — Tourmaline editor scene overlay (ASCII, Maya .ma flavor).
//
// A .tour file is a human-readable list of CRUD commands applied ON TOP
// of a source glTF (never a blob store: meshes/textures stay in the
// source + loose files and are referenced by id/path only). Both the
// Editor (live edits + undo) and PakCooker (--tour replay at cook time)
// speak it, so what-you-see is what gets cooked:
//
//   //Name: bistro.tour
//   //Last modified: Fri, Sep 26, 2026 09:12:04 AM
//   source -p "content/models/Bistro/bistro.gltf" -fingerprint "9f3a...";
//   reference -n 0 -source "props/chair.gltf" -name "chair";  # refs FIRST
//   fileInfo -k "application" -v "tourmaline-tour";            # constants
//   fileInfo -k "product" -v "Tourmaline";
//   fileInfo -k "version" -v "1";
//   rename -n 45 "Shop_Wall";                       # update (name)
//   setAttr -n 45 -at translate -type "double3" -t 1.2 3.4 5.6;
//   assignMaterial -n 46 -m 12;                     # update (material link)
//   setMatAttr -m 12 -at baseColor -type "float4" -t 1 0.5 0.2 1;
//   setNoLightmap -n 45 -type "bool" -v 1;          # update (bake flag, legacy opt-out)
//   setLightmap -n 45 -type "bool" -v 1;            # update (bake flag, opt-in)
//   setVisibility -n 45 -type "bool" -v 1;          # update (hidden flag)
//   parent -n 45 -p 12;                             # update (topology)
//   delete -uuid 9f3a... -n 45 -name "Shop_Wall";   # remove (uuid/name/id)
//   createLight -p 0 -name "Spot_1" -at spot -t 1 2 3 1500 25 20 35 0 1;
//       setLightAttr -ref 0 -at intensity -v 1500;  # tabulated: -ref
//                                                   # props ride under
//                                                   # their create
//   override -pattern "Building_.*" -at visibility -v 1 -enabled on;   # wide
//   override -pattern "Tree.*" -at material -m 3 -enabled on;          # wide
//   createNode -at exportSettings -p 0 -name "ExportSettings"; # the WorldSettings
//       setExportAttr -ref 0 -at cubemap -type "bool" -v on;  # tabulated: -ref
//                                                   # props ride under
//                                                   # their create
//
// Non-scalar setAttr values carry an explicit -type (double3/float4/
// string/bool); plain scalars stay bare. Unknown verbs and unknown
// -type flavors warn-and-skip (forward compatibility).
//
// Wide overrides (Clarisse-style) match node NAMES by ECMAScript regex and
// apply after every direct command at replay; per-node local commands win
// over any rule touching the same node+attr (more specific wins), and
// later rules win over earlier ones. Rule order in the file is the
// precedence order.
//
// Node/material ids are CLUSTER-space: the importer builds clusters
// deterministically from the source file, so the same source always yields
// the same ids on both sides. Recorded node names are verify-only (warn on
// mismatch, apply by id anyway). Unknown verbs warn-and-skip (forward
// compatibility = edit compatibility across versions).

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Asset/meshlet.h> // kMeshletMaxVertices/Triangles (the engine meshlet canon)
#include <Engine/Scene/scene.h> // SceneExportSettings (the export-settings node payload)

namespace smvk {
struct Cluster;
} // namespace smvk

struct Scene;

namespace smvk::tour {

inline constexpr int kTourVersion = 1;

// Cook settings interchange is GONE: the export settings are a SCENE
// NODE now (createNode -at exportSettings + setExportAttr verbs, the
// payload in Scene::exportForNode / SceneExportSettings — the WorldSettings
// pattern). The old `cook` block + TourCook struct died with it: Cook.exe
// reads the settings off the replayed scene (CLI flags still win); the
// editor authors them on the auto-seeded singleton node.

// Maya file node (the editor's image definition, promoted to a cooked
// record): a texture source OUTSIDE the DAG with its sampling intent.
// The cooker reads these for every converted texture — colorspace picks
// the KTX2 transfer (sRGB vs linear), wrap/filter override the glTF
// sampler on a match. Matched to glTF images by path basename, then
// stem; unmatched images fall back to usage inference (baseColor /
// emissive = sRGB, everything else linear).
struct FileNode {
	std::string name; // display name (defaults to the path stem)
	std::string path; // source image (host path or VFS-relative)
	int colorspace = 0; // 0 = sRGB (color/emissive), 1 = linear (data)
	int wrap = 0;       // 0 = repeat, 1 = clamp-edge, 2 = mirrored-repeat
	int filter = 0;     // 0 = linear, 1 = nearest
};

// Material-asset import (ordered editor-side POOL imports, replayed
// BEFORE the command list on both sides so cluster pool indices recorded
// by later assignMaterial/-m lines resolve exactly like the authoring
// session): asset-browser materials (a .material + its texture files
// beside it) minted into the cluster pool, plus the editor's default-
// material seed. The record order IS the pool order — nothing else
// appends to the pool after the reference bind.
struct MaterialAssetCmd {
	bool editorDefault = false; // the deterministic "editor://default" seed
	                             // material (no source file)
	std::string path;  // .material source (project-relative or absolute,
	                   // POSIX separators — the reference-source convention)
	std::string name; // display name
};

// Texture-asset import (ordered editor-side POOL imports, replayed
// BEFORE the command list on both sides so the textures the scene needs
// resolve exactly like the authoring session): cookie .texture files
// (a .texture + its source image beside it) minted into the cluster
// pool. The record order IS the TEXTURE import order (before the
// material-asset records, so a material referencing a .texture finds
// its pool entry). Textures the glTF itself imports need no record
// (reference-bound, like every other cluster content).
struct TextureAssetCmd {
	std::string path;  // .texture source (project-relative or absolute,
	                   // POSIX separators — the reference-source convention)
	std::string name; // display name
};

// GI-asset binding (the scene's HARD identification of its baked GI
// artifacts — the mesh-reference convention applied to bakes). Each
// record names ONE baked artifact by VFS path + the artifact cache's
// .meta GUID, keyed to what it serves:
//   - kind World: the world-field probe octree;
//   - kind Cvol: ONE authored convex volume, addressed by the cvolume
//     family ref (the SAME -ref grammar setCvolAttr uses — the k-th
//     createCvolume in file order), naming its own .cvol.
// Stamped by the bake owner (GiBaker / the editor cook) AT BAKE TIME,
// consumed by the pak cooker (embeds EXACTLY the bound files + registers
// the runtime GiBindings off them) and the editor (preview loaders
// resolve by record — no stem guessing, no manifests). Bake artifacts
// carry RANDOM MINTED names (a fresh guid per bake): the record IS the
// identity — the file name carries no scene/stem information to go
// stale, a rebake never collides with or overwrites the old artifact
// (the stamp's supersede list retires it BY RECORD), and the pak's
// name never enters the bake identity at all.
struct GiAssetCmd {
	enum class Kind : uint8_t { World, Cvol };
	Kind kind = Kind::World;
	uint32_t ref = 0;    // cvolume family ref (Kind::Cvol only): raw
	                     // kCreateCvolRefBase|k form as authored
	std::string source;  // VFS path, POSIX separators ("gi/<uuid>.sggi")
	std::string guid;    // artifact .meta GUID ("" = path-only bind)
};

enum class Verb : uint8_t {
	Rename,
	SetTranslate,
	SetRotate, // degrees XYZ, same convention as the editor inspector
	SetScale,
	AssignMaterial, // material = cluster material POOL index
	SetMatAttr,     // text = attr name; vec/vec4/number by attr
	SetNoLightmap,  // flag
	SetLightmap,    // flag (bake opt-in; default off, see Scene::lightmap)
	SetDitherCloseup, // flag (subtree dither on camera closeup)
	SetVisibility,  // flag: hidden self state (hierarchical, see Scene::hidden)
	SetStatic,      // flag: static-subtree ROOT mark (Scene::staticRoot —
	                // frozen transforms after the initial build, mega-mesh
	                // collapse eligibility; see Scene/static_merge.h)
	Parent,         // node2 = new parent (cluster node id)
	CreateLight,    // node = PARENT cluster id; text = light name;
	                // vec = local position; vec4 = (typeFloat [1=point,
	                // 2=spot], intensity [cd], range [m, 0=inf],
	                // innerConeDeg); number = outerConeDeg; flag =
	                // static bake-only; node2 = shadow request (0/1)
	SetLightAttr,   // node = target cluster id, or a created-light ref
	                // (kCreateLightRefBase | k — see below); text = attr
	                // ("color","intensity","range","inner","outer",
	                // "static","shadow","name"); vec4 = color value;
	                // number = scalar (cone angles in DEGREES); flag =
	                // flag value; name = new name (attr "name" only)
	CreateTerrain,  // node = PARENT cluster id; text = terrain name;
	                // vec = local position (group origin corner); vec4 =
	                // (size[m], res[samples/side], height[m], freq[1/m]);
	                // number = noise seed; material = cluster material pool
	                // index; node2 = tilesX | (tilesZ << 16) (tile split)
	TerrainPaint,   // node = kCreateTerrainRefBase | k (k-th createTerrain);
	                // text = op ("raise","lower","flatten","smooth"); vec4 =
	                // (worldX, worldZ, radius, flattenTarget); number =
	                // strength (m for raise/lower, 0..1 blend otherwise)
 	Delete,         // Maya delete: file-stable node identity —
 	                // uuid PRIMARY (see SetUuid), name secondary,
 	                // numeric id as legacy fallback/disambiguator:
 	                //   delete -uuid <hex> -n <id> -name "<name>"
 	                // (-uuid and/or -name may be absent in hand-written
 	                // or legacy lines). Resolution: exact uuid match
 	                // wins; else a unique full-name match; else a
 	                // '/'-separated ancestor path suffix ("grp/mesh",
 	                // matched from any depth); else the recorded id
 	                // when it still names the same node (disambiguator
 	                // for duplicated names) or for nameless legacy
 	                // deletes. Ids are never renumbered; pool resources
 	                // are never reclaimed — only the LINKS drop.
 	SetUuid,        // node = cluster node id; uuid = 32-hex file-stable
 	                // identity recorded on it (Scene::uuidForNode). The
 	                // editor assigns these for every node a Delete line
 	                // addresses and emits them BEFORE the delete lines;
 	                // replay/cook record them back onto the nodes.
 	CreateTransform, // node = PARENT cluster id; text = node name;
 	                // vec4 = (tx,ty,tz...) — see below. A plain transform
 	                // node (Maya group locator): no mesh, no light, no
 	                // pool entry. The editor uses these as reference-
 	                // displacement placeholders (see below); anything may
 	                // parent under them like any transform.
 	                // -t packs local TRS: x y z rx ry rz sx sy sz
 	                // (9 values, rotation in DEGREES, editor convention).
 	                // (Legacy verb — new files write the unified
 	                // createNode form below.)
   	CreateNode,     // THE node verb (Maya createNode): -at names the
   	                // node TYPE, the rest is per-type payload:
   	                //   DAG types (hierarchy nodes — transform +
   	                //   parentable): transform, pointLight, spotLight,
   	                //   terrain, cvolume, cubemap, camera,
   	                //   exportSettings (the WorldSettings-style export
   	                //   config singleton; payload via setExportAttr,
   	                //   NOT on the create line). Replayed by
   	                //   cook + editor with full create semantics (-ref
   	                //   bookkeeping included).
   	                //   System types (no DAG presence — editor-managed
   	                //   data): material, file, image. Parsed +
   	                //   validated, skipped at replay with a log.
   	                // New files write ONLY this form; the legacy
   	                // createLight/createTerrain/createTransform/
   	                // createCvolume verbs still parse (backward compat).
 	CreateCVolume,  // node = PARENT cluster id; text = volume name;
	                // vec = local position (box center); svec = local
	                // size (full extents, m); vec4 = (priority,
	                // spacing[m], insideFade[m], outsideFade[m]).
	                // An authored convex GI volume (gi_baker box spec
	                // shape): axis-aligned, rotation ignored.
 	SetCvolAttr,    // node = target cluster id, or a created-volume ref
	                // (kCreateCvolRefBase | k — see below); text = attr
	                // ("priority","spacing","inside","outside",
	                // "position","rotation","size","name"); number =
	                // scalar value; vec4 = triple value (position/
	                // rotation-deg/size); name = new name (attr "name").
  	CreateCubemap,  // node = PARENT cluster id; text = capture name;
  	               // vec = local position (pinned probe); number =
  	               // influence radius (m). An authored cubemap capture:
  	               // the cook's --cubemap stage bakes it as a 'pin' row
  	               // (wins weight ties at load).
  	SetCubemapAttr, // node = target cluster id, or a created-capture ref
  	               // (kCreateCubemapRefBase | k — see below); text = attr
  	               // ("position","radius","name"); number = scalar
  	               // (radius); vec4 = triple (position); name = new name
  	               // (attr "name").
  	SetExportAttr,  // node = target cluster id, or the created
  	               // exportSettings node ref (kCreateExportRefBase | k);
  	               // text = attr — a SceneExportSettings field name
  	               // ("meshopt","lods","lodRatios","lodError",
  	               // "lightmap","lmMetersPerTexel","lmLobes",
  	               // "lmAtlasWidth","lmRays","lmProxyRes",
  	               // "lmExcludeSmall","lmExcludeKw","cubemap",
  	               // "cubemapSize","cubemapRays","cubemapOctree",
  	               // "cubemapOctreeMax","cubemapOctreeMin",
  	               // "cubemapOctreeDepth","cubemapExtraRadius",
  	               // "meshlets","meshletMaxVerts","meshletMaxTris",
  	               // "meshletConeWeight","giCvol","giWorld","autoKtx",
  	               // "ignoreVfs"); number = scalar value; flag = bool
  	               // value (-type bool); name = string value (-type
  	               // string). The scene's WorldSettings-style export
  	               // config node (the cook block REPLACEMENT — see
  	               // SceneExportSettings).
  	CreateCamera, // node = PARENT cluster id; text = camera name;
  	              // vec = local position; svec = local rotation in
  	              // DEGREES (editor TRS convention); vec4 =
  	              // (projFloat [0 = perspective, 1 = orthographic],
  	              // fovYDeg, nearClip, farClip); number = orthoHalfH
  	              // (m, ortho only). An authored camera node: the lens
  	              // lives in the cluster cameras pool, the pose rides
  	              // the node transform (forward = node -Z in world).
  	SetCamAttr,   // node = target cluster id, or a created-camera ref
  	              // (kCreateCameraRefBase | k — see below); text = attr
  	              // ("projection","fovY","orthoHalfH","near","far",
  	              // "name","position","rotation"); number = scalar
  	              // value (projection: 0/1); name = new name (attr
  	              // "name" only); vec4 = triple (position/
  	              // rotation-deg). Post-creation TRS edits on created
  	              // cameras lower to position/rotation (their cluster
  	              // ids don't exist until replay).
 };

// Node-type table for createNode -at (Maya node types): whether the
// type takes DAG presence (hierarchy node: transform + parentable) or
// is a system type (editor-managed data, no scene node).
enum class CreateNodeKind : uint8_t { Dag, System };
struct CreateNodeType {
	const char* name;      // -at value ("transform", "pointLight", ...)
	CreateNodeKind kind;
};
// The known types, in file order (index-stable for tooling).
const CreateNodeType* CreateNodeTypes(size_t& countOut);
// Lookup by -at value (accepts the legacy light aliases "point" and
// "spot" for pointLight/spotLight). Null when unknown.
const CreateNodeType* FindCreateNodeType(const std::string& at);

// Clarisse-style wide override attribute (regex selector over node
// names). Stored in Doc::overrides, evaluated AFTER direct commands at
// cook/replay time; per-node local commands win over matching wide
// rules (more specific wins), later rules win over earlier ones.
enum class OverrideAttr : uint8_t {
	Visibility,   // flag = hidden (1 hides the subtree root)
	Material,     // material = cluster material pool index
	NoLightmap,   // flag
	Lightmap,     // flag (bake opt-in)
	DitherCloseup, // flag
};

struct OverrideRule {
	OverrideAttr attr = OverrideAttr::Visibility;
	std::string pattern; // ECMAScript regex over node names
	bool flag = false;   // Visibility/NoLightmap/Lightmap/Dither value
	uint32_t material = 0; // Material: cluster material pool index
	bool enabled = true;
};

// Clarisse-style SCENE REFERENCE: a placeholder node in the OWNING scene
// whose subtree is another scene, kept as a reference (structure + link
// to the source, never a flattened duplicate). References nest: a
// referenced source can itself carry `reference` lines in its own .tour,
// resolved recursively at load/cook (cycle-guarded). Overrides apply per
// occurrence through the existing machinery (referenced nodes are normal
// nodes once inlined), so a wheel override in the shot never touches the
// wheel asset.
struct ReferenceCmd {
	uint32_t parent = 0;      // placeholder's parent (cluster node id)
	std::string source;       // referenced scene: .gltf (raw geometry) or
	                          // .tour (the authored scene — its source gltf
	                          // is imported, then its tour commands replay
	                          // with id offsets; see ApplySceneReferences)
	std::string guid;         // source file's asset GUID (.meta sidecar):
	                          // stable across moves/renames, resolved first;
	                          // `source` stays as the legacy fallback
	std::string name;         // placeholder node name ("REF <name>")
	glm::vec3 translate{ 0.0f }; // optional placement offset
	bool hasTranslate = false;
	bool enabled = true;      // false = unloaded (kept in the list, not
	                          // bound; Maya-style load/unload)
	// ROOT reference: the scene's OWN source glTF, kept in the document
	// as reference #0 (the definition is self-contained — no separate
	// `source` line). Inlined directly under the scene root with NO
	// placeholder and NO namespace (Maya: the master file's own
	// contents carry no prefix), so cluster ids match a direct source
	// import exactly. Editor and cook build identical worlds from the
	// identical doc. Never disabled/removed/replaced.
	bool root = false;
	// SOURCED MESH ASSET (the asset-bind semantic): true = the entry
	// places a derived .geometry/.mesh data blob's content under
	// `parent` — serialized as the per-node `source` verb (`source -n
	// <p> -p "<blob>"`). Meshes NEVER ride the `reference` verb: that
	// verb is for referencing another .tour SCENE. Legacy tours naming
	// a blob in `reference -source` parse back with this set (the kind
	// is extension-derived), so old files keep binding; a re-save
	// migrates them to the `source` verb. ROOT entries ignore it (the
	// scene's own source stays `reference -root` whatever it points
	// at — every existing model tour records it that way).
	bool asset = false;
};

struct Command {
	Verb verb = Verb::Rename;
	uint32_t node = 0;     // cluster node id (node verbs)
	uint32_t node2 = 0;    // Parent: new parent cluster node id;
	                       // CreateLight: shadow request (0/1)
	std::string parentRef; // -p as QUOTED name/path/uuid (symbolic parent
	                       // form): for Parent/CreateLight/CreateTerrain/
	                       // CreateTransform, the parent by human or uuid
	                       // identity instead of a bare numeric id. Empty
	                       // = numeric node2 form (legacy).
	uint32_t material = 0; // AssignMaterial / SetMatAttr: cluster pool index
	std::string text;      // Rename: new name; SetMatAttr/SetLightAttr:
	                       // attr name; CreateLight: light name
	glm::vec3 vec{ 0.0f }; // translate / rotate-deg / scale /
	                       // CreateLight local position
	glm::vec3 svec{ 1.0f, 1.0f, 1.0f }; // createTransform local scale;
	                       // CreateCVolume local size (full extents, m)
	glm::vec4 vec4{ 1.0f }; // mat vec attr value (baseColor/emissive);
	                        // CreateLight: (typeFloat, intensity, range,
	                        // innerConeDeg); SetLightAttr "color": rgb
	double number = 0.0;   // mat scalar attr value; CreateLight:
	                       // outerConeDeg; SetLightAttr scalar value
	bool flag = false;     // SetNoLightmap / SetLightmap /
	                       // SetDitherCloseup value;
	                       // CreateLight: static bake-only;
	                       // SetLightAttr static/shadow value
	std::string name;      // recorded node name (verify-only);
	                       // SetLightAttr "name": the NEW name
	                       // Delete: secondary addressing key
	std::string uuid;      // SetUuid: the recorded 32-hex identity;
	                       // Delete: its PRIMARY addressing key
 	std::string nodeType;  // createNode -at type ("transform",
  	                       // "pointLight", "spotLight", "terrain",
  	                       // "cvolume", "cubemap", "camera",
  	                       // "exportSettings", "material", "file",
  	                       // "image")
};

// SetLightAttr addressing for lights created by an earlier CreateLight
// verb in the same doc (they own no cluster id until replayed):
// node = kCreateLightRefBase | k references the k-th CreateLight
// command (0-based file order). Maintained as a creation-order list at
// replay: cook ApplyTourToCluster keeps local created ids; the editor
// TourToCmd keeps live world ids (m_tourCreatedNodes).
inline constexpr uint32_t kCreateLightRefBase = 0x80000000u;

// Same scheme for terrain created by an earlier CreateTerrain: generic
// verbs (Rename/SetTranslate/.../Parent) address created terrain nodes
// via kCreateTerrainRefBase | k (k-th CreateTerrain in file order).
// Disjoint bit from the light base; a node carries at most one.
inline constexpr uint32_t kCreateTerrainRefBase = 0x40000000u;

// Same scheme for convex volumes created by an earlier CreateCVolume:
// SetCvolAttr (and only it — generic verbs on created volumes lower to
// SetCvolAttr position/rotation/size/name at save) addresses them via
// kCreateCvolRefBase | k (k-th CreateCVolume in file order). Disjoint
// from both bases above; a node carries at most one.
inline constexpr uint32_t kCreateCvolRefBase = 0x20000000u;

// Same scheme for cubemap captures created by an earlier CreateCubemap:
// SetCubemapAttr addresses them via kCreateCubemapRefBase | k (k-th
// CreateCubemap in file order). Disjoint from all bases above.
inline constexpr uint32_t kCreateCubemapRefBase = 0x10000000u;

// Same scheme for cameras created by an earlier CreateCamera:
// SetCamAttr addresses them via kCreateCameraRefBase | k (k-th
// CreateCamera in file order). Disjoint from all bases above.
inline constexpr uint32_t kCreateCameraRefBase = 0x08000000u;

// Same scheme for the exportSettings node created by an earlier
// createNode -at exportSettings: SetExportAttr addresses it via
// kCreateExportRefBase | k (k-th exportSettings create in file order —
// the singleton semantic keeps that at 0 or 1; hand-authored duplicates
// warn + first wins). Disjoint from all bases above.
inline constexpr uint32_t kCreateExportRefBase = 0x04000000u;

// All create-family ref bases OR'd together: a create-family ref
// carries EXACTLY ONE base bit plus a small k (< 0x08000000 — the
// create counts are bounded far below the lowest base). A value with
// SEVERAL high bits set (a sentinel like UINT32_MAX, or any garbage) is
// a ref of NO family — ref checks must test the exclusive form
// ((n & mask) == base), never a single bit (a single-bit test made
// UINT32_MAX masquerade as a terrain ref and misreported the replay
// as "terrain ref out of range" instead of the honest node error).
inline constexpr uint32_t kCreateRefFamilyMask =
	kCreateLightRefBase | kCreateTerrainRefBase | kCreateCvolRefBase
	| kCreateCubemapRefBase | kCreateCameraRefBase
	| kCreateExportRefBase;

struct Doc {
	int version = kTourVersion;
	std::string source;      // source glTF path (DEPRECATED: geometry may
	                         // come entirely from references — may be
	                         // empty; the loader builds an empty world
	                         // then binds references + replays commands)
	std::string fingerprint; // size+mtime of source at save (warn on mismatch)
	std::vector<Command> commands;
	std::vector<OverrideRule> overrides; // wide regex rules (ordered, later wins)
	// Scene references (ordered; the OWNING scene's level-1 list —
	// deeper nesting comes from each referenced source's own tour).
	// Written FIRST in the file (Maya file-block order).
	std::vector<ReferenceCmd> references;
	// File nodes (ordered Maya file/image declarations, editor-authored):
	// cooked records — the cooker consumes them for KTX conversion
	// (unlike editorSettings, which never cook).
	std::vector<FileNode> fileNodes;
	// Material-asset imports (ordered; see MaterialAssetCmd): replayed by
	// the editor AND the cook BEFORE the command list, so pool indices
	// recorded by assignMaterial/-m lines match the authoring session.
	std::vector<MaterialAssetCmd> materialAssets;
	// Texture-asset imports (ordered; see TextureAssetCmd): replayed by
	// the editor AND the cook BEFORE the material-asset records, so a
	// material (or a light cookie) referencing a .texture finds its pool
	// entry exactly like the authoring session.
	std::vector<TextureAssetCmd> textureAssets;
	// GI-asset bindings (ordered; see GiAssetCmd): the scene's baked-GI
	// identification — the world field + one record per authored convex
	// volume. Resolved onto the replayed scene by ApplyTourToCluster
	// (Scene::giWorld* + SceneCvol::bake*), read by the pak cooker / the
	// editor loaders. The bake owner upserts these after every bake.
	std::vector<GiAssetCmd> giAssets;
	// fileInfo constants (Maya fileInfo block): fixed format metadata
	// (application/product/version), then anything carried. Written
	// right after the references.
	std::vector<std::pair<std::string, std::string>> fileInfo;
	// Editor-only settings (editorSet lines): viewport/env authoring state
	// the EDITOR persists for itself. NEVER applied by ApplyTourToCluster
	// — the cook silently ignores this whole section (editor IBL and
	// friends must not leak into paks). The ONLY exception: the env
	// blocks (env.default.* / env.n.* / env.current — resolved into the
	// cluster scene by ApplyTourToCluster) and the TYPED scene-object
	// reference fields below — both are world settings the runtime App
	// consumes through the cooked pak, not editor view state.
	//
	// TYPED SCENE-OBJECT REFERENCE FIELD (the reusable pattern): the key
	// names the object family, the value is a replay-stable address in
	// the family's own addressing grammar. camera.default is the first
	// field: "ref <k>" (the k-th CreateCamera in the doc — the same
	// family ref space setCamAttr -ref uses) or "node <clusterId>" (a
	// cluster-backed camera, e.g. an imported glTF camera). Resolved at
	// cook (ApplyTourToCluster -> Scene::defaultCameraNode, validated
	// against the replayed scene, dangling refs warn + drop like env
	// blocks) and at editor replay (the live world node id). Future
	// typed fields (light.default, ...) reuse this grammar.
	std::vector<std::pair<std::string, std::string>> editorSettings;
	// NOTE: no cook block anymore — the export settings are a SCENE NODE
	// (createNode -at exportSettings + setExportAttr, payload in
	// Scene::exportForNode); see SceneExportSettings. The old `cook`
	// verb + TourCook struct are gone (old files: the verb warns + skips;
	// settings default until re-authored on the node).
};

// Best-effort source fingerprint ("<size>:<mtime>" hex); empty when the
// file is missing.
std::string Fingerprint(const std::string& path);

bool SaveTour(const std::string& path, const Doc& doc, std::string& err);
bool LoadTour(const std::string& path, Doc& doc, std::string& err,
	std::vector<std::string>* warnings = nullptr);
// Parse from an in-memory text (the file loader reads then calls this;
// also lets VFS-based consumers cook referenced-scene tours).
bool LoadTourFromString(const std::string& text, Doc& doc, std::string& err,
	std::vector<std::string>* warnings = nullptr);

// Replay node/material commands onto a freshly imported cook Cluster
// (cluster-space ids apply directly). Runs BEFORE meshopt/LOD/bake so pool
// indices match the import. Returns false on structural errors (bad node /
// material ids); name mismatches only warn. strict = false (reference
// replay): a failed command is REPORTED into `warnings` and skipped
// instead of aborting — a partially-broken reference must not take the
// whole scene down (ApplySceneReferences surfaces the list to the editor).
// createdCvolsOut (optional): the resolved created-cvolume family —
// createdCvolsOut[k] = the node id of the k-th createCvolume (the SAME
// index the giAsset/cvol family refs address). The bake owners (GiBaker,
// Cook.exe's in-process cvol bake) need it to stamp the doc's giAsset
// records back after a bake (StampGiAssets).
bool ApplyTourToCluster(Cluster& cluster, const Doc& doc,
	std::vector<std::string>* warnings = nullptr, std::string* err = nullptr,
	bool strict = true,
	std::vector<uint32_t>* createdCvolsOut = nullptr);

// The bake owner's STAMP: mirror a replayed scene's GI bindings onto the
// doc's giAsset records. The SCENE is the authority — the bake updates
// Scene::giWorldSource + SceneCvol::bakeSource in place (unbaked volumes
// keep their old bindings; deleted volumes drop), then this regenerates
// doc.giAssets to match (World record when the scene carries one, one
// Cvol record per bound volume, family-ref addressed over createdCvols).
// Callers: GiBaker (writes the tour back), Cook.exe's in-process cvol
// fallback, the editor cook (its live doc). Nodes outside createdCvols
// (hand-injected, no tour create) warn+skip. `superseded` (optional):
// the OLD records' paths that the new set no longer references — the
// caller deletes those files (record-exact; bake artifacts carry RANDOM
// minted names, so a stale file can never collide with a fresh bake —
// the only way an old artifact lingers is nobody deleting it, and the
// record set-difference is the exact list). Returns false only on a
// structural error (err out).
bool StampGiAssets(Doc& doc, const Scene& scene,
	const std::vector<uint32_t>& createdCvols,
	std::string* err = nullptr,
	std::vector<std::string>* superseded = nullptr);

// Structural reparent on any Scene (shared by the editor live path and the
// cook replay path). Index-preserving: no nodes are added/removed, only
// parent/child/sibling links + subtree levels. Silently ignores cycles,
// the root, and depth overflow past MAX_NODE_LEVEL.
void TourReparent(Scene& scene, uint32_t node, uint32_t newParent);

// Human-readable one-liner per command (console/log use).
std::string Describe(const Command& c);
std::string DescribeOverride(const OverrideRule& r);

// 128-bit random identity, 32 lowercase hex chars. Assigned by the
// editor for every node a Delete line addresses (emitted as `uuid`
// lines ahead of the deletes); replay/cook record them back onto the
// nodes so deletes resolve through it — stable across re-imports that
// shift numeric ids.
std::string GenerateUuid();

// Symbolic parent resolution (the -p quoted form): a uuid hex, a node
// name, or a '/'-separated ancestor path — resolved against the CURRENT
// scene, so parents survive numeric id shifts. False = nothing matched.
bool ResolveParentTarget(const Scene& scene, const std::string& ref,
	uint32_t& out);

// Maya's identity layers, mapped onto this codebase (setUuid parity):
//
//   MUuid (persistent)      uuidForNode: saved in the scene, stable
//                           across renames and hierarchy moves. Like
//                           Maya, setUuid REJECTS a uuid already held
//                           by another LIVE node (SetNodeUuid).
//   MObject (runtime)       the numeric node id: valid only for the
//                           live scene — never a file identity.
//   MDagPath (location)     DagFullPath / DagShortestUniquePath below:
//                           full and shortest-unique '/'-paths (Maya's
//                           fullPathName/partialPathName). The ONLY way
//                           to name a specific occurrence.
//   name+namespace (human)  getNodeName (namespaced for references).
//
// Record a uuid on a node (Maya setUuid): false when another live node
// already holds it (nothing assigned); true + assigned otherwise.
// Re-assigning the SAME uuid to its current holder is a no-op true.
bool SetNodeUuid(Scene& scene, uint32_t node, const std::string& uuid);
// Full '/'-path from the hierarchy root ("grp/mesh"; the unnamed root
// contributes nothing). Shortest path that still names the node
// uniquely, or empty when even the full path is ambiguous.
std::string DagFullPath(const Scene& scene, uint32_t node);
std::string DagShortestUniquePath(const Scene& scene, uint32_t node);
// Resolve a full path, a '/'-separated suffix from any depth, or a
// bare name (unique match only).
bool ResolveDagPath(const Scene& scene, const std::string& path,
	uint32_t& out);

// Delete addressing shared by the cook replay and the editor live
// replay: uuid exact match wins; else a unique full-name match; else a
// '/'-separated ancestor path suffix ("grp/mesh", matched from any
// depth); else idFallback when it is not UINT32_MAX, still maps into
// the scene, and (for named deletes) names the same node — the
// disambiguator for duplicated names and the legacy path for nameless
// deletes. False = nothing resolved (warn-and-skip).
bool ResolveDeleteTarget(const Scene& scene, const std::string& uuid,
	const std::string& name, uint32_t idFallback, uint32_t& out);

// Override helpers: attr name mapping + ECMAScript regex match over node
// names (false on empty/bad patterns, never throws).
bool OverrideAttrFromString(const std::string& s, OverrideAttr& out);
const char* OverrideAttrName(OverrideAttr a);
bool OverridePatternMatches(const std::string& pattern,
	const std::string& nodeName);

// Apply one SetExportAttr command onto a SceneExportSettings payload
// (field-name keyed; value rides flag/name/number per the attr's type).
// The ONE mapping for the replay (ApplyTourToCluster), the editor live
// path, and the doc walk below (the same lines, so the results agree
// by construction). Unknown attrs warn-and-skip through warnOut (never
// fatal); returns false only then.
bool ApplyExportAttr(SceneExportSettings& es, const Command& c,
	std::string* warnOut = nullptr);

// Reads the scene's export settings straight off a Doc's command list:
// the FIRST createNode -at exportSettings (the singleton rule) + its
// -ref-addressed SetExportAttr lines, defaulted fields otherwise.
// Pre-replay consumers (Cook.exe/Pak.exe must merge BEFORE the
// definition build consumes the meshopt/LOD/meshlet configs): the replay
// consumes the SAME lines through the SAME mapping, so the results
// agree by construction. Returns true when the doc carries a node;
// false when not (out stays untouched). warnOut collects non-fatal
// notes (duplicates, unknown attrs, -n forms left to the replay).
bool ExportSettingsFromDoc(const Doc& doc, SceneExportSettings& out,
	std::string* warnOut = nullptr);

} // namespace smvk::tour
