// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <Engine/smvk.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace smvk {

// ---------------------------------------------------------------------------
// Fixed-budget CPU virtual sub-allocator — the VmaVirtualBlock replacement.
//
// The static vertex/index monoblock sub-allocation is pure CPU offset math:
// no device calls, no backend types. Same contract shape as the VMA virtual
// block it replaces: aligned allocation inside a fixed budget returning an
// opaque uint64 token (0 = null, the VmaVirtualAllocation convention — the
// gfx filter's allocBits packing keeps working verbatim), explicit free,
// first-fit with a coalesced free list. The range RETIREMENT ordering is the
// CALLER's discipline (cluster.cpp's deferred frees) — the arena itself is
// immediate, exactly like vmaVirtualFree.
// ---------------------------------------------------------------------------
class StaticHeapArena
{
public:
	using AllocId = uint64_t; // 0 = null

	StaticHeapArena() = default;
	explicit StaticHeapArena(uint64_t capacity)
	{
		Initialize(capacity);
	}

	void Initialize(uint64_t capacity)
	{
		capacity_ = capacity;
		used_ = 0;
		// Full reset: a re-Initialize on a previously-used arena (a
		// re-attach / config-driven re-alloc) must rewind the bump top or
		// the arena keeps claiming exhaustion against the OLD top — the
		// free list was cleared, so top_ was the only live state left.
		top_ = 0;
		nextId_ = 1;
		free_.clear();
		live_.clear();
	}

	bool Valid() const { return capacity_ != 0; }
	uint64_t Capacity() const { return capacity_; }
	uint64_t Used() const { return used_; }

	// The vmaGetVirtualBlockStatistics equivalent (the retirement fuzz
	// test's leak oracle): live range count + live bytes.
	struct Statistics
	{
		uint64_t allocationCount = 0;
		uint64_t allocationBytes = 0;
	};
	Statistics Stats() const
	{
		Statistics out{};
		out.allocationCount = live_.size();
		out.allocationBytes = used_;
		return out;
	}

	// First-fit over the coalesced free list, bump at the top. Returns the
	// token (0 = budget exhausted, outOffset untouched) — mirrors
	// vmaVirtualAllocate's VK_ERROR and keeps the callers' rollback paths.
	AllocId Allocate(uint64_t size, uint64_t alignment, uint64_t& outOffset)
	{
		if (!Valid() || size == 0 || size > capacity_)
			return 0;
		alignment = alignment ? alignment : 1;

		// Free list first (sorted by offset, coalesced): align each hole's
		// start and take the first that still fits. The leading alignment
		// gap and the trailing remainder go back onto the list.
		for (size_t i = 0; i < free_.size(); ++i)
		{
			const uint64_t aligned =
				(free_[i].offset + alignment - 1) / alignment * alignment;
			const uint64_t end = free_[i].offset + free_[i].size;
			if (aligned >= end || end - aligned < size)
				continue;

			const uint64_t holeOffset = free_[i].offset;
			const uint64_t holeSize = free_[i].size;
			free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
			InsertFree(holeOffset, aligned - holeOffset);
			InsertFree(aligned + size,
			           holeOffset + holeSize - (aligned + size));

			const AllocId id = nextId_++;
			live_.push_back(Live{aligned, size, id});
			used_ += size;
			outOffset = aligned;
			return id;
		}

		// Bump at the top; the alignment gap stays free.
		const uint64_t aligned = (top_ + alignment - 1) / alignment * alignment;
		if (aligned + size > capacity_)
			return 0;
		InsertFree(top_, aligned - top_);
		const AllocId id = nextId_++;
		live_.push_back(Live{aligned, size, id});
		top_ = aligned + size;
		used_ += size;
		outOffset = aligned;
		return id;
	}

	void Free(AllocId id)
	{
		if (id == 0)
			return;
		for (size_t i = 0; i < live_.size(); ++i)
		{
			if (live_[i].id != id)
				continue;
			InsertFree(live_[i].offset, live_[i].size);
			used_ -= live_[i].size;
			live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
			return;
		}
	}

private:
	struct Live
	{
		uint64_t offset;
		uint64_t size;
		AllocId id; // 0 on the free list
	};

	void InsertFree(uint64_t offset, uint64_t size)
	{
		if (size == 0)
			return;
		// Sorted insert + neighbor coalesce (the list is offset-sorted).
		size_t at = free_.size();
		for (size_t i = 0; i < free_.size(); ++i)
		{
			if (free_[i].offset > offset)
			{
				at = i;
				break;
			}
		}
		free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(at),
		             Live{offset, size, 0});
		// Coalesce forward, then backward (stable single pass each).
		for (size_t i = 0; i + 1 < free_.size();)
		{
			if (free_[i].offset + free_[i].size == free_[i + 1].offset)
			{
				free_[i].size += free_[i + 1].size;
				free_.erase(free_.begin() +
				            static_cast<std::ptrdiff_t>(i + 1));
			}
			else
			{
				++i;
			}
		}
	}

	uint64_t capacity_ = 0;
	uint64_t used_ = 0;
	uint64_t top_ = 0;
	AllocId nextId_ = 1;
	std::vector<Live> live_; // live ranges (id != 0)
	std::vector<Live> free_; // free ranges (id == 0), offset-sorted, coalesced
};

// ---------------------------------------------------------------------------
// The engine-wide static vertex/index monoblock heap (moved from
// Vulkan/detail/vulkan_impl.h): the three RHI buffer handles ARE the
// backend-neutral part (AllocateStaticVertexFmt in mesh.cpp creates them
// through the facade); the two arenas replace the VmaVirtualBlocks. The
// buffers are lifetime-global (freed with the resource pools at RM teardown).
// ---------------------------------------------------------------------------
struct StaticVertexFmtHeap
{
	BufferHandle m_staticVertexPosBuffer{};
	BufferHandle m_staticVertexShadingBuffer{};
	// OPTIONAL lightmap stream (PackedVertexLightmap, 8 B/vertex):
	// carved only for meshes whose vertices carry chart data —
	// independently packed (its windows do NOT mirror the pos/shading
	// vertexBase), so consumers address it through per-mesh bases
	// (StaticMesh::lightmapBase / the draw ring's misc2.w delta), never
	// through baseVertex at offset 0.
	BufferHandle m_staticVertexLightmapBuffer{};
	BufferHandle m_staticIndexBuffer{};
	StaticHeapArena m_staticVertexBlock; // VERTEX units (pos + shading streams)
	StaticHeapArena m_staticLightmapBlock; // VERTEX units (the lightmap stream)
	StaticHeapArena m_staticIndexBlock;  // BYTES (u32 index monoblock)
};
inline StaticVertexFmtHeap staticVertexFmtHeap;

// One mesh's carved ranges. The arena tokens free RETIREMENT-ORDERED
// (cluster.cpp's deferred frees) — never in place while a frame may still
// draw from the range; vertexBase/indexByteOffset are the published geometry
// (surfaces' firstIndex/baseVertex address the monoblock through them).
// lightmapBase/lightmapAllocation are 0/absent for non-lightmapped meshes
// (the optional stream).
struct StaticMeshAllocation
{
	StaticHeapArena::AllocId vertexAllocation = 0;
	StaticHeapArena::AllocId indexAllocation = 0;
	StaticHeapArena::AllocId lightmapAllocation = 0;
	uint64_t vertexBase = 0;      // VERTEX units into the pos/shading heaps
	uint64_t indexByteOffset = 0; // byte offset into the index monoblock
	uint64_t lightmapBase = 0;    // VERTEX units into the lightmap heap
	                              // (lightmapAllocation 0 = unused)
};

} // namespace smvk
