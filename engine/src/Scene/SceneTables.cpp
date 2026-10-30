// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Typed scene-table maintenance (Epoch 3 of the xForNode migration —
// docs/scene_tables_migration.md). Handwritten per-kind functions, no
// templates: the near-identical bodies below are deliberate (the
// migration rules: explicit code over infrastructure — a FindRow<T>
// would be the first step toward a framework).
//
// The tables are INTRODUCED but not yet authoritative — the
// xForNode maps stay the source of truth until Epochs 4-5 migrate the
// consumers. Nothing here reads or writes the maps.

#include <cassert>

#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>

namespace {

// rowsForNode must cover every live node. Lazily grown (addNode does
// not touch it — Scene is a plain aggregate); every entry starts
// all-InvalidRow (NodeRows' defaults).
void EnsureNodeRows(Scene& scene)
{
	if (scene.rowsForNode.size() < scene.hierarchy.size())
		scene.rowsForNode.resize(scene.hierarchy.size());
}

bool NodeInRange(const Scene& scene, NodeId node)
{
	return size_t(node) < scene.hierarchy.size();
}

} // namespace

void RebuildNodeRows(Scene& scene)
{
	// EXACT resize — grows AND shrinks: the derived index always
	// matches the live node count after a rebuild (the mutators'
	// EnsureNodeRows is grow-only by design; a rebuild is the full
	// reconciliation, including after compacting node deletions).
	scene.rowsForNode.resize(scene.hierarchy.size());
	for (NodeRows& rows : scene.rowsForNode)
		rows = NodeRows{};
	// One pass per table. Rows whose node is out of range are DEAD
	// (the node count shrank past them — a compacting deletion) and
	// get compacted out here: they are unreachable through the index
	// by construction, and "a table guarantees valid nodes" is one of
	// the migration invariants. NOTE: node-id REMAPPING on compaction
	// (deleteSceneNodes shifts every later node) is Epoch 6's contract
	// — this epoch never compacts nodes under live tables.
	for (size_t i = 0; i < scene.tables.meshes.size();) {
		MeshRow& row = scene.tables.meshes[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.meshes.back();
			scene.tables.meshes.pop_back();
			continue; // the moved-in row re-checks at this index
		}
		scene.rowsForNode[row.node].mesh = RowId(i);
		++i;
	}
	for (size_t i = 0; i < scene.tables.materials.size();) {
		MaterialRow& row = scene.tables.materials[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.materials.back();
			scene.tables.materials.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].material = RowId(i);
		++i;
	}
	for (size_t i = 0; i < scene.tables.lights.size();) {
		LightRow& row = scene.tables.lights[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.lights.back();
			scene.tables.lights.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].light = RowId(i);
		++i;
	}
	for (size_t i = 0; i < scene.tables.cameras.size();) {
		CameraRow& row = scene.tables.cameras[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.cameras.back();
			scene.tables.cameras.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].camera = RowId(i);
		++i;
	}
	for (size_t i = 0; i < scene.tables.decals.size();) {
		DecalRow& row = scene.tables.decals[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.decals.back();
			scene.tables.decals.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].decal = RowId(i);
		++i;
	}
}

// ---- mesh ----

MeshRow* FindMesh(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].mesh;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.meshes.size());
	return &scene.tables.meshes[rid];
}

const MeshRow* FindMesh(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].mesh;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.meshes.size());
	return &scene.tables.meshes[rid];
}

bool SetMesh(Scene& scene, NodeId node, uint32_t mesh)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	// At most one row per node: Set REPLACES (duplicate rejection with
	// replacement semantics — the map-insert-overwrite behavior).
	if (MeshRow* row = FindMesh(scene, node)) {
		row->mesh = mesh;
		return true;
	}
	scene.tables.meshes.push_back(MeshRow{ node, mesh });
	scene.rowsForNode[node].mesh = RowId(scene.tables.meshes.size() - 1);
	return true;
}

bool RemoveMesh(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].mesh;
	if (rid == InvalidRow)
		return false;
	// Swap-and-pop + lookup repair: the LAST row moves into the freed
	// slot; its NodeRows entry re-points at the slot it now occupies.
	// Dense stays dense, removal is O(1), only one index entry changes.
	scene.tables.meshes[rid] = scene.tables.meshes.back();
	scene.tables.meshes.pop_back();
	if (size_t(rid) < scene.tables.meshes.size()) {
		const NodeId moved = scene.tables.meshes[rid].node;
		scene.rowsForNode[moved].mesh = rid;
	}
	scene.rowsForNode[node].mesh = InvalidRow;
	return true;
}

// ---- material (independent of the mesh row — see scene_tables.h) ----

MaterialRow* FindMaterial(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].material;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.materials.size());
	return &scene.tables.materials[rid];
}

const MaterialRow* FindMaterial(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].material;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.materials.size());
	return &scene.tables.materials[rid];
}

bool SetMaterial(Scene& scene, NodeId node, uint32_t material)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (MaterialRow* row = FindMaterial(scene, node)) {
		row->material = material;
		return true;
	}
	scene.tables.materials.push_back(MaterialRow{ node, material });
	scene.rowsForNode[node].material = RowId(scene.tables.materials.size() - 1);
	return true;
}

bool RemoveMaterial(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].material;
	if (rid == InvalidRow)
		return false;
	scene.tables.materials[rid] = scene.tables.materials.back();
	scene.tables.materials.pop_back();
	if (size_t(rid) < scene.tables.materials.size()) {
		const NodeId moved = scene.tables.materials[rid].node;
		scene.rowsForNode[moved].material = rid;
	}
	scene.rowsForNode[node].material = InvalidRow;
	return true;
}

// ---- light ----

LightRow* FindLight(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].light;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.lights.size());
	return &scene.tables.lights[rid];
}

const LightRow* FindLight(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].light;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.lights.size());
	return &scene.tables.lights[rid];
}

bool SetLight(Scene& scene, NodeId node, uint32_t light)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (LightRow* row = FindLight(scene, node)) {
		row->light = light;
		return true;
	}
	scene.tables.lights.push_back(LightRow{ node, light });
	scene.rowsForNode[node].light = RowId(scene.tables.lights.size() - 1);
	return true;
}

bool RemoveLight(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].light;
	if (rid == InvalidRow)
		return false;
	scene.tables.lights[rid] = scene.tables.lights.back();
	scene.tables.lights.pop_back();
	if (size_t(rid) < scene.tables.lights.size()) {
		const NodeId moved = scene.tables.lights[rid].node;
		scene.rowsForNode[moved].light = rid;
	}
	scene.rowsForNode[node].light = InvalidRow;
	return true;
}

// ---- camera ----

CameraRow* FindCamera(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].camera;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.cameras.size());
	return &scene.tables.cameras[rid];
}

const CameraRow* FindCamera(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].camera;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.cameras.size());
	return &scene.tables.cameras[rid];
}

bool SetCamera(Scene& scene, NodeId node, uint32_t camera)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (CameraRow* row = FindCamera(scene, node)) {
		row->camera = camera;
		return true;
	}
	scene.tables.cameras.push_back(CameraRow{ node, camera });
	scene.rowsForNode[node].camera = RowId(scene.tables.cameras.size() - 1);
	return true;
}

bool RemoveCamera(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].camera;
	if (rid == InvalidRow)
		return false;
	scene.tables.cameras[rid] = scene.tables.cameras.back();
	scene.tables.cameras.pop_back();
	if (size_t(rid) < scene.tables.cameras.size()) {
		const NodeId moved = scene.tables.cameras[rid].node;
		scene.rowsForNode[moved].camera = rid;
	}
	scene.rowsForNode[node].camera = InvalidRow;
	return true;
}

// ---- decal ----

DecalRow* FindDecal(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].decal;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.decals.size());
	return &scene.tables.decals[rid];
}

const DecalRow* FindDecal(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].decal;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.decals.size());
	return &scene.tables.decals[rid];
}

bool SetDecal(Scene& scene, NodeId node, uint32_t decal)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (DecalRow* row = FindDecal(scene, node)) {
		row->decal = decal;
		return true;
	}
	scene.tables.decals.push_back(DecalRow{ node, decal });
	scene.rowsForNode[node].decal = RowId(scene.tables.decals.size() - 1);
	return true;
}

bool RemoveDecal(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].decal;
	if (rid == InvalidRow)
		return false;
	scene.tables.decals[rid] = scene.tables.decals.back();
	scene.tables.decals.pop_back();
	if (size_t(rid) < scene.tables.decals.size()) {
		const NodeId moved = scene.tables.decals[rid].node;
		scene.rowsForNode[moved].decal = rid;
	}
	scene.rowsForNode[node].decal = InvalidRow;
	return true;
}

// ---- all kinds at once (the node-deletion hook the editor/tour paths
// adopt in Epoch 5) ----

bool RemoveNodeRows(Scene& scene, NodeId node)
{
	bool removed = false;
	removed |= RemoveMesh(scene, node);
	removed |= RemoveMaterial(scene, node);
	removed |= RemoveLight(scene, node);
	removed |= RemoveCamera(scene, node);
	removed |= RemoveDecal(scene, node);
	return removed;
}
