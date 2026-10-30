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
#include <type_traits>
#include <vector>

#include <Engine/Scene/scene_node_id.h>

// Row index into a SceneTables member vector. InvalidRow = no row.
using RowId = uint32_t;
constexpr RowId InvalidRow = 0xFFFFFFFFu;

// Node -> mesh association. `mesh` is the owning Cluster's
// ClusterStaticMeshHandle index (cluster scene) or the world
// meshLocations index (merged world scene) — exactly what
// meshForNode stores today; the ownership model is unchanged.
// `material` preserves the materialForNode override semantics
// (UINT32_MAX = no override; the import path always pairs them).
// Whether a material association can exist WITHOUT a mesh is an
// Epoch 4 question — answered against the import/tour code before
// the maps die; until then MeshRow.material is authoritative ONLY
// alongside a mesh row (the maps remain authoritative this epoch).
struct MeshRow {
	NodeId node = 0;
	uint32_t mesh = 0xFFFFFFFFu;
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

// The row contract for the future pak chunks (Epoch 7): every row is
// trivially copyable with a static layout — the writer/reader pair
// pins field order + widths explicitly, and these asserts prove the
// compiler layout stays the plain data the disk format assumes.
static_assert(std::is_trivially_copyable_v<MeshRow> && sizeof(MeshRow) == 12);
static_assert(std::is_trivially_copyable_v<LightRow> && sizeof(LightRow) == 8);
static_assert(std::is_trivially_copyable_v<CameraRow> && sizeof(CameraRow) == 8);
static_assert(std::is_trivially_copyable_v<DecalRow> && sizeof(DecalRow) == 8);

// All authored association tables. One vector per kind; rows are
// unordered (node order is a COOK-time property, see the header).
// More kinds arrive with their migration epochs (giVolume / cubemap /
// env / envVolume / export rows at Epoch 5, rigid bodies + colliders
// at Epoch 10) — each as its own explicit member + row type.
struct SceneTables {
	std::vector<MeshRow> meshes;
	std::vector<LightRow> lights;
	std::vector<CameraRow> cameras;
	std::vector<DecalRow> decals;
};

// Derived per-node index over SceneTables — resized to the node count,
// one entry per table kind (InvalidRow = the node has no row of that
// kind). NEVER serialized, never a source of truth: RebuildNodeRows
// reconstructs it from the tables alone. `body` is reserved for
// Epoch 10's RigidBodyRow (present now so the struct's shape is
// stable across the migration).
struct NodeRows {
	RowId mesh = InvalidRow;
	RowId light = InvalidRow;
	RowId camera = InvalidRow;
	RowId decal = InvalidRow;
	RowId body = InvalidRow;
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
LightRow* FindLight(Scene& scene, NodeId node);
const LightRow* FindLight(const Scene& scene, NodeId node);
CameraRow* FindCamera(Scene& scene, NodeId node);
const CameraRow* FindCamera(const Scene& scene, NodeId node);
DecalRow* FindDecal(Scene& scene, NodeId node);
const DecalRow* FindDecal(const Scene& scene, NodeId node);

// Association mutators. At most one row per node per kind: Set REPLACES
// the existing row (duplicate rejection with replacement semantics).
// Returns false when the node index is invalid (>= node count); a Set
// on a valid node always succeeds.
bool SetMesh(Scene& scene, NodeId node, uint32_t mesh, uint32_t material);
bool SetLight(Scene& scene, NodeId node, uint32_t light);
bool SetCamera(Scene& scene, NodeId node, uint32_t camera);
bool SetDecal(Scene& scene, NodeId node, uint32_t decal);

// Removes the node's row of that kind (swap-and-pop + lookup repair).
// Returns whether a row existed.
bool RemoveMesh(Scene& scene, NodeId node);
bool RemoveLight(Scene& scene, NodeId node);
bool RemoveCamera(Scene& scene, NodeId node);
bool RemoveDecal(Scene& scene, NodeId node);

// Drops EVERY row the node holds (all kinds at once — the node-
// deletion hook the editor/tour paths adopt in Epoch 5). Returns
// whether anything was removed.
bool RemoveNodeRows(Scene& scene, NodeId node);
