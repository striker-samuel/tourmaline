// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldFieldAtlasPool — resident voxel storage for one storage format.
//
// One persistent 3D image whose extent is slotGrid * storedBrickSize
// (e.g. 6x6x5 slots of 68^3 texels = 408x408x340). Bricks are assigned
// to SLOTS inside the atlas — no per-brick texture objects, one bindless
// index for hundreds of resident bricks. The image is created with
// mipLevels=1 (higher virtual LODs are independently resident bricks,
// NOT atlas mips — an atlas mip chain would mix unrelated slots) and
// lives until the system dies (NOT a rendergraph transient; like the
// SG-GI probe grids it is a persistent bindless volume sampled outside
// pass declaration).
//
// CPU bookkeeping (free-list + per-slot owner) runs fully headless — the
// GPU image appears lazily via EnsureGpu, so the residency logic and its
// tests need no Vulkan device. The STRICT-RESOURCES contract applies at
// EnsureGpu: an empty create returns false after OnResourceExhaustion
// and the system degrades to CPU-only (never aborts outside --strict).
//
// The pool does NOT know about eviction policy or LODs — the system
// decides victims (it owns last-use clocks) and calls ReleaseSlot. The
// per-slot owner tag exists so a stale release is caught loudly.

#include <cstdint>
#include <string>
#include <vector>

#include <Engine/smvk.h>
#include <Engine/Memory/pool.h>
#include <Engine/WorldFields/WorldFieldTypes.h>

namespace engine::worldfields {

class WorldFieldAtlasPool
{
public:
	struct Desc
	{
		smvk::Format format = smvk::Format::R8_UNorm;
		uint32_t storedBrickSize = kBrickStoredRes; // texels per axis per slot
		uint32_t slotsX = 1;
		uint32_t slotsY = 1;
		uint32_t slotsZ = 1;
		// Owned copy — the source WorldFieldDesc (and its std::string)
		// dies at RegisterField return; the debug name must outlive it
		// until the lazy EnsureGpu.
		std::string name = "world field atlas";
	};

	explicit WorldFieldAtlasPool(const Desc& desc);

	// ---- CPU bookkeeping (headless) ----
	uint32_t SlotCount() const { return slotTotal_; }
	uint32_t FreeSlotCount() const { return static_cast<uint32_t>(freeSlots_.size()); }
	bool SlotFree(uint32_t slot) const;
	// Pop a free slot and bind it to `owner` (false = pool exhausted —
	// the caller skips the request gracefully; never evicts here).
	bool TryAcquireSlot(const WorldBrickKey& owner, uint32_t& outSlot);
	// Free a slot (asserts the current owner matches — a mismatched
	// release is a pager bookkeeping bug, programmer error).
	void ReleaseSlot(uint32_t slot, const WorldBrickKey& owner);
	const WorldBrickKey& SlotOwner(uint32_t slot) const; // kFreeSlotKey when free

	// ---- layout math ----
	uint32_t StoredBrickBytes() const; // tight stored-brick payload bytes
	smvk::Extent3D AtlasExtent() const; // slots * stored, per axis
	smvk::ImageRegion3D SlotRegion(uint32_t slot) const; // the upload region
	void SlotOrigin(uint32_t slot, uint32_t outXYZ[3]) const; // slot grid pos

	// ---- GPU (lazy; headless pools skip this entirely) ----
	bool EnsureGpu(smvk::ResourceManager& ctx);
	bool GpuReady() const { return bindlessIndex_ != UINT32_MAX; }
	uint32_t BindlessIndex() const { return bindlessIndex_; }
	smvk::ImageHandle Image() const; // empty until EnsureGpu
	uint64_t AtlasBytes() const; // committed payload estimate

private:
	Desc desc_;
	uint32_t slotTotal_ = 0;
	std::vector<uint32_t> freeSlots_; // LIFO free list
	std::vector<WorldBrickKey> slotOwners_; // slotTotal_ entries
	smvk::Holder<smvk::TextureHandle> atlas_; // image+view+bindless record
	smvk::ImageHandle image_{}; // the atlas VkImage (region-upload target)
	uint32_t bindlessIndex_ = UINT32_MAX;
};

} // namespace engine::worldfields
