#pragma once
// .tour — VulkanToy editor scene overlay (ASCII, Maya .ma flavor).
//
// A .tour file is a human-readable, versioned list of CRUD commands applied
// ON TOP of a source glTF (never a blob store: meshes/textures stay in the
// source + loose files and are referenced by id/path only). Both the Editor
// (live edits + undo) and PakCooker (--tour replay at cook time) speak it,
// so what-you-see is byte-for-byte what gets cooked:
//
//   // .tour 1 — hallway lookdev
//   source -p "content/models/Bistro/bistro.gltf" -fingerprint "9f3a...";
//   rename -n 45 "Shop_Wall";                       # update (name)
//   setAttr -n 45 -at translate -t 1.2 3.4 5.6;      # update (transform)
//   assignMaterial -n 46 -m 12;                     # update (material link)
//   setMatAttr -m 12 -at baseColor -t 1 0.5 0.2 1;   # update (mat value)
//   setNoLightmap -n 45 -v 1;                       # update (bake flag)
//   setVisibility -n 45 -v 1;                       # update (hidden flag)
//   parent -n 45 -p 12;                             # update (topology)
//   override -pattern "Building_.*" -at visibility -v 1 -enabled on;   # wide
//   override -pattern "Tree.*" -at material -m 3 -enabled on;          # wide
//   cook -outPak "content/paks/bistro.pak" ...;     # cook settings (not undo)
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
	bool lightmap = false;
	uint32_t lmDensity = 32;
	uint32_t lmLobes = 12;
	uint32_t lmAtlasWidth = 2048;
	uint64_t lmBudget = 4ull * 1024 * 1024;
	uint32_t lmRays = 256;
	float lmExcludeSmall = 0.0f;
	std::string lmExcludeKw;
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
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
};

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
	std::string source;       // referenced .gltf (owner-relative or as-is)
	std::string name;         // placeholder node name ("REF <name>")
	glm::vec3 translate{ 0.0f }; // optional placement offset
	bool hasTranslate = false;
};

struct Command {
	Verb verb = Verb::Rename;
	uint32_t node = 0;     // cluster node id (node verbs)
	uint32_t node2 = 0;    // Parent: new parent cluster node id
	uint32_t material = 0; // AssignMaterial / SetMatAttr: cluster pool index
	std::string text;      // Rename: new name; SetMatAttr: attr name
	glm::vec3 vec{ 0.0f }; // translate / rotate-deg / scale
	glm::vec4 vec4{ 1.0f }; // mat vec attr value (baseColor/emissive)
	double number = 0.0;   // mat scalar attr value
	bool flag = false;     // SetNoLightmap / SetDitherCloseup value
	std::string name;      // recorded node name (verify-only)
};

struct Doc {
	int version = kTourVersion;
	std::string source;      // source glTF path (as written by the editor)
	std::string fingerprint; // size+mtime of source at save (warn on mismatch)
	std::vector<Command> commands;
	std::vector<OverrideRule> overrides; // wide regex rules (ordered, later wins)
	// Scene references (ordered; the OWNING scene's level-1 list —
	// deeper nesting comes from each referenced source's own tour).
	std::vector<ReferenceCmd> references;
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
// material ids); name mismatches only warn.
bool ApplyTourToCluster(Cluster& cluster, const Doc& doc,
	std::vector<std::string>* warnings = nullptr, std::string* err = nullptr);

// Structural reparent on any Scene (shared by the editor live path and the
// cook replay path). Index-preserving: no nodes are added/removed, only
// parent/child/sibling links + subtree levels. Silently ignores cycles,
// the root, and depth overflow past MAX_NODE_LEVEL.
void TourReparent(Scene& scene, uint32_t node, uint32_t newParent);

// Human-readable one-liner per command (console/log use).
std::string Describe(const Command& c);
std::string DescribeOverride(const OverrideRule& r);

// Override helpers: attr name mapping + ECMAScript regex match over node
// names (false on empty/bad patterns, never throws).
bool OverrideAttrFromString(const std::string& s, OverrideAttr& out);
const char* OverrideAttrName(OverrideAttr a);
bool OverridePatternMatches(const std::string& pattern,
	const std::string& nodeName);

} // namespace smvk::tour
