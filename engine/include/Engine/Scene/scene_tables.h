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
//   * `TransformHierarchy::rowsForNode` is a DERIVED per-node index over the tables
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

namespace tour::transformhierarchy {
struct TransformHierarchy;
} // namespace tour::transformhierarchy
namespace tour::transformhierarchy {

// Row index into a SceneTables member vector. InvalidRow = no row.
using RowId = uint32_t;
constexpr RowId InvalidRow = 0xFFFFFFFFu;

// Node -> mesh association. `mesh` is the owning WorldPage's
// WorldPageStaticMeshHandle index (cluster scene) or the world
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

// Node -> light association (the owning WorldPage's lights pool index —
// what lightForNode stores today).
struct LightRow {
    NodeId node = 0;
    uint32_t light = 0xFFFFFFFFu;
};

// Node -> camera association (the owning WorldPage's cameras pool index).
struct CameraRow {
    NodeId node = 0;
    uint32_t camera = 0xFFFFFFFFu;
};

// Node -> decal association (the owning WorldPage's decals pool index).
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

// Node -> authored audio source (the emitter association; see
// SceneAudioSource). Inline payload like giVolume/envVolume: an
// emitter is a node-local definition — no pool, the sounds list is
// asset REFERENCES (the clips themselves are shared pak assets).
struct AudioSourceRow {
    NodeId node = 0;
    SceneAudioSource source{};
};

// Node -> authored AUDIO ROOM (the environmental-reverb volume; see
// SceneAudioRoom). Inline payload like audioSource/giVolume: the room
// is a node-local definition — at most ONE per node (Set replaces),
// independent of any GiVolumeRow on the same node.
struct AudioRoomRow {
    NodeId node = 0;
    SceneAudioRoom room{};
};

// Node -> authored rigid-body and collider settings.
struct PhysicsBodyRow {
    NodeId node = 0;
    ScenePhysicsBody body{};
};

// Node -> optional spatial trigger component.
struct TriggerVolumeRow {
    NodeId node = 0;
    SceneTriggerVolume trigger{};
};

// Node -> PROJECT-DEFINED SCHEMA ASSOCIATION (the custom-node-schema
// lane, Engine/Scene/node_schema.h + docs/content_as_code.md): a
// typed data block attached to a regular scene transform node. The
// engine stores COOKED BYTES (the schema's value-block layout — no
// reflection by design); the schema LAYOUT rides the per-pak schema
// registry blob and the GENERATED project::types PODs own the
// type-safe decode ("compile types; cook values"). Inline payload:
// the value block is a node-local definition, exactly like the audio
// source. At most ONE schema association per node (Set replaces).
struct SchemaAssocRow {
    NodeId node = 0;
    uint32_t schemaId = 0;       // the STABLE schema id (the pak's registry)
    std::vector<uint8_t> values; // the cooked value block
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
// Each kind is its own explicit member + row type (the xForNode
// migration landed kind-by-kind across the epochs; every kind is
// present now).
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
    std::vector<AudioSourceRow> audioSources;
    std::vector<AudioRoomRow> audioRooms;
    std::vector<TriggerVolumeRow> triggerVolumes;
    std::vector<PhysicsBodyRow> physicsBodies;
    std::vector<SchemaAssocRow> schemaAssocs;
};

// Derived per-node index over SceneTables — resized to the node count,
// one entry per table kind (InvalidRow = the node has no row of that
// kind). NEVER serialized, never a source of truth: RebuildNodeRows
// reconstructs it from the tables alone. `body` is the physics-body
// association (PhysicsBodyRow).
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
    RowId audioSource = InvalidRow;
    RowId audioRoom = InvalidRow;
    RowId triggerVolume = InvalidRow;
    RowId schemaAssoc = InvalidRow;
    RowId body = InvalidRow; // physics body association
};

// Dense per-node IDENTITY metadata (Epoch 5: replaced nameForNode +
// uuidForNode). Names are near-universal and uuids are node identity,
// not associations — a dense array indexed by node (like the flag
// tables), NOT an optional table. `name` indexes TransformHierarchy::nodeNames
// (UINT32_MAX = unnamed); `uuid` is the 32-hex tour UUID ("" = none).
// NOT trivially copyable (uuid owns a string); the cooked META chunk
// (Epoch 7) serializes it field-wise.
struct NodeMetadata {
    uint32_t name = 0xFFFFFFFFu;
    std::string uuid;
    // Per-instance direct-light receiving channels (not a mesh/material property).
    uint32_t lightLayerMask = 0xffffffffu;
};

// ---- handwritten maintenance (no FindRow<T> — one explicit set per
// kind; the duplication is the design, see the header) ----

// Full rebuild of scene.rowsForNode from scene.tables (also resizes it
// to the current node count). Call after direct table manipulation;
// the Set/Remove mutators below keep the index correct incrementally.
void RebuildNodeRows(TransformHierarchy& scene);

// Row access. Null when the node has no row of that kind (or the node
// is out of range).
MeshRow* FindMesh(TransformHierarchy& scene, NodeId node);
const MeshRow* FindMesh(const TransformHierarchy& scene, NodeId node);
MaterialRow* FindMaterial(TransformHierarchy& scene, NodeId node);
const MaterialRow* FindMaterial(const TransformHierarchy& scene, NodeId node);
LightRow* FindLight(TransformHierarchy& scene, NodeId node);
const LightRow* FindLight(const TransformHierarchy& scene, NodeId node);
CameraRow* FindCamera(TransformHierarchy& scene, NodeId node);
const CameraRow* FindCamera(const TransformHierarchy& scene, NodeId node);
DecalRow* FindDecal(TransformHierarchy& scene, NodeId node);
const DecalRow* FindDecal(const TransformHierarchy& scene, NodeId node);
GiVolumeRow* FindGiVolume(TransformHierarchy& scene, NodeId node);
const GiVolumeRow* FindGiVolume(const TransformHierarchy& scene, NodeId node);
CubemapRow* FindCubemap(TransformHierarchy& scene, NodeId node);
const CubemapRow* FindCubemap(const TransformHierarchy& scene, NodeId node);
ExportRow* FindExport(TransformHierarchy& scene, NodeId node);
const ExportRow* FindExport(const TransformHierarchy& scene, NodeId node);
EnvRow* FindEnv(TransformHierarchy& scene, NodeId node);
const EnvRow* FindEnv(const TransformHierarchy& scene, NodeId node);
EnvVolumeRow* FindEnvVolume(TransformHierarchy& scene, NodeId node);
const EnvVolumeRow* FindEnvVolume(const TransformHierarchy& scene, NodeId node);
AudioSourceRow* FindAudioSource(TransformHierarchy& scene, NodeId node);
const AudioSourceRow* FindAudioSource(const TransformHierarchy& scene, NodeId node);
AudioRoomRow* FindAudioRoom(TransformHierarchy& scene, NodeId node);
PhysicsBodyRow* FindPhysicsBody(TransformHierarchy& scene, NodeId node);
const PhysicsBodyRow* FindPhysicsBody(const TransformHierarchy& scene, NodeId node);
TriggerVolumeRow* FindTriggerVolume(TransformHierarchy& scene, NodeId node);
const TriggerVolumeRow* FindTriggerVolume(const TransformHierarchy& scene, NodeId node);
const AudioRoomRow* FindAudioRoom(const TransformHierarchy& scene, NodeId node);
SchemaAssocRow* FindSchemaAssoc(TransformHierarchy& scene, NodeId node);
const SchemaAssocRow* FindSchemaAssoc(const TransformHierarchy& scene, NodeId node);

// Association mutators. At most one row per node per kind: Set REPLACES
// the existing row (duplicate rejection with replacement semantics).
// Returns false when the node index is invalid (>= node count); a Set
// on a valid node always succeeds.
bool SetMesh(TransformHierarchy& scene, NodeId node, uint32_t mesh);
bool SetMaterial(TransformHierarchy& scene, NodeId node, uint32_t material);
bool SetLight(TransformHierarchy& scene, NodeId node, uint32_t light);
bool SetCamera(TransformHierarchy& scene, NodeId node, uint32_t camera);
bool SetDecal(TransformHierarchy& scene, NodeId node, uint32_t decal);
bool SetGiVolume(TransformHierarchy& scene, NodeId node, const SceneGiVolume& volume);
bool SetCubemap(TransformHierarchy& scene, NodeId node, const SceneCubemap& cubemap);
bool SetExport(TransformHierarchy& scene, NodeId node, const SceneExportSettings& settings);
bool SetEnv(TransformHierarchy& scene, NodeId node, const SceneEnv& environment);
bool SetEnvVolume(TransformHierarchy& scene, NodeId node, const EnvVolume& volume);
bool SetAudioSource(TransformHierarchy& scene, NodeId node, const SceneAudioSource& source);
bool SetAudioRoom(TransformHierarchy& scene, NodeId node, const SceneAudioRoom& room);
bool SetPhysicsBody(TransformHierarchy& scene, NodeId node, const ScenePhysicsBody& body);
bool SetTriggerVolume(TransformHierarchy& scene, NodeId node, const SceneTriggerVolume& trigger);
// The schema association (node_schema.h): the cooked value block is
// stored verbatim — the schema id names the layout (the pak's
// registry); `values` may be empty (defaults decode at read time).
bool SetSchemaAssoc(TransformHierarchy& scene,
                    NodeId node,
                    uint32_t schemaId,
                    std::vector<uint8_t> values);

// Removes the node's row of that kind (swap-and-pop + lookup repair).
// Returns whether a row existed.
bool RemoveMesh(TransformHierarchy& scene, NodeId node);
bool RemoveMaterial(TransformHierarchy& scene, NodeId node);
bool RemoveLight(TransformHierarchy& scene, NodeId node);
bool RemoveCamera(TransformHierarchy& scene, NodeId node);
bool RemoveDecal(TransformHierarchy& scene, NodeId node);
bool RemoveGiVolume(TransformHierarchy& scene, NodeId node);
bool RemoveCubemap(TransformHierarchy& scene, NodeId node);
bool RemoveExport(TransformHierarchy& scene, NodeId node);
bool RemoveEnv(TransformHierarchy& scene, NodeId node);
bool RemoveEnvVolume(TransformHierarchy& scene, NodeId node);
bool RemoveAudioSource(TransformHierarchy& scene, NodeId node);
bool RemoveAudioRoom(TransformHierarchy& scene, NodeId node);
bool RemovePhysicsBody(TransformHierarchy& scene, NodeId node);
bool RemoveTriggerVolume(TransformHierarchy& scene, NodeId node);
bool RemoveSchemaAssoc(TransformHierarchy& scene, NodeId node);

// ---- dense node metadata (name/uuid; see NodeMetadata) ----
// Grows scene.metadata to the node count on demand (addNode does not
// touch it — same lazy contract as rowsForNode).
void EnsureNodeMetadata(TransformHierarchy& scene);
// Name access (index into TransformHierarchy::nodeNames; UINT32_MAX = unnamed).
uint32_t GetNodeNameIndex(const TransformHierarchy& scene, NodeId node);
void SetNodeNameIndex(TransformHierarchy& scene, NodeId node, uint32_t nameIndex);
// Uuid access ("" = none).
const std::string& GetNodeUuid(const TransformHierarchy& scene, NodeId node);
void SetNodeUuidRaw(TransformHierarchy& scene, NodeId node, const std::string& uuid);

// Drops EVERY row the node holds (all kinds at once — the node-
// deletion hook the editor/tour paths adopt). Returns
// whether anything was removed.
bool RemoveNodeRows(TransformHierarchy& scene, NodeId node);

} // namespace tour::transformhierarchy

// GLOBAL compat aliases (the migration bridge - retire with the call
// sites; the boundary scanner guards against NEW globals, these are
// the tracked pre-existing set).
using RowId = tour::transformhierarchy::RowId;
using tour::transformhierarchy::AudioSourceRow;
using tour::transformhierarchy::CameraRow;
using tour::transformhierarchy::CubemapRow;
using tour::transformhierarchy::DecalRow;
using tour::transformhierarchy::EnsureNodeMetadata;
using tour::transformhierarchy::EnvRow;
using tour::transformhierarchy::EnvVolumeRow;
using tour::transformhierarchy::ExportRow;
using tour::transformhierarchy::FindAudioSource;
using tour::transformhierarchy::FindCamera;
using tour::transformhierarchy::FindCubemap;
using tour::transformhierarchy::FindDecal;
using tour::transformhierarchy::FindEnv;
using tour::transformhierarchy::FindEnvVolume;
using tour::transformhierarchy::FindExport;
using tour::transformhierarchy::FindGiVolume;
using tour::transformhierarchy::FindLight;
using tour::transformhierarchy::FindMaterial;
using tour::transformhierarchy::FindMesh;
using tour::transformhierarchy::FindSchemaAssoc;
using tour::transformhierarchy::GetNodeNameIndex;
using tour::transformhierarchy::GetNodeUuid;
using tour::transformhierarchy::GiVolumeRow;
using tour::transformhierarchy::InvalidRow;
using tour::transformhierarchy::LightRow;
using tour::transformhierarchy::MaterialRow;
using tour::transformhierarchy::MeshRow;
using tour::transformhierarchy::NodeMetadata;
using tour::transformhierarchy::NodeRows;
using tour::transformhierarchy::RebuildNodeRows;
using tour::transformhierarchy::RemoveAudioSource;
using tour::transformhierarchy::RemoveCamera;
using tour::transformhierarchy::RemoveCubemap;
using tour::transformhierarchy::RemoveDecal;
using tour::transformhierarchy::RemoveEnv;
using tour::transformhierarchy::RemoveEnvVolume;
using tour::transformhierarchy::RemoveExport;
using tour::transformhierarchy::RemoveGiVolume;
using tour::transformhierarchy::RemoveLight;
using tour::transformhierarchy::RemoveMaterial;
using tour::transformhierarchy::RemoveMesh;
using tour::transformhierarchy::RemoveNodeRows;
using tour::transformhierarchy::RemoveSchemaAssoc;
using tour::transformhierarchy::SceneTables;
using tour::transformhierarchy::SchemaAssocRow;
using tour::transformhierarchy::SetAudioSource;
using tour::transformhierarchy::SetCamera;
using tour::transformhierarchy::SetCubemap;
using tour::transformhierarchy::SetDecal;
using tour::transformhierarchy::SetEnv;
using tour::transformhierarchy::SetEnvVolume;
using tour::transformhierarchy::SetExport;
using tour::transformhierarchy::SetGiVolume;
using tour::transformhierarchy::SetLight;
using tour::transformhierarchy::SetMaterial;
using tour::transformhierarchy::SetMesh;
using tour::transformhierarchy::SetNodeNameIndex;
using tour::transformhierarchy::SetNodeUuidRaw;
using tour::transformhierarchy::SetSchemaAssoc;
