// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Definition-tour -> frozen World (single offline cluster, unresolved).
// Factors the identical tour bootstrap in GiBaker / Cook / PakCooker:
// empty root + reference bind + texture/material asset imports +
// command replay (HOST-path resolution, editor parity). Bakers take the
// built World; the tour file never reaches them ("tour -> world ->
// baker"). Per-tool extras (cook-default merge, stems, artifact paths)
// stay in the tools: this covers the shared definition build only.
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>

namespace smvk {
class Cluster;
class World;
struct MeshletConfig;
namespace tour {
struct Doc;
struct FileNode;
} // namespace tour
} // namespace smvk

struct GltfArtifactCache;
struct MeshOptConfig;

namespace tourworld {

struct BuildOptions {
	std::string contentDir; // record resolution root (may be empty)
	bool importTextureAssets = false;  // tour.textureAssets records
	bool importMaterialAssets = false; // tour.materialAssets records
	bool autoKtx = true;
	const std::vector<smvk::tour::FileNode>* fileNodes = nullptr;
	GltfArtifactCache* artifactCache = nullptr; // scratch hook, nullable
	smvk::MeshOptConfig meshOpt{};
	smvk::MeshletConfig meshlets{};
};

struct BuiltWorld {
	std::unique_ptr<smvk::World> world;
	size_t commands = 0; // tour commands replayed
	size_t refsApplied = 0; // reference subtrees bound
	size_t refsTopLevel = 0; // top-level references in the doc
};

// Replays doc into a fresh offline World (single cluster, unresolved —
// cluster-space ids, exactly what bakers consume). Prints the legacy
// definition lines through tool::out()/tool::err() (console by default,
// editor feed when sunk). Null on failure (warnings appended).
std::unique_ptr<BuiltWorld> BuildWorldFromDoc(
	const smvk::tour::Doc& doc, const BuildOptions& opt,
	std::vector<std::string>& warnings, std::string& err);

} // namespace tourworld
