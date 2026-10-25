// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Pak as a library (the editor links it; the CLI stays a thin user
// interface over the same Run). PakArgs mirrors the FULL CLI surface:
// every --flag has a field here and the CLI main maps its parsed Args
// 1:1 (the original lib split mapped only 6 fields — --level,
// --dump-pak, verify, cubemap knobs silently no-op'd; that lossy map
// is what this struct kills). Embedders (the editor) fill the subset
// they drive; every other field keeps the CLI default.
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace paktool {

struct PakArgs {
	std::string contentDir = "content";
	std::vector<std::string> inVfsList; // processed .geometry artifacts
	std::string inVfs; // primary identity (first artifact / tour ref)
	std::string tour; // definition tour (editor scene overlay)
	std::string registryDir; // explicit programmatic registry root
	                         // (editor store); bare CLI packs derive it
	                         // from the content root
	std::string outPak;
	// Read-only inspection (--dump-pak): print the pak manifest, no
	// cooking, no writes. dumpNodes pins the scene node print.
	std::string dumpPak;
	std::vector<uint32_t> dumpNodes;
	bool dumpManifest = false;
	// Re-open the written pak with PakReader after writing (default on).
	bool verify = true;
	// Per-blob LZ4 compression (default on: only stores when it shrinks).
	bool compress = true;
	int lz4Accel = 1;
	bool meshopt = true;
	float overdraw = 1.05f;
	bool autoKtx = true;
	// SKIP the loose VFS bake-file discovery at assembly (-1 = the tour's
	// exportSettings node decides — the SceneExportSettings::ignoreVfs
	// default; 0/1 = CLI-pinned, e.g. the editor passes the node's value
	// through here). On = the pak embeds ONLY the scene's own content
	// (meshes/materials/textures, blobs by path): no gi/ cubemap
	// manifests + KTX rows, no .sggi, no .cvmanifest + .cvol rows, no
	// bindings for them (see SceneExportSettings::ignoreVfs).
	int ignoreVfs = -1;
	// Level blob (see Engine/Asset/level.h): pack the scene + the baked
	// data into ONE levels/<name>.level blob. Empty = no level blob.
	// (No cubemap knobs here: PAK never bakes captures — cook.exe does;
	// pak embeds the loose bake files it discovers at the canonical
	// gi/cubemaps/ paths.)
	std::string levelName;
	// Tiled terrain source dir (host path; see Rendergraph/terrain_meta.h).
	std::string terrainDir;
	// Sun/sky bake env (the CLI sun defaults; the tour env block wins
	// when the definition carries one — see EnvForBake).
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
	glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
	// Probe-volume bake files this pak's scenes sample (repeatable).
	std::vector<std::string> giProbes;
};

// Byte-identical to the CLI run (same return codes). Prints go through
// tool::out()/tool::err() so embedders can capture lines via
// tool::SetLineSink.
int Run(const PakArgs& args);

} // namespace paktool
