// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Typed scene-table maintenance (Epoch 3 of the xForNode migration —
// docs/scene_tables_migration.md). Handwritten per-kind functions, no
// templates: the near-identical bodies below are deliberate (the
// migration rules: explicit code over infrastructure — a FindRow<T>
// would be the first step toward a framework).
//
// AUTHORITATIVE since Epochs 4-5 (every xForNode map is gone). Since
// Epoch 6b rows address STABLE SLOTS: a row's node must be a live
// slot (NodeInRange checks range + liveness), deletion retires slots
// and drops their rows, and ids never renumber.

#include <cassert>
#include <string>
#include <utility>

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

// Slot liveness for the association mutators (Epoch 6b): a row may
// only ever point at a LIVE slot — range alone is not validity
// anymore (a retired slot is in range but dead; SetX on it would
// mint a row nothing can address once the slot is reused).
bool NodeInRange(const Scene& scene, NodeId node)
{
	return size_t(node) < scene.hierarchy.size() && IsLiveSlot(scene, node);
}

} // namespace

void RebuildNodeRows(Scene& scene)
{
	// EXACT resize — grows AND shrinks to the SLOT count (hierarchy
	// size; retirement keeps holes, so this is capacity, not the live
	// node count). The mutators' EnsureNodeRows is grow-only by
	// design; a rebuild is the full reconciliation.
	scene.rowsForNode.resize(scene.hierarchy.size());
	for (NodeRows& rows : scene.rowsForNode)
		rows = NodeRows{};
	// One pass per table. Rows whose node is dead (retired —
	// NodeInRange checks liveness, Epoch 6b) are unreachable through
	// the index by construction and get compacted out here: "a table
	// guarantees live nodes" is one of the migration invariants. The
	// retirement paths drop rows themselves (RemoveNodeRows); this
	// rebuild is the backstop for everything else.
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
	for (size_t i = 0; i < scene.tables.giVolumes.size();) {
		GiVolumeRow& row = scene.tables.giVolumes[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.giVolumes.back();
			scene.tables.giVolumes.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].giVolume = RowId(i);
		++i;
	}
	for (size_t i = 0; i < scene.tables.cubemaps.size();) {
		CubemapRow& row = scene.tables.cubemaps[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.cubemaps.back();
			scene.tables.cubemaps.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].cubemap = RowId(i);
		++i;
	}
	for (size_t i = 0; i < scene.tables.exports.size();) {
		ExportRow& row = scene.tables.exports[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.exports.back();
			scene.tables.exports.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].exportSettings = RowId(i);
		++i;
	}
	for (size_t i = 0; i < scene.tables.envs.size();) {
		EnvRow& row = scene.tables.envs[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.envs.back();
			scene.tables.envs.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].env = RowId(i);
		++i;
	}
	for (size_t i = 0; i < scene.tables.envVolumes.size();) {
		EnvVolumeRow& row = scene.tables.envVolumes[i];
		if (!NodeInRange(scene, row.node)) {
			row = scene.tables.envVolumes.back();
			scene.tables.envVolumes.pop_back();
			continue;
		}
		scene.rowsForNode[row.node].envVolume = RowId(i);
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

// ---- gi volume (inline payload) ----

GiVolumeRow* FindGiVolume(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].giVolume;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.giVolumes.size());
	return &scene.tables.giVolumes[rid];
}

const GiVolumeRow* FindGiVolume(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].giVolume;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.giVolumes.size());
	return &scene.tables.giVolumes[rid];
}

bool SetGiVolume(Scene& scene, NodeId node, const SceneGiVolume& volume)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (GiVolumeRow* row = FindGiVolume(scene, node)) {
		row->volume = volume;
		return true;
	}
	GiVolumeRow row{};
	row.node = node;
	row.volume = volume;
	scene.tables.giVolumes.push_back(std::move(row));
	scene.rowsForNode[node].giVolume = RowId(scene.tables.giVolumes.size() - 1);
	return true;
}

bool RemoveGiVolume(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].giVolume;
	if (rid == InvalidRow)
		return false;
	// The payload rows own strings — move the back row in (the source
	// is popped right after; the indirection rows above are POD where
	// a move IS the copy).
	scene.tables.giVolumes[rid] = std::move(scene.tables.giVolumes.back());
	scene.tables.giVolumes.pop_back();
	if (size_t(rid) < scene.tables.giVolumes.size()) {
		const NodeId moved = scene.tables.giVolumes[rid].node;
		scene.rowsForNode[moved].giVolume = rid;
	}
	scene.rowsForNode[node].giVolume = InvalidRow;
	return true;
}

// ---- cubemap pin (inline payload) ----

CubemapRow* FindCubemap(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].cubemap;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.cubemaps.size());
	return &scene.tables.cubemaps[rid];
}

const CubemapRow* FindCubemap(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].cubemap;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.cubemaps.size());
	return &scene.tables.cubemaps[rid];
}

bool SetCubemap(Scene& scene, NodeId node, const SceneCubemap& cubemap)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (CubemapRow* row = FindCubemap(scene, node)) {
		row->cubemap = cubemap;
		return true;
	}
	CubemapRow row{};
	row.node = node;
	row.cubemap = cubemap;
	scene.tables.cubemaps.push_back(std::move(row));
	scene.rowsForNode[node].cubemap = RowId(scene.tables.cubemaps.size() - 1);
	return true;
}

bool RemoveCubemap(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].cubemap;
	if (rid == InvalidRow)
		return false;
	scene.tables.cubemaps[rid] = std::move(scene.tables.cubemaps.back());
	scene.tables.cubemaps.pop_back();
	if (size_t(rid) < scene.tables.cubemaps.size()) {
		const NodeId moved = scene.tables.cubemaps[rid].node;
		scene.rowsForNode[moved].cubemap = rid;
	}
	scene.rowsForNode[node].cubemap = InvalidRow;
	return true;
}

// ---- export settings (inline payload) ----

ExportRow* FindExport(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].exportSettings;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.exports.size());
	return &scene.tables.exports[rid];
}

const ExportRow* FindExport(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].exportSettings;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.exports.size());
	return &scene.tables.exports[rid];
}

bool SetExport(Scene& scene, NodeId node, const SceneExportSettings& settings)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (ExportRow* row = FindExport(scene, node)) {
		row->settings = settings;
		return true;
	}
	ExportRow row{};
	row.node = node;
	row.settings = settings;
	scene.tables.exports.push_back(std::move(row));
	scene.rowsForNode[node].exportSettings = RowId(scene.tables.exports.size() - 1);
	return true;
}

bool RemoveExport(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].exportSettings;
	if (rid == InvalidRow)
		return false;
	scene.tables.exports[rid] = std::move(scene.tables.exports.back());
	scene.tables.exports.pop_back();
	if (size_t(rid) < scene.tables.exports.size()) {
		const NodeId moved = scene.tables.exports[rid].node;
		scene.rowsForNode[moved].exportSettings = rid;
	}
	scene.rowsForNode[node].exportSettings = InvalidRow;
	return true;
}

// ---- environment block (inline payload) ----

EnvRow* FindEnv(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].env;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.envs.size());
	return &scene.tables.envs[rid];
}

const EnvRow* FindEnv(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].env;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.envs.size());
	return &scene.tables.envs[rid];
}

bool SetEnv(Scene& scene, NodeId node, const SceneEnv& environment)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (EnvRow* row = FindEnv(scene, node)) {
		row->environment = environment;
		return true;
	}
	EnvRow row{};
	row.node = node;
	row.environment = environment;
	scene.tables.envs.push_back(std::move(row));
	scene.rowsForNode[node].env = RowId(scene.tables.envs.size() - 1);
	return true;
}

bool RemoveEnv(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].env;
	if (rid == InvalidRow)
		return false;
	scene.tables.envs[rid] = std::move(scene.tables.envs.back());
	scene.tables.envs.pop_back();
	if (size_t(rid) < scene.tables.envs.size()) {
		const NodeId moved = scene.tables.envs[rid].node;
		scene.rowsForNode[moved].env = rid;
	}
	scene.rowsForNode[node].env = InvalidRow;
	return true;
}

// ---- env volume (inline payload) ----

EnvVolumeRow* FindEnvVolume(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].envVolume;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.envVolumes.size());
	return &scene.tables.envVolumes[rid];
}

const EnvVolumeRow* FindEnvVolume(const Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return nullptr;
	const RowId rid = scene.rowsForNode[node].envVolume;
	if (rid == InvalidRow)
		return nullptr;
	assert(size_t(rid) < scene.tables.envVolumes.size());
	return &scene.tables.envVolumes[rid];
}

bool SetEnvVolume(Scene& scene, NodeId node, const EnvVolume& volume)
{
	if (!NodeInRange(scene, node))
		return false;
	EnsureNodeRows(scene);
	if (EnvVolumeRow* row = FindEnvVolume(scene, node)) {
		row->volume = volume;
		return true;
	}
	EnvVolumeRow row{};
	row.node = node;
	row.volume = volume;
	scene.tables.envVolumes.push_back(std::move(row));
	scene.rowsForNode[node].envVolume = RowId(scene.tables.envVolumes.size() - 1);
	return true;
}

bool RemoveEnvVolume(Scene& scene, NodeId node)
{
	if (!NodeInRange(scene, node) || node >= scene.rowsForNode.size())
		return false;
	const RowId rid = scene.rowsForNode[node].envVolume;
	if (rid == InvalidRow)
		return false;
	scene.tables.envVolumes[rid] = std::move(scene.tables.envVolumes.back());
	scene.tables.envVolumes.pop_back();
	if (size_t(rid) < scene.tables.envVolumes.size()) {
		const NodeId moved = scene.tables.envVolumes[rid].node;
		scene.rowsForNode[moved].envVolume = rid;
	}
	scene.rowsForNode[node].envVolume = InvalidRow;
	return true;
}
// ---- dense node metadata (name/uuid) ----

void EnsureNodeMetadata(Scene& scene)
{
	if (scene.metadata.size() < scene.hierarchy.size())
		scene.metadata.resize(scene.hierarchy.size());
}

uint32_t GetNodeNameIndex(const Scene& scene, NodeId node)
{
	if (node >= scene.metadata.size())
		return 0xFFFFFFFFu;
	return scene.metadata[node].name;
}

void SetNodeNameIndex(Scene& scene, NodeId node, uint32_t nameIndex)
{
	if (size_t(node) >= scene.hierarchy.size())
		return;
	EnsureNodeMetadata(scene);
	scene.metadata[node].name = nameIndex;
}

const std::string& GetNodeUuid(const Scene& scene, NodeId node)
{
	static const std::string kEmpty;
	if (node >= scene.metadata.size())
		return kEmpty;
	return scene.metadata[node].uuid;
}

void SetNodeUuidRaw(Scene& scene, NodeId node, const std::string& uuid)
{
	if (size_t(node) >= scene.hierarchy.size())
		return;
	EnsureNodeMetadata(scene);
	scene.metadata[node].uuid = uuid;
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
	removed |= RemoveGiVolume(scene, node);
	removed |= RemoveCubemap(scene, node);
	removed |= RemoveExport(scene, node);
	removed |= RemoveEnv(scene, node);
	removed |= RemoveEnvVolume(scene, node);
	return removed;
}
