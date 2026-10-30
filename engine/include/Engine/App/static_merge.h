// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Static-subtree collapse: bakes world transforms of frozen nodes and merges
// them into per-material mega-mesh blocks.
//
// Why per-material (not one mesh for everything)? The current renderer binds
// one material per node (the material table) and draws every surface in the
// mesh with it, so a single multi-material mesh would render wrong today.
// One block per material keeps the draw path correct while still collapsing
// N static draws into (number of distinct materials) draws. When there is a
// single material this is literally one mega-mesh / one draw. Each block's
// meshlet set (see Asset/meshlet.h) carries the material implicitly, which is
// exactly the granularity the future indirect/meshlet pass wants.

#include <cstdint>
#include <string>
#include <vector>

#include <Engine/Scene/scene.h>

namespace smvk {

struct Cluster;

struct StaticMergeOptions {
	// Remove the source static nodes after baking (their geometry lives on
	// in the merged blocks). When false the sources are kept untouched.
	bool deleteSources = true;
	// URI prefix for merged meshes, e.g. "<merged>#static".
	std::string uriPrefix = "<merged>#static";
};

struct MergedBlock {
	uint32_t materialPoolIndex = 0; // source materials pool index (or UINT32_MAX when unassigned)
	bool hasMaterial = false;
	uint32_t srcNodeCount = 0;
	int node = -1; // new scene node owning the block
	uint32_t meshPoolIndex = 0; // staticMeshes pool index of the block
	std::string uri;
};

struct StaticMergeResult {
	bool ok = false;
	std::string err;
	std::vector<MergedBlock> blocks;
	uint32_t srcNodesConsumed = 0;
};

// Collects renderable static nodes (staticNode != 0 && a mesh row present).
// NOTE: Scene lives in the global namespace (see Scene/scene.h).
std::vector<uint32_t> CollectStaticRenderNodes(const ::Scene& scene);

// Bakes globals (caller should have fresh globalTransform) and merges every
// static renderable node, grouped by material pool index. Requires retained
// CPU uploads (offline clusters, or runtime clusters imported with
// SetRetainUploads(true)); fails otherwise. New block nodes are added as
// children of node 0 with identity local transforms (vertices are
// world-baked) and marked static.
StaticMergeResult CollapseStaticSubtrees(
	Cluster& cluster,
	const StaticMergeOptions& opts = StaticMergeOptions{});

} // namespace smvk
