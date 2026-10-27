// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldFieldAsset — the cooked, indexed, per-brick streamable world-field
// payload (".wfb"): the milestone-4 asset form.
//
// The plan's contract: NO one enormous world-sized 3D texture — a
// versioned INDEX of independently addressable bricks. Each index row
// carries (LOD, signed XYZ) -> payload offset/size + occupancy; payloads
// are per-brick (BC4 blocks for static density), so the pager streams
// single bricks from it and the file never needs wholesale decompression
// to serve one.
//
// FILE LAYOUT (little-endian, packed, static_assert-pinned):
//   Header (96 B, magic "TWFB")   — the magic IS the version (the repo's
//     no-format-versions doctrine: a breaking change bumps the magic,
//     old files fail closed; never add converters).
//   Index  (brickCount x 32 B, sorted by (lod, x, y, z))
//     u32 lod | i32 x,y,z | u64 payloadOffset | u32 payloadBytes
//     (payloadBytes == 0 is the cooked-EMPTY marker: the negative
//     cache's cooked form — no slot is ever allocated for it)
//   Payloads (brick blocks at their offsets, z-major tight layout —
//     exactly the atlas slot layout)
//
// VFS/pak route: the file rides the pak as a Texture-kind blob under its
// VFS path (the PakBackend serves by path — zero pak-format change) or
// loose in the content tree; the runtime reads it through OverlayVfs.
// The index's offsets make per-brick lazy reads a later refinement with
// NO format change — v1 loads the blob once, the reader owns the bytes.
//
// Thread-safety: the loaded asset is IMMUTABLE (CookedBrickSource serves
// bricks from it concurrently on worker threads).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/smvk.h>
#include <Engine/WorldFields/WorldField.h>

namespace engine::worldfields {

// ---------------------------------------------------------------------------
// Header — packed, little-endian; every field pinned by static_asserts.
// ---------------------------------------------------------------------------
struct WorldFieldAssetHeader
{
	char magic[4] = { 'T', 'W', 'F', 'B' };
	uint16_t formatKind = 1; // 1 = BC4_UNORM blocks
	uint16_t encoding = uint16_t(WorldFieldEncoding::SqrtUNorm);
	float baseVoxelSizeMeters = 1.0f;
	uint16_t lodCount = 1;
	uint16_t brickInteriorRes = kBrickInteriorRes; // 64 (pinned)
	uint16_t storedRes = kBrickStoredRes; // 68 (pinned)
	uint16_t reserved = 0;
	float originMeters[3] = { 0.0f, 0.0f, 0.0f };
	float decodeScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	float decodeBias[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float fallbackValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t brickCount = 0;
	uint32_t indexBytes = 0; // brickCount * sizeof(IndexRow)
	uint64_t payloadBytes = 0;
};
static_assert(sizeof(WorldFieldAssetHeader) == 96, "WorldFieldAssetHeader size: pinned format");

struct WorldFieldAssetIndexRow
{
	uint32_t lod = 0;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint64_t payloadOffset = 0; // into the payload region (after the index)
	uint32_t payloadBytes = 0; // 0 = the cooked-EMPTY marker (occupancy)
	// (the u64 at offset 16 forces the row to 32 B / align 8 — pinned; no
	// separate occupancy word: payloadBytes == 0 IS the empty state)
};
static_assert(sizeof(WorldFieldAssetIndexRow) == 32, "WorldFieldAssetIndexRow size: pinned format");

// ---------------------------------------------------------------------------
// Builder (cook side / tests): collects bricks + empties, writes the
// sorted index + payload arena. All pointer+count per the API convention.
// ---------------------------------------------------------------------------
class WorldFieldAssetBuilder
{
public:
	explicit WorldFieldAssetBuilder(smvk::Format format,
		WorldFieldEncoding encoding, float baseVoxelSizeMeters,
		uint16_t lodCount, glm::vec3 originMeters,
		glm::vec4 decodeScale, glm::vec4 decodeBias, glm::vec4 fallbackValue);

	// A brick's already-encoded payload (blocks; the caller validated the
	// byte math — AddBrick asserts the size matches the format).
	void AddBrick(uint32_t lod, int32_t x, int32_t y, int32_t z,
		const std::byte* blocks, size_t blockBytes);
	// A cooked-EMPTY brick: index row, no payload, never a slot.
	void AddEmpty(uint32_t lod, int32_t x, int32_t y, int32_t z);

	uint32_t BrickCount() const { return uint32_t(rows_.size()); }

	// Serialize into caller storage; returns bytes written (0 on
	// capacity shortfall). WriteToFile wraps it (fopen "wb").
	size_t Serialize(std::byte* out, size_t outCapacity) const;
	bool WriteToFile(const char* path) const;

private:
	WorldFieldAssetHeader header_{};
	std::vector<WorldFieldAssetIndexRow> rows_;
	std::vector<std::byte> payloads_;
};

// ---------------------------------------------------------------------------
// Reader — the immutable loaded view.
// ---------------------------------------------------------------------------
class WorldFieldAsset
{
public:
	enum class BrickLookup : uint8_t
	{
		Payload, // found, encoded blocks follow
		Empty, // found, cooked-empty (never a slot)
		Missing, // not in the index (outside the cooked region)
	};

	// Loads + validates (magic, sizes, offsets). The asset OWNS its
	// bytes (a copy in the LoadFromMemory form — the VFS/pak blob can be
	// released afterwards).
	static bool LoadFromMemory(WorldFieldAsset* out,
		const std::byte* data, size_t bytes);
	static bool LoadFromFile(WorldFieldAsset* out, const char* path);

	// ---- accessors (pointer + count) ----
	const WorldFieldAssetHeader& Header() const { return header_; }
	const WorldFieldAssetIndexRow* Rows() const { return rowsView_; }
	uint32_t BrickCount() const { return header_.brickCount; }
	const std::byte* PayloadBase() const
	{
		return owned_.empty() ? nullptr : owned_.data() + payloadBaseOffset_;
	}

	BrickLookup FindBrick(uint32_t lod, int32_t x, int32_t y, int32_t z,
		const std::byte** outPayload, size_t* outBytes) const;

	// The runtime field descriptor this asset feeds (paging knobs —
	// table dims, pool shape — stay deployment choices; format/encoding/
	// voxel size/origin/decode MUST come from the asset).
	WorldFieldDesc MakeDesc(const char* name,
		uint32_t tableDimX, uint32_t tableDimY, uint32_t tableDimZ,
		uint32_t poolSlotsX, uint32_t poolSlotsY, uint32_t poolSlotsZ) const;

private:
	WorldFieldAssetHeader header_{};
	std::vector<std::byte> owned_; // the loaded bytes
	const WorldFieldAssetIndexRow* rowsView_ = nullptr; // into owned_
	size_t payloadBaseOffset_ = 0;
};

// ---------------------------------------------------------------------------
// CookedBrickSource — the asset-backed IWorldFieldBrickSource (immutable
// reads; safe on worker threads).
// ---------------------------------------------------------------------------
class CookedBrickSource final : public IWorldFieldBrickSource
{
public:
	explicit CookedBrickSource(const WorldFieldAsset& asset)
		: asset_(&asset)
	{
	}

	bool GenerateBrick(const WorldFieldDesc& field, uint32_t lod,
		int32_t x, int32_t y, int32_t z,
		std::byte* payload, size_t payloadBytes) override;

private:
	const WorldFieldAsset* asset_; // not owned; immutable
};

} // namespace engine::worldfields
