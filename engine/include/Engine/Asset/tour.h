#pragma once
// .tour — VulkanToy editor scene overlay (ASCII, Maya .ma flavor).
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
//   fileInfo -k "application" -v "vulkantoy-tour";            # constants
//   fileInfo -k "product" -v "VulkanToy";
//   fileInfo -k "version" -v "1";
//   rename -n 45 "Shop_Wall";                       # update (name)
//   setAttr -n 45 -at translate -type "double3" -t 1.2 3.4 5.6;
//   assignMaterial -n 46 -m 12;                     # update (material link)
//   setMatAttr -m 12 -at baseColor -type "float4" -t 1 0.5 0.2 1;
//   setNoLightmap -n 45 -type "bool" -v 1;          # update (bake flag)
//   setVisibility -n 45 -type "bool" -v 1;          # update (hidden flag)
//   parent -n 45 -p 12;                             # update (topology)
//   delete -uuid 9f3a... -n 45 -name "Shop_Wall";   # remove (uuid/name/id)
//   createLight -p 0 -name "Spot_1" -at spot -t 1 2 3 1500 25 20 35 0 1;
//       setLightAttr -ref 0 -at intensity -v 1500;  # tabulated: -ref
//                                                   # props ride under
//                                                   # their create
//   override -pattern "Building_.*" -at visibility -v 1 -enabled on;   # wide
//   override -pattern "Tree.*" -at material -m 3 -enabled on;          # wide
//   cook -outPak "" ...;   # cook settings (destination NOT persisted)
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

namespace smvk {
struct Cluster;
} // namespace smvk

struct Scene;

namespace smvk::tour {

inline constexpr int kTourVersion = 1;

// Cook settings interchange (mirrors PakCooker's bake flags; the editor
// edits them in the cook panel, PakCooker consumes them as defaults under
// --tour unless its own CLI flags override — CLI wins).
struct TourCook {
	std::string outPak;
	bool meshopt = true;
	bool lods = false;
	// LOD chain detail (PakCooker --lod-ratios/--lod-error): the
	// comma-separated ratio list and the simplification target error.
	std::string lodRatios = "0.6,0.3,0.12";
	float lodError = 0.02f;
	bool lightmap = false;
	// Chart density request, METERS PER TEXEL (0.25 = one texel per
	// 25 cm). Legacy -lmDensity lines (texels/m) convert on load.
	float lmMetersPerTexel = 0.25f;
	uint32_t lmLobes = 12;
	// FIXED atlas width; the atlas height grows with the content.
	uint32_t lmAtlasWidth = 1024;
	uint32_t lmRays = 256;
	// Voxel-remesh scene proxy resolution (meshopt_remesh grid [4..256];
	// 0 = legacy per-mesh LOD0 path, kept for A/B only).
	int lmProxyRes = 256;
	float lmExcludeSmall = 0.0f;
	std::string lmExcludeKw;
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
	// World-sun color (linear RGB): with the angles/intensity above it IS
	// the cooked world sun — the lightmap bake env AND the pak runtime
	// sun (the cooker synthesizes a directional from it when the cluster
	// has none, so editor world-sun settings travel to the App).
	glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
	// Preconvolved specular cubemaps (PakCooker --cubemap stage: GGX
	// mip chains embedded as textures/cubemaps/<name>.ktx2; empty name
	// defaults to the out-pak stem).
	bool cubemap = false;
	std::string cubemapName;
	uint32_t cubemapSize = 128;
	uint32_t cubemapRays = 128;
	// Meshlet cook (PakCooker --meshlets stage): per-meshlet build
	// over the cooked topology (max verts/tris per meshlet + the
	// cone-weight partitioning bias, 0..1).
	bool meshlets = false;
	uint32_t meshletMaxVerts = 64;
	uint32_t meshletMaxTris = 124;
	float meshletConeWeight = 0.0f;
	// GI bakes (cook dialog, editor-driven): authored convex volumes
	// (GiBaker cvol stage, gi/<pak-stem>_* sidecars) and the adaptive
	// probe octree (GiBaker --out, gi/<pak-stem>.sggi — the runtime's
	// global fallback outside any convex volume).
	bool giCvol = true;
	bool giWorld = true;
	// Auto-convert non-KTX textures to KTX2 at cook time (in-process
	// stb decode + libktx write, `.ktx2` sidecar cached next to the
	// source). Off restores the old warn-and-skip for non-KTX payloads.
	bool autoKtx = true;
};

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

enum class Verb : uint8_t {
	Rename,
	SetTranslate,
	SetRotate, // degrees XYZ, same convention as the editor inspector
	SetScale,
	AssignMaterial, // material = cluster material POOL index
	SetMatAttr,     // text = attr name; vec/vec4/number by attr
	SetNoLightmap,  // flag
	SetDitherCloseup, // flag (subtree dither on camera closeup)
	SetVisibility,  // flag: hidden self state (hierarchical, see Scene::hidden)
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
  	                //   terrain, cvolume, cubemap, camera. Replayed by
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
  	               // (kCreateCubemapRefBase | k — see below); text =
  	               // attr ("position","radius","name"); number =
  	               // scalar (radius); vec4 = triple (position); name =
  	               // new name (attr "name").
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
	DitherCloseup, // flag
};

struct OverrideRule {
	OverrideAttr attr = OverrideAttr::Visibility;
	std::string pattern; // ECMAScript regex over node names
	bool flag = false;   // Visibility/NoLightmap/Dither value
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
	bool flag = false;     // SetNoLightmap / SetDitherCloseup value;
	                       // CreateLight: static bake-only;
	                       // SetLightAttr static/shadow value
	std::string name;      // recorded node name (verify-only);
	                       // SetLightAttr "name": the NEW name
	                       // Delete: secondary addressing key
	std::string uuid;      // SetUuid: the recorded 32-hex identity;
	                       // Delete: its PRIMARY addressing key
 	std::string nodeType;  // createNode -at type ("transform",
 	                       // "pointLight", "spotLight", "terrain",
 	                       // "cvolume", "material", "file", "image",
 	                       // "camera")
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
	// fileInfo constants (Maya fileInfo block): fixed format metadata
	// (application/product/version), then anything carried. Written
	// right after the references.
	std::vector<std::pair<std::string, std::string>> fileInfo;
	// Editor-only settings (editorSet lines): viewport/env authoring state
	// the EDITOR persists for itself. NEVER applied by ApplyTourToCluster
	// — the cook silently ignores this whole section (editor IBL and
	// friends must not leak into paks).
	std::vector<std::pair<std::string, std::string>> editorSettings;
	TourCook cook;
	bool hasCook = false;
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
bool ApplyTourToCluster(Cluster& cluster, const Doc& doc,
	std::vector<std::string>* warnings = nullptr, std::string* err = nullptr,
	bool strict = true);

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

} // namespace smvk::tour
