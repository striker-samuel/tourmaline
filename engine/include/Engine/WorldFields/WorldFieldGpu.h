// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// World-field GPU table — the C++ half of a C++/HLSL layout contract.
//
// The mirror is engine/shaders/world_field.hlsli (WorldPageGpu /
// WorldLevelGpu / WorldFieldGpu + the WORLD_FIELD_* constants). Both
// sides are pinned by static_asserts on the C++ side and offset comments
// on the HLSL side — the same discipline as FrameUniforms / SgProbeGpu.
// The table travels in ONE storage buffer (BDA published through
// FrameUniforms::worldFields); every offset below is measured from that
// buffer's BASE:
//
//   +0                 WorldFieldTableHeader (16 B)
//   +16                WorldFieldGpu fields[fieldCount]  (fixed 320 B stride)
//   +header.pagesOffset WorldPageGpu pages[]             (flat pool, 16 B each)
//
// pages[] is addressed per field per LOD through
// WorldLevelGpu::pageTableOffset (a byte offset from the table base).
// The whole table is rebuilt + re-uploaded by WorldFieldSystem whenever
// residency or a clipmap window changes (a few KB — trivial), so the BDA
// is stable for the system's lifetime and no per-flight snapshots are
// needed: publication ordering is structural (see WorldField.h's
// publication contract).
//
// PACKING RULES (both sides): every struct is a sequence of 16-byte
// chunks (float4/uint4/int4 or scalar quads); no member crosses a
// 16-byte boundary. The HLSL mirror compiles under -fvk-use-dx-layout,
// so scalars align to 4 and 16-byte vectors to 16 — the C++ mirrors
// use the same explicit interleaves the other GPU-mirrored structs use
// (see FrameUniforms' aoPad note for why).

#include <cstdint>

#include <glm/glm.hpp>

#include <Engine/smvk.h>

namespace engine::worldfields {

// ---------------------------------------------------------------------------
// Constants shared with world_field.hlsli (WORLD_FIELD_* there). The HLSL
// side defines the same values as preprocessor macros with a MUST-match
// comment; editing either side recompiles every shader that includes
// world_field.hlsli via the SHADER_HEADERS glob dependency.
// ---------------------------------------------------------------------------

// Registered-field capacity of the GPU table (fields[] is sized by this
// on the HLSL side through the stride arithmetic; the C++ side keeps the
// live count in the header).
inline constexpr uint32_t kMaxFields = 8;

// Per-field LOD capacity (levels[] inline array bound in WorldFieldGpu).
inline constexpr uint32_t kMaxFieldLods = 4;

// Field ids are the compact indices into fields[] — see WorldField.h
// (registration order is the stable field id, kWorldFieldFog first).

// ---------------------------------------------------------------------------
// One page-table entry. atlasSlot semantics:
//   >= 0 : the brick occupies that physical slot of the field's atlas
//          pool — sample it (brickXYZ is the ABSOLUTE brick coordinate,
//          rechecked below as the stale-toroidal-entry guard)
//   <  0 : not resident this frame — walk coarser, then the field's
//          decoded fallbackValue. (A dedicated "constant zero" page
//          state is reserved for the BC4 fog milestone: brickless empty
//          cells that never allocate physical slots.)
// ---------------------------------------------------------------------------
struct alignas(16) WorldPageGpu
{
	int32_t brickX = 0; // 0  absolute brick coord (signed world lattice)
	int32_t brickY = 0; // 4
	int32_t brickZ = 0; // 8
	int32_t atlasSlot = -1; // 12  physical pool slot; -1 = not resident
};
static_assert(sizeof(WorldPageGpu) == 16, "WorldPageGpu size: HLSL mirror (world_field.hlsli)");

// ---------------------------------------------------------------------------
// One clipmap LOD of a field: a power-of-two toroidal table of
// windowDimXYZ entries centered on the interest point. windowMinBrick is
// the ABSOLUTE brick coordinate stored at table cell (0,0,0); cell (c)
// holds brick windowMin + c, and its toroidal slot is brick & (dim-1) —
// the absolute-coord tag in every page is what makes a wrapped-around
// stale entry detectable.
// ---------------------------------------------------------------------------
struct alignas(16) WorldLevelGpu
{
	glm::ivec4 windowMinBrick{ 0 }; // 0   xyz = min brick coord of the window; w unused
	glm::uvec4 windowDim{ 0 }; // 16  xyz = per-axis entry counts (power of two); w unused
	uint32_t pageTableOffset = 0; // 32  byte offset of this level's pages from the table base
	float voxelSizeMeters = 1.0f; // 36  voxel edge at this LOD (base * 2^lod)
	uint32_t reserved[2] = { 0, 0 }; // 40  (16-align tail; struct ends 48)
};
static_assert(sizeof(WorldLevelGpu) == 48, "WorldLevelGpu size: HLSL mirror (world_field.hlsli)");

// ---------------------------------------------------------------------------
// One logical field as the GPU sees it: addressing (origin), decode
// (encoded texel -> world attribute), fallback, and the physical atlas
// binding. The struct is a fixed 320-byte stride so the shader can index
// fields[] without a second indirection.
// ---------------------------------------------------------------------------
struct alignas(16) WorldFieldGpu
{
	glm::vec4 originMeters{ 0.0f }; // 0   xyz = world min corner of brick (0,0,0); w unused
	glm::vec4 decodeScale{ 1.0f }; // 16  attribute = decodeScale * decodeFn(encoded) + decodeBias
	glm::vec4 decodeBias{ 0.0f }; // 32
	glm::vec4 fallbackValue{ 0.0f }; // 48  DECODED value when no level resolves (m/s wind, m^-1 fog...)
	glm::uvec4 atlasSlots{ 0 }; // 64  xyz = physical slot grid (atlasDims = slots * storedBrick); w unused
	glm::uvec4 meta{ 0 }; // 80  x = atlas bindless index (UINT32_MAX = no atlas yet),
	//     y = encoding (WorldFieldEncoding), z = levelCount, w = flags
	WorldLevelGpu levels[kMaxFieldLods]; // 96  4 * 48 = 192 -> 288
	uint32_t reserved[8] = {}; // 288 (16-align tail; struct ends 320)
};
static_assert(sizeof(WorldFieldGpu) == 320, "WorldFieldGpu size: HLSL mirror (world_field.hlsli)");
static_assert(offsetof(WorldFieldGpu, levels) == 96, "WorldFieldGpu levels offset: HLSL mirror");

// The 16-byte table header. pagesOffset = 16 + fieldCount * 320 (always
// 16-aligned; the C++ side asserts the arithmetic at publish time).
struct alignas(16) WorldFieldTableHeader
{
	uint32_t fieldCount = 0; // 0  live fields this frame
	uint32_t fieldStride = 320; // 4  bytes (sizeof(WorldFieldGpu), pinned)
	uint32_t pagesOffset = 0; // 8  byte offset of the page pool from the table base
	uint32_t reserved = 0; // 12
};
static_assert(sizeof(WorldFieldTableHeader) == 16, "WorldFieldTableHeader size: HLSL mirror");

// What WorldFieldSystem publishes into FrameUniforms (the app-side
// mirror is two plain members — a BDA + a count).
struct WorldFieldGpuBinding
{
	uint64_t tableBda = 0; // 0 = subsystem off / not yet published
	uint32_t fieldCount = 0;
	bool Valid() const { return tableBda != 0; }
};

} // namespace engine::worldfields
