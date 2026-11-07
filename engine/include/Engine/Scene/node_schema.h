// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// PROJECT-DEFINED NODE SCHEMAS (Phase 3 of the consolidated content
// architecture — docs/content_as_code.md): the EDITOR defines DATA
// (typed attribute sets attached to regular scene transform nodes),
// the ENGINE stores COOKED BYTES, and the GENERATED project::types
// PODs own the type-safe decode. "Compile types; cook values":
// changing a value is a cooked-data change; adding a field changes
// the generated type + recooks the data.
//
// There is deliberately NO general-purpose runtime reflection: the
// engine-side surface is the schema LAYOUT (the cooked value block's
// field offsets — enough to validate + author), the per-pak SCHEMA
// REGISTRY blob, and the scene's schema-association rows (node +
// schemaId + the cooked value bytes). Everything typed lives in the
// generated header, whose emitter guarantees the C++ POD layout
// equals the cooked layout (static_asserts on the offsets).
//
// The authored format (a .nodeschema text file — the project owns
// them under content/, the recipe's include globs claim them):
//
//   # stable numeric id; the NAME may change, the id may not
//   schema ZombieSpawner 3001
//     # field: <name> <type> [count] [= default literals]
//     attr respawnTime f32 = 12.0
//     attr maxAlive     u32 = 8
//     attr spawnOffset  vec3 = 0 0 0
//     attr guardMesh    ref = 0x3000     # a typed asset reference
//     attr patrolPoints vec3 4           # a fixed array of 4 vec3
//     attr label        str = "spawner"
//
// Types: f32 i32 u32 bool vec2 vec3 vec4 str ref. `ref` is a stable
// content id (the typed asset-reference lane — cook-time dependency
// edges). Field identity is the DECLARATION ORDINAL (the stable field
// id); renames are free, reordering is a data change (recook).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace smvk {

enum class SchemaFieldType : uint32_t {
    F32 = 1,
    I32 = 2,
    U32 = 3,
    Bool = 4,
    Vec2 = 5, // two f32
    Vec3 = 6, // three f32
    Vec4 = 7, // four f32
    Str = 8,  // u32 byte length + bytes (cooked inline)
    Ref = 9,  // u64 stable content id (the typed asset reference)
};

// One field's cooked shape. `offset` is the field's byte offset in
// the schema's cooked value block (computed by FinalizeSchema; the
// layout is fields-in-declaration-order, each aligned to its natural
// size — str pads to 4, ref aligns 8).
struct SchemaField {
    uint32_t id = 0; // the declaration ordinal (stable identity)
    std::string name;
    SchemaFieldType type = SchemaFieldType::F32;
    uint32_t count = 1;                // fixed array element count (1 = scalar)
    std::vector<uint8_t> defaultValue; // cooked-form default
    // Computed by FinalizeSchema:
    uint32_t offset = 0; // byte offset in the value block
    uint32_t stride = 0; // per-element cooked stride
};

struct NodeSchema {
    uint32_t id = 0;  // the STABLE schema id (project-authored)
    std::string name; // display name (free to change)
    std::vector<SchemaField> fields;
    // Computed by FinalizeSchema:
    uint32_t valueBytes = 0; // the full cooked value-block size

    [[nodiscard]] const SchemaField* Field(uint32_t fieldId) const;
};

// Computes the cooked layout (offsets/strides/valueBytes) and returns
// false + err on a malformed schema (empty name/id, duplicate field
// names, count 0, a default that does not encode).
bool FinalizeSchema(NodeSchema& schema, std::string& err);

// The per-element cooked stride of a type (str: the 4-byte length
// prefix — VARIABLE stride, the only non-fixed type; the layout
// treats a str ARRAY as unsupported: count must be 1).
uint32_t SchemaTypeStride(SchemaFieldType type);

// ---- the authored text format -------------------------------------------
// Parses ONE schema definition (the `schema <name> <id>` block; the
// file may carry several — parse line-wise or split on `schema `).
// False + err with the offending line number on any malformation.
bool ParseNodeSchemaText(const std::string& text, NodeSchema& out, std::string& err);
// Parses EVERY `schema` block in a .nodeschema file (order preserved).
bool ParseNodeSchemaFile(const std::string& text, std::vector<NodeSchema>& out, std::string& err);

// ---- the per-pak registry blob -------------------------------------------
// One PakAssetKind::Schema blob per pak at the reserved path
// "#schemas": the cooked schema layouts the runtime (and the
// generated types' consumers) validate against.
//   u32 magic 'SCHR' | u32 version(=1) | u32 schemaCount
//   per schema: u32 id | u32 nameLen | name | u32 valueBytes |
//               u32 fieldCount
//     per field: u32 id | u32 nameLen | name | u32 type |
//                u32 count | u32 offset | u32 stride |
//                u32 defaultLen | default bytes
std::vector<uint8_t> WriteSchemaRegistryBlob(const std::vector<NodeSchema>& schemas);
bool ParseSchemaRegistryBlob(std::span<const uint8_t> blob,
                             std::vector<NodeSchema>& out,
                             std::string& err);

inline constexpr const char* kPakSchemaRegistryPath = "#schemas";

} // namespace smvk
