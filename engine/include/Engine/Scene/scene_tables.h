// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Typed scene data tables — the target representation of the
// xForNode-map migration (docs/scene_tables_migration.md, Epoch 3).
//
// THE MODEL (the migration's locked decisions):
//   * One DENSE table per authored association kind; each row carries
//     its owning node + the cluster resource index (or inline payload,
//     from Epoch 5 on). Rows are trivially copyable POD — no strings
//     inside rows (Epoch 5's inline payloads take StringRef offsets
//     into a flat string arena), no constructors, no ownership.
//   * ONE row per node per table (the current at-most-one map
//     semantics); different tables coexist on the same node freely —
//     a node is not typed, it ASSOCIATES.
//   * `Scene::rowsForNode` is a DERIVED per-node index over the tables
//     — an acceleration structure, never a second source of truth.
//     Rebuild it after any direct table manipulation; the mutators
//     below keep it incrementally correct themselves.
//   * In-memory invalid/sentinel is UINT32_MAX (the house convention —
//     bindless indices, UINT32_MAX handles everywhere). The COOKED
//     per-node table (Epoch 7) uses 0 = absent instead, so a
//     zero-filled region reads as "no association"; the ±1 conversion
//     lives only inside the handwritten writer/reader pair.
//
// DELETION POLICY: swap-and-pop with lookup repair (dense tables stay
// dense, removal is O(1), the moved row's NodeRows entry is fixed up
// in place). Deterministic COOKING (Epoch 7+) writes rows in node
// order regardless of live storage order — the writer sorts, the
// tables don't.
//
// NO templates, NO registration, NO reflection: each table kind gets
// its own handwritten row type, mutators, and (later) serializer +
// inspector. Adding a kind = writing its explicit code (the migration
// rules, docs/scene_tables_migration.md).

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <Engine/Scene/scene_node_id.h>
#include <Engine/Scene/scene_payloads.h>

// Row index into a SceneTables member vector. InvalidRow = no row.
using RowId = uint32_t;
constexpr RowId InvalidRow = 0xFFFFFFFFu;

// Node -> mesh association. `mesh` is the owning Cluster's
// ClusterStaticMeshHandle index (cluster scene) or the world
// meshLocations index (merged world scene) — exactly what
// meshForNode stored; the ownership model is unchanged.
// NOTE: material is NOT folded in here. The Epoch 4 audit proved
// material associations exist WITHOUT meshes (tour assignMaterial on
// bare nodes — pinned by test_tour's "replay assign" — and gltf's
// instancing path can set a mesh with no material): two independent
// maps became two independent tables, one owner per fact.
struct MeshRow {
	NodeId node = 0;
	uint32_t mesh = 0xFFFFFFFFu;
};

// Node -> material association (the materialForNode override —
// independent of any mesh row on the same node).
struct MaterialRow {
	NodeId node = 0;
	uint32_t material = 0xFFFFFFFFu;
};

// Node -> light association (the owning Cluster's lights pool index —
// what lightForNode stores today).
struct LightRow {
	NodeId node = 0;
	uint32_t light = 0xFFFFFFFFu;
};

// Node -> camera association (the owning Cluster's cameras pool index).
struct CameraRow {
	NodeId node = 0;
	uint32_t camera = 0xFFFFFFFFu;
};

// Node -> decal association (the owning Cluster's decals pool index).
struct DecalRow {
	NodeId node = 0;
	uint32_t decal = 0xFFFFFFFFu;
};

// ---- inline-payload rows (Epoch 5) ----
// These carry the AUTHORED PAYLOAD VALUE itself (no pool indirection —
// the definitions are node-local). The indirection rows above are
// trivially copyable; these are plain copyable structs whose payloads
// may hold std::string (the AUTHORING-side structs, see
// scene_payloads.h) — the COOKED representation (Epoch 7) is a
// separate field-wise layout with StringRefs into a string arena,
// never these objects as bytes. Same one-row-per-node contract.

// Node -> authored GI volume (was giVolumeForNode).
struct GiVolumeRow {
	NodeId node = 0;
	SceneGiVolume volume{};
};

// Node -> authored cubemap capture pin (was cubemapForNode).
struct CubemapRow {
	NodeId node = 0;
	SceneCubemap cubemap{};
};

// Node -> export/cook settings (was exportForNode; deliberately
// editor/cook-only — never packed into runtime content, preserved
// behavior from the map era).
struct ExportRow {
	NodeId node = 0;
	SceneExportSettings settings{};
};

// Node -> authored environment look block (was envForNode).
struct EnvRow {
	NodeId node = 0;
	SceneEnv environment{};
};

// Node -> authored env-volume shape (was envVolumeForNode).
struct EnvVolumeRow {
	NodeId node = 0;
	EnvVolume volume{};
};

// The row contract for the future pak chunks (Epoch 7): every row is
// trivially copyable with a static layout — the writer/reader pair
// pins field order + widths explicitly, and these asserts prove the
// compiler layout stays the plain data the disk format assumes.
static_assert(std::is_trivially_copyable_v<MeshRow> && sizeof(MeshRow) == 8);
static_assert(std::is_trivially_copyable_v<MaterialRow> && sizeof(MaterialRow) == 8);
static_assert(std::is_trivially_copyable_v<LightRow> && sizeof(LightRow) == 8);
static_assert(std::is_trivially_copyable_v<CameraRow> && sizeof(CameraRow) == 8);
static_assert(std::is_trivially_copyable_v<DecalRow> && sizeof(DecalRow) == 8);
// (The inline-payload rows are intentionally NOT trivially copyable —
// their payloads own strings. The triviality contract is the
// indirection rows', which the pak writes as literal row arrays.)

// All authored association tables. One vector per kind; rows are
// unordered (node order is a COOK-time property, see the header).
// More kinds arrive with their migration epochs (giVolume / cubemap /
// env / envVolume / export rows at Epoch 5, rigid bodies + colliders
// at Epoch 10) — each as its own explicit member + row type.
struct SceneTables {
	std::vector<MeshRow> meshes;
	std::vector<MaterialRow> materials;
	std::vector<LightRow> lights;
	std::vector<CameraRow> cameras;
	std::vector<DecalRow> decals;
	std::vector<GiVolumeRow> giVolumes;
	std::vector<CubemapRow> cubemaps;
	std::vector<ExportRow> exports;
	std::vector<EnvRow> envs;
	std::vector<EnvVolumeRow> envVolumes;
	// Rigid bodies + colliders arrive at Epoch 10.
};

// Derived per-node index over SceneTables — resized to the node count,
// one entry per table kind (InvalidRow = the node has no row of that
// kind). NEVER serialized, never a source of truth: RebuildNodeRows
// reconstructs it from the tables alone. `body` is reserved for
// Epoch 10's RigidBodyRow (present now so the struct's shape is
// stable across the migration).
struct NodeRows {
	RowId mesh = InvalidRow;
	RowId material = InvalidRow;
	RowId light = InvalidRow;
	RowId camera = InvalidRow;
	RowId decal = InvalidRow;
	RowId giVolume = InvalidRow;
	RowId cubemap = InvalidRow;
	RowId exportSettings = InvalidRow;
	RowId env = InvalidRow;
	RowId envVolume = InvalidRow;
	RowId body = InvalidRow;
};

// Dense per-node IDENTITY metadata (Epoch 5: replaced nameForNode +
// uuidForNode). Names are near-universal and uuids are node identity,
// not associations — a dense array indexed by node (like the flag
// tables), NOT an optional table. `name` indexes Scene::nodeNames
// (UINT32_MAX = unnamed); `uuid` is the 32-hex tour UUID ("" = none).
// NOT trivially copyable (uuid owns a string); the cooked META chunk
// (Epoch 7) serializes it field-wise.
struct NodeMetadata {
	uint32_t name = 0xFFFFFFFFu;
	std::string uuid;
};

struct Scene;

// ---- handwritten maintenance (no FindRow<T> — one explicit set per
// kind; the duplication is the design, see the header) ----

// Full rebuild of scene.rowsForNode from scene.tables (also resizes it
// to the current node count). Call after direct table manipulation;
// the Set/Remove mutators below keep the index correct incrementally.
void RebuildNodeRows(Scene& scene);

// Row access. Null when the node has no row of that kind (or the node
// is out of range).
MeshRow* FindMesh(Scene& scene, NodeId node);
const MeshRow* FindMesh(const Scene& scene, NodeId node);
MaterialRow* FindMaterial(Scene& scene, NodeId node);
const MaterialRow* FindMaterial(const Scene& scene, NodeId node);
LightRow* FindLight(Scene& scene, NodeId node);
const LightRow* FindLight(const Scene& scene, NodeId node);
CameraRow* FindCamera(Scene& scene, NodeId node);
const CameraRow* FindCamera(const Scene& scene, NodeId node);
DecalRow* FindDecal(Scene& scene, NodeId node);
const DecalRow* FindDecal(const Scene& scene, NodeId node);
GiVolumeRow* FindGiVolume(Scene& scene, NodeId node);
const GiVolumeRow* FindGiVolume(const Scene& scene, NodeId node);
CubemapRow* FindCubemap(Scene& scene, NodeId node);
const CubemapRow* FindCubemap(const Scene& scene, NodeId node);
ExportRow* FindExport(Scene& scene, NodeId node);
const ExportRow* FindExport(const Scene& scene, NodeId node);
EnvRow* FindEnv(Scene& scene, NodeId node);
const EnvRow* FindEnv(const Scene& scene, NodeId node);
EnvVolumeRow* FindEnvVolume(Scene& scene, NodeId node);
const EnvVolumeRow* FindEnvVolume(const Scene& scene, NodeId node);

// Association mutators. At most one row per node per kind: Set REPLACES
// the existing row (duplicate rejection with replacement semantics).
// Returns false when the node index is invalid (>= node count); a Set
// on a valid node always succeeds.
bool SetMesh(Scene& scene, NodeId node, uint32_t mesh);
bool SetMaterial(Scene& scene, NodeId node, uint32_t material);
bool SetLight(Scene& scene, NodeId node, uint32_t light);
bool SetCamera(Scene& scene, NodeId node, uint32_t camera);
bool SetDecal(Scene& scene, NodeId node, uint32_t decal);
bool SetGiVolume(Scene& scene, NodeId node, const SceneGiVolume& volume);
bool SetCubemap(Scene& scene, NodeId node, const SceneCubemap& cubemap);
bool SetExport(Scene& scene, NodeId node, const SceneExportSettings& settings);
bool SetEnv(Scene& scene, NodeId node, const SceneEnv& environment);
bool SetEnvVolume(Scene& scene, NodeId node, const EnvVolume& volume);

// Removes the node's row of that kind (swap-and-pop + lookup repair).
// Returns whether a row existed.
bool RemoveMesh(Scene& scene, NodeId node);
bool RemoveMaterial(Scene& scene, NodeId node);
bool RemoveLight(Scene& scene, NodeId node);
bool RemoveCamera(Scene& scene, NodeId node);
bool RemoveDecal(Scene& scene, NodeId node);
bool RemoveGiVolume(Scene& scene, NodeId node);
bool RemoveCubemap(Scene& scene, NodeId node);
bool RemoveExport(Scene& scene, NodeId node);
bool RemoveEnv(Scene& scene, NodeId node);
bool RemoveEnvVolume(Scene& scene, NodeId node);

// ---- dense node metadata (name/uuid; see NodeMetadata) ----
// Grows scene.metadata to the node count on demand (addNode does not
// touch it — same lazy contract as rowsForNode).
void EnsureNodeMetadata(Scene& scene);
// Name access (index into Scene::nodeNames; UINT32_MAX = unnamed).
uint32_t GetNodeNameIndex(const Scene& scene, NodeId node);
void SetNodeNameIndex(Scene& scene, NodeId node, uint32_t nameIndex);
// Uuid access ("" = none).
const std::string& GetNodeUuid(const Scene& scene, NodeId node);
void SetNodeUuidRaw(Scene& scene, NodeId node, const std::string& uuid);

// Drops EVERY row the node holds (all kinds at once — the node-
// deletion hook the editor/tour paths adopt in Epoch 5). Returns
// whether anything was removed.
bool RemoveNodeRows(Scene& scene, NodeId node);
