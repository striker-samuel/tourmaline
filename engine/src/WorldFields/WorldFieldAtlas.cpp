// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <cassert>

#include <Engine/WorldFields/WorldFieldAtlas.h>

namespace engine::worldfields {

WorldFieldAtlasPool::WorldFieldAtlasPool(const Desc& desc)
	: desc_(desc)
{
	const smvk::FormatBlockInfo fmt = smvk::GetFormatBlockInfo(desc_.format);
	assert(desc_.format != smvk::Format::Undefined);
	assert(fmt.bytesPerBlock > 0 && "atlas format must carry a payload (no depth/stencil)");
	assert(desc_.storedBrickSize > 0 && (desc_.storedBrickSize % fmt.blockWidth) == 0
		&& (desc_.storedBrickSize % fmt.blockHeight) == 0
		&& "stored brick must be block-aligned (kBrickStoredRes=68 is 4-divisible for BCn)");
	assert(desc_.slotsX > 0 && desc_.slotsY > 0 && desc_.slotsZ > 0);

	slotTotal_ = desc_.slotsX * desc_.slotsY * desc_.slotsZ;
	slotOwners_.assign(slotTotal_, kFreeSlotKey);
	freeSlots_.reserve(slotTotal_);
	// Reverse order so slot 0 pops first (deterministic paging in tests).
	for (uint32_t s = slotTotal_; s-- > 0;)
		freeSlots_.push_back(s);
}

bool WorldFieldAtlasPool::SlotFree(uint32_t slot) const
{
	assert(slot < slotTotal_);
	return slot < slotTotal_ && slotOwners_[slot].fieldId == kFreeSlotFieldId;
}

bool WorldFieldAtlasPool::TryAcquireSlot(const WorldBrickKey& owner, uint32_t& outSlot)
{
	if (freeSlots_.empty())
		return false; // graceful: the caller skips the request this frame
	outSlot = freeSlots_.back();
	freeSlots_.pop_back();
	assert(slotOwners_[outSlot].fieldId == kFreeSlotFieldId && "free-list slot was bound");
	slotOwners_[outSlot] = owner;
	return true;
}

void WorldFieldAtlasPool::ReleaseSlot(uint32_t slot, const WorldBrickKey& owner)
{
	assert(slot < slotTotal_ && "ReleaseSlot: slot out of range");
	if (slot >= slotTotal_)
		return;
	// A mismatched owner means the pager released a slot twice or for the
	// wrong brick — its own bookkeeping bug, loud in every build (then a
	// defensive no-op so a bad release cannot corrupt the free list).
	if (!(slotOwners_[slot] == owner)) {
		assert(false && "ReleaseSlot: slot owner mismatch (pager bookkeeping bug)");
		return;
	}
	slotOwners_[slot] = kFreeSlotKey;
	freeSlots_.push_back(slot);
}

const WorldBrickKey& WorldFieldAtlasPool::SlotOwner(uint32_t slot) const
{
	assert(slot < slotTotal_);
	return slot < slotTotal_ ? slotOwners_[slot] : kFreeSlotKey;
}

uint32_t WorldFieldAtlasPool::StoredBrickBytes() const
{
	const smvk::FormatBlockInfo fmt = smvk::GetFormatBlockInfo(desc_.format);
	const uint32_t blocksX = desc_.storedBrickSize / fmt.blockWidth;
	const uint32_t blocksY = desc_.storedBrickSize / fmt.blockHeight;
	return blocksX * blocksY * desc_.storedBrickSize * fmt.bytesPerBlock;
}

smvk::Extent3D WorldFieldAtlasPool::AtlasExtent() const
{
	return smvk::Extent3D{
		desc_.slotsX * desc_.storedBrickSize,
		desc_.slotsY * desc_.storedBrickSize,
		desc_.slotsZ * desc_.storedBrickSize,
	};
}

void WorldFieldAtlasPool::SlotOrigin(uint32_t slot, uint32_t outXYZ[3]) const
{
	assert(slot < slotTotal_);
	outXYZ[0] = slot % desc_.slotsX;
	outXYZ[1] = (slot / desc_.slotsX) % desc_.slotsY;
	outXYZ[2] = slot / (desc_.slotsX * desc_.slotsY);
}

smvk::ImageRegion3D WorldFieldAtlasPool::SlotRegion(uint32_t slot) const
{
	uint32_t xyz[3];
	SlotOrigin(slot, xyz);
	smvk::ImageRegion3D r{};
	r.mipLevel = 0;
	r.baseArrayLayer = 0;
	r.offset = smvk::Offset3D{
		int32_t(xyz[0] * desc_.storedBrickSize),
		int32_t(xyz[1] * desc_.storedBrickSize),
		int32_t(xyz[2] * desc_.storedBrickSize),
	};
	r.extent = smvk::Extent3D{ desc_.storedBrickSize, desc_.storedBrickSize, desc_.storedBrickSize };
	return r;
}

bool WorldFieldAtlasPool::EnsureGpu(smvk::ResourceManager& ctx)
{
	if (bindlessIndex_ != UINT32_MAX)
		return true;

	smvk::ImageCreateInfo ci{};
	ci.type = smvk::ImageType::Image3D;
	ci.extent = AtlasExtent();
	ci.format = desc_.format;
	// Sampled (bindless volume), TransferDst (the pager's region uploads),
	// TransferSrc (debug dumps + the GPU smoke-test readback).
	ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst
		| smvk::ImageUsage::TransferSrc;
	ci.mipLevels = 1; // virtual LODs are resident bricks, never atlas mips
	ci.arrayLayers = 1;
	// CONCURRENT graphics+compute+transfer (CreateImageImpl widens the
	// family set when the queues are distinct): the pager writes from
	// the transfer queue while fragment/compute sample — no per-page
	// queue-family ownership ping-pong (uploadImageRegion keys its
	// release/acquire pair off isConcurrent exactly like uploadImage).
	ci.concurrentCompute = true;
	ci.name = desc_.name.c_str();

	// No initial data: the atlas content arrives per-brick through
	// UploadDataToImage3DRegion; UNDEFINED until the first region upload
	// (uploadImageRegion's first-upload path handles that layout).
	smvk::Holder<smvk::TextureHandle> tex = ctx.CreateTexture(ci, nullptr);
	if (tex.empty()) {
		// STRICT-RESOURCES contract: exhaustion is the caller's policy.
		smvk::OnResourceExhaustion("WorldFieldAtlasPool::EnsureGpu");
		return false; // CPU-only degradation, never an abort
	}
	bindlessIndex_ = ctx.GetTextureBindlessIndex(tex);
	image_ = ctx.GetTextureImageHandle(tex); // the pager's region-upload target
	atlas_ = std::move(tex);
	return true;
}

smvk::ImageHandle WorldFieldAtlasPool::Image() const { return image_; }

uint64_t WorldFieldAtlasPool::AtlasBytes() const
{
	if (bindlessIndex_ == UINT32_MAX)
		return 0;
	return uint64_t(StoredBrickBytes()) * slotTotal_;
}

} // namespace engine::worldfields
