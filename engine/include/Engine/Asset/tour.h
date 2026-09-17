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
//   parent -n 45 -p 12;                             # update (topology)
//   cook -outPak "content/paks/bistro.pak" ...;     # cook settings (not undo)
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
	Parent,         // node2 = new parent (cluster node id)
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
	bool flag = false;     // SetNoLightmap value
	std::string name;      // recorded node name (verify-only)
};

struct Doc {
	int version = kTourVersion;
	std::string source;      // source glTF path (as written by the editor)
	std::string fingerprint; // size+mtime of source at save (warn on mismatch)
	std::vector<Command> commands;
	TourCook cook;
	bool hasCook = false;
};

// Best-effort source fingerprint ("<size>:<mtime>" hex); empty when the
// file is missing.
std::string Fingerprint(const std::string& path);

bool SaveTour(const std::string& path, const Doc& doc, std::string& err);
bool LoadTour(const std::string& path, Doc& doc, std::string& err,
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

} // namespace smvk::tour
