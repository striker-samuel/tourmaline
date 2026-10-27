// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cassert>
#include <cstring>

#include <Engine/WorldFields/WorldFieldAsset.h>
#include <Engine/WorldFields/WorldFieldCodec.h>

namespace engine::worldfields {

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

WorldFieldAssetBuilder::WorldFieldAssetBuilder(smvk::Format format,
	WorldFieldEncoding encoding, float baseVoxelSizeMeters,
	uint16_t lodCount, glm::vec3 originMeters,
	glm::vec4 decodeScale, glm::vec4 decodeBias, glm::vec4 fallbackValue)
{
	assert(format == smvk::Format::BC4_UNorm
		&& "v1 asset form carries BC4_UNORM blocks (the static-density codec)");
	header_.formatKind = 1;
	header_.encoding = uint16_t(encoding);
	header_.baseVoxelSizeMeters = baseVoxelSizeMeters;
	header_.lodCount = lodCount;
	header_.brickInteriorRes = kBrickInteriorRes;
	header_.storedRes = kBrickStoredRes;
	header_.reserved = 0;
	header_.originMeters[0] = originMeters.x;
	header_.originMeters[1] = originMeters.y;
	header_.originMeters[2] = originMeters.z;
	header_.decodeScale[0] = decodeScale.x;
	header_.decodeScale[1] = decodeScale.y;
	header_.decodeScale[2] = decodeScale.z;
	header_.decodeScale[3] = decodeScale.w;
	header_.decodeBias[0] = decodeBias.x;
	header_.decodeBias[1] = decodeBias.y;
	header_.decodeBias[2] = decodeBias.z;
	header_.decodeBias[3] = decodeBias.w;
	header_.fallbackValue[0] = fallbackValue.x;
	header_.fallbackValue[1] = fallbackValue.y;
	header_.fallbackValue[2] = fallbackValue.z;
	header_.fallbackValue[3] = fallbackValue.w;
	header_.brickCount = 0;
	header_.indexBytes = 0;
	header_.payloadBytes = 0;
}

void WorldFieldAssetBuilder::AddBrick(uint32_t lod, int32_t x, int32_t y,
	int32_t z, const std::byte* blocks, size_t blockBytes)
{
	assert(blocks != nullptr);
	// The one BC4 brick size (68/4)^2 * 68 * 8 bytes.
	const size_t expected = StoredBrickBlockBytes(kBrickStoredRes, 4, 4, 8);
	assert(blockBytes == expected && "AddBrick: BC4 stored-brick block size");
	if (blockBytes != expected)
		return;
	WorldFieldAssetIndexRow row{};
	row.lod = lod;
	row.x = x;
	row.y = y;
	row.z = z;
	row.payloadOffset = payloads_.size();
	row.payloadBytes = uint32_t(blockBytes);
	rows_.push_back(row);
	payloads_.insert(payloads_.end(), blocks, blocks + blockBytes);
}

void WorldFieldAssetBuilder::AddEmpty(uint32_t lod, int32_t x, int32_t y, int32_t z)
{
	WorldFieldAssetIndexRow row{};
	row.lod = lod;
	row.x = x;
	row.y = y;
	row.z = z;
	row.payloadOffset = 0;
	row.payloadBytes = 0; // EMPTY: index row only, never a slot
	rows_.push_back(row);
}

size_t WorldFieldAssetBuilder::Serialize(std::byte* out, size_t outCapacity) const
{
	const size_t total = sizeof(WorldFieldAssetHeader)
		+ rows_.size() * sizeof(WorldFieldAssetIndexRow) + payloads_.size();
	if (out == nullptr || outCapacity < total)
		return 0;

	// Sort the index (lod, x, y, z) — deterministic lookups + a stable
	// file for the pak pipeline's freshness contract.
	std::vector<WorldFieldAssetIndexRow> sorted = rows_;
	std::sort(sorted.begin(), sorted.end(),
		[](const WorldFieldAssetIndexRow& a, const WorldFieldAssetIndexRow& b) {
			if (a.lod != b.lod) return a.lod < b.lod;
			if (a.x != b.x) return a.x < b.x;
			if (a.y != b.y) return a.y < b.y;
			return a.z < b.z;
		});

	WorldFieldAssetHeader h = header_;
	h.brickCount = uint32_t(sorted.size());
	h.indexBytes = uint32_t(sorted.size() * sizeof(WorldFieldAssetIndexRow));
	h.payloadBytes = payloads_.size();
	std::memcpy(out, &h, sizeof(h));
	std::memcpy(out + sizeof(WorldFieldAssetHeader), sorted.data(), h.indexBytes);
	if (!payloads_.empty())
		std::memcpy(out + sizeof(WorldFieldAssetHeader) + h.indexBytes,
			payloads_.data(), payloads_.size());
	return total;
}

bool WorldFieldAssetBuilder::WriteToFile(const char* path) const
{
	assert(path != nullptr);
	const size_t total = sizeof(WorldFieldAssetHeader)
		+ rows_.size() * sizeof(WorldFieldAssetIndexRow) + payloads_.size();
	std::vector<std::byte> bytes;
	bytes.resize(total);
	if (Serialize(bytes.data(), bytes.size()) != total)
		return false;
	std::FILE* f = std::fopen(path, "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	std::fclose(f);
	return ok;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

bool WorldFieldAsset::LoadFromMemory(WorldFieldAsset* out,
	const std::byte* data, size_t bytes)
{
	assert(out != nullptr && data != nullptr);
	if (bytes < sizeof(WorldFieldAssetHeader))
		return false;
	WorldFieldAssetHeader h{};
	std::memcpy(&h, data, sizeof(h));
	if (std::memcmp(h.magic, "TWFB", 4) != 0)
		return false; // fail closed on any other magic (no converters)
	if (h.brickInteriorRes != kBrickInteriorRes || h.storedRes != kBrickStoredRes)
		return false; // the brick-geometry layout contract
	if (h.formatKind != 1)
		return false; // unknown payload form
	if (h.lodCount == 0 || h.lodCount > kMaxFieldLods)
		return false;
	const size_t indexBytes = size_t(h.brickCount) * sizeof(WorldFieldAssetIndexRow);
	if (h.indexBytes != uint32_t(indexBytes)
		|| sizeof(WorldFieldAssetHeader) + indexBytes + h.payloadBytes != bytes)
		return false;

	out->owned_.assign(data, data + bytes);
	out->header_ = h;
	out->rowsView_ = reinterpret_cast<const WorldFieldAssetIndexRow*>(
		out->owned_.data() + sizeof(WorldFieldAssetHeader));
	out->payloadBaseOffset_ = sizeof(WorldFieldAssetHeader) + indexBytes;

	// Offset bounds validation (one linear pass — a corrupt/torn file
	// fails the load instead of handing the pager garbage).
	for (uint32_t i = 0; i < h.brickCount; ++i) {
		const WorldFieldAssetIndexRow& r = out->rowsView_[i];
		if (r.payloadBytes != 0u) {
			if (r.payloadOffset + r.payloadBytes > h.payloadBytes)
				return false;
			const size_t expected = StoredBrickBlockBytes(kBrickStoredRes, 4, 4, 8);
			if (r.payloadBytes != expected)
				return false;
		}
	}
	return true;
}

bool WorldFieldAsset::LoadFromFile(WorldFieldAsset* out, const char* path)
{
	assert(out != nullptr && path != nullptr);
	std::FILE* f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	// (Windows long is 32-bit — a `1l << 31` cap overflows negative and
	// rejects EVERY file; compare as unsigned int64 against a sane 2 GiB)
	if (size <= 0 || uint64_t(size) > (uint64_t(1) << 31)) {
		std::fclose(f);
		return false;
	}
	// (brace-init: a parenthesized size_t(size) would most-vexing-parse
	// into a function declaration returning vector<std::byte>)
	std::vector<std::byte> bytes{ size_t(size) };
	const bool read = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
	std::fclose(f);
	if (!read)
		return false;
	return LoadFromMemory(out, bytes.data(), bytes.size());
}

WorldFieldAsset::BrickLookup WorldFieldAsset::FindBrick(uint32_t lod,
	int32_t x, int32_t y, int32_t z,
	const std::byte** outPayload, size_t* outBytes) const
{
	// Binary search over the sorted index (the hot path — one brick per
	// generated page).
	int64_t lo = 0, hi = int64_t(header_.brickCount) - 1;
	while (lo <= hi) {
		const int64_t mid = lo + (hi - lo) / 2;
		const WorldFieldAssetIndexRow& r = rowsView_[mid];
		if (r.lod != lod) {
			if (r.lod < lod) lo = mid + 1;
			else hi = mid - 1;
			continue;
		}
		if (r.x != x) {
			if (r.x < x) lo = mid + 1;
			else hi = mid - 1;
			continue;
		}
		if (r.y != y) {
			if (r.y < y) lo = mid + 1;
			else hi = mid - 1;
			continue;
		}
		if (r.z != z) {
			if (r.z < z) lo = mid + 1;
			else hi = mid - 1;
			continue;
		}
		// Found.
		if (r.payloadBytes == 0u) {
			if (outPayload) *outPayload = nullptr;
			if (outBytes) *outBytes = 0;
			return BrickLookup::Empty;
		}
		if (outPayload)
			*outPayload = PayloadBase() + r.payloadOffset;
		if (outBytes)
			*outBytes = r.payloadBytes;
		return BrickLookup::Payload;
	}
	if (outPayload) *outPayload = nullptr;
	if (outBytes) *outBytes = 0;
	return BrickLookup::Missing;
}

WorldFieldDesc WorldFieldAsset::MakeDesc(const char* name,
	uint32_t tableDimX, uint32_t tableDimY, uint32_t tableDimZ,
	uint32_t poolSlotsX, uint32_t poolSlotsY, uint32_t poolSlotsZ) const
{
	// The asset OWNS the data semantics; the deployment owns paging.
	WorldFieldDesc d{};
	d.name = name ? name : "cooked world field";
	d.format = smvk::Format::BC4_UNorm; // formatKind 1
	d.encoding = WorldFieldEncoding(header_.encoding);
	d.updateMode = WorldFieldUpdateMode::StaticStreamed;
	d.baseVoxelSizeMeters = header_.baseVoxelSizeMeters;
	d.brickResolution = header_.brickInteriorRes;
	d.lodCount = header_.lodCount;
	d.originMeters = glm::vec3(header_.originMeters[0],
		header_.originMeters[1], header_.originMeters[2]);
	d.tableDimX = tableDimX;
	d.tableDimY = tableDimY;
	d.tableDimZ = tableDimZ;
	d.poolSlotsX = poolSlotsX;
	d.poolSlotsY = poolSlotsY;
	d.poolSlotsZ = poolSlotsZ;
	d.decodeScale = glm::vec4(header_.decodeScale[0], header_.decodeScale[1],
		header_.decodeScale[2], header_.decodeScale[3]);
	d.decodeBias = glm::vec4(header_.decodeBias[0], header_.decodeBias[1],
		header_.decodeBias[2], header_.decodeBias[3]);
	d.fallbackValue = glm::vec4(header_.fallbackValue[0], header_.fallbackValue[1],
		header_.fallbackValue[2], header_.fallbackValue[3]);
	return d;
}

// ---------------------------------------------------------------------------
// CookedBrickSource
// ---------------------------------------------------------------------------

bool CookedBrickSource::GenerateBrick(const WorldFieldDesc& field, uint32_t lod,
	int32_t x, int32_t y, int32_t z,
	std::byte* payload, size_t payloadBytes)
{
	// The desc came from MakeDesc on this asset — the payload size the
	// pager reserved matches the asset's blocks. (A mismatched desc is
	// a wiring bug; fail closed rather than serve torn data.)
	const size_t expected = StoredBrickBlockBytes(kBrickStoredRes, 4, 4, 8);
	if (payloadBytes != expected || field.format != smvk::Format::BC4_UNorm)
		return false;

	const std::byte* blocks = nullptr;
	size_t blockBytes = 0;
	const WorldFieldAsset::BrickLookup lookup =
		asset_->FindBrick(lod, x, y, z, &blocks, &blockBytes);
	if (lookup == WorldFieldAsset::BrickLookup::Missing
		|| lookup == WorldFieldAsset::BrickLookup::Empty)
		return false; // outside the cooked region / cooked-empty: known-absent
	if (blockBytes != payloadBytes)
		return false;
	std::memcpy(payload, blocks, payloadBytes);
	return true;
}

} // namespace engine::worldfields
