// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldFields shared value types — the dependency root of the module
// (WorldField.h and WorldFieldAtlas.h both include this; it includes
// nothing of the module). Keeps the field descriptor/system header and
// the atlas pool header decoupled so the system can own pools by value.
//
// Brick geometry + the brick key are the pieces BOTH sides need; the
// encoding enum is mirrored by world_field.hlsli (WORLD_FIELD_ENC_*).

#include <cstdint>

#include <Engine/smvk.h>

namespace engine::worldfields {

// ---------------------------------------------------------------------------
// Brick geometry constants — MUST match WORLD_FIELD_* in
// engine/shaders/world_field.hlsli (the GPU sampler hardcodes them).
// ---------------------------------------------------------------------------
inline constexpr uint32_t kBrickInteriorRes = 64;
inline constexpr uint32_t kBrickBorder = 2; // halo per side (filter neighbors + BCn divisibility)
inline constexpr uint32_t kBrickStoredRes = kBrickInteriorRes + 2 * kBrickBorder; // 68
// Per-LOD page-table entry cap (tableDimX*Y*Z <= this; power-of-two axes).
inline constexpr uint32_t kMaxPageTableEntriesPerLevel = 2048;

// ---------------------------------------------------------------------------
// Encoding: how a texel's decoded attribute maps from the stored range.
//   Linear          : attribute = scale * enc     + bias
//   SqrtUNorm       : attribute = scale * enc*enc + bias (UNORM enc in
//                     [0,1] — fog extinction's sqrt-precision-at-low-
//                     density packing; interpolate ENCODED then decode)
//   SignedNormalized: attribute = scale * enc     + bias (SNORM hardware
//                     decode gives [-1,1]; wind velocity)
// The GPU decodes AFTER interpolation. Keep in sync with WORLD_FIELD_ENC_*
// in world_field.hlsli (the numeric values ride the GPU table's meta.y).
// ---------------------------------------------------------------------------
enum class WorldFieldEncoding : uint32_t
{
	Linear = 0,
	SqrtUNorm = 1,
	SignedNormalized = 2,
};

enum class WorldFieldUpdateMode : uint32_t
{
	StaticStreamed = 0, // cooked/procedural, paged in and never rewritten
	CpuUpdated = 1, // CPU rewrites bricks over time (reserved, v1 treats as static)
	GpuGenerated = 2, // compute writes atlas slots directly (reserved)
};

// ---------------------------------------------------------------------------
// Brick identity: field id (registration order — the GPU fields[] index)
// + LOD + SIGNED brick coords on the field's world lattice.
// ---------------------------------------------------------------------------
struct WorldBrickKey
{
	uint32_t fieldId = 0;
	uint32_t lod = 0;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;

	bool operator==(const WorldBrickKey& o) const
	{
		return fieldId == o.fieldId && lod == o.lod
			&& x == o.x && y == o.y && z == o.z;
	}
};

struct WorldBrickKeyHash
{
	size_t operator()(const WorldBrickKey& k) const noexcept
	{
		// FNV-1a over the 20 bytes — cheap, order-independent enough for a
		// few-thousand-entry residency map.
		uint32_t h = 2166136261u;
		const auto mix = [&h](uint32_t v) {
			h ^= v;
			h *= 16777619u;
		};
		mix(k.fieldId);
		mix(k.lod);
		mix(static_cast<uint32_t>(k.x));
		mix(static_cast<uint32_t>(k.y));
		mix(static_cast<uint32_t>(k.z));
		return static_cast<size_t>(h);
	}
};

// Sentinel marking a free pool slot (SlotOwner() when unbound).
inline constexpr uint32_t kFreeSlotFieldId = 0xFFFFFFFFu;
inline const WorldBrickKey kFreeSlotKey{ kFreeSlotFieldId, 0, 0, 0, 0 };

} // namespace engine::worldfields
