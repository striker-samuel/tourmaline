// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// The scene node identity vocabulary — shared by scene.h (the graph)
// and scene_tables.h (the typed association tables) without a cycle.
//
// EPOCH 6b (docs/scene_tables_migration.md): NodeId is a STABLE SLOT
// index, not a dense array index. The per-node parallel vectors keep
// their []-indexing shape but no longer compact on deletion — a
// deleted node's slot goes to the scene's free list and every OTHER
// node's id stays put ("handles survive node deletion without
// renumbering"). Freshly-built scenes allocate slots sequentially, so
// a scene that never deleted anything is bit-identical to the old
// dense representation — the pak/scene serializers rely on that
// (they compact live slots to dense on write and expand on read; the
// cooked format stays dense by the locked decision).
//
// GENERATIONS guard against slot reuse (the ABA problem): every slot
// carries a 12-bit generation in Scene::nodeGeneration, bumped on
// retirement. A NodeId alone is an INDEX (fine for internal plumbing
// — hierarchy links, rows, per-frame dirty lists — where the
// deletion discipline drops dangling references at retirement time).
// A LONG-LIVED reference (editor history, selections, anything that
// can outlive a deletion) should hold a NodeHandle instead: the slot
// plus the generation it was minted at, validatable after the fact.
//
// (the namespace/RHI boundary audit, todo item 8.4): the declarations
// live in tour::scene — the global-namespace cleanup's first slice.
// The GLOBAL aliases below keep the whole tree's unqualified `NodeId`
// spellings compiling through the consumer migration; retire them as
// call sites move to tour::scene::NodeId.
#include <cstdint>

namespace tour::scene {

using NodeId = uint32_t;

// ---- slot geometry (pinned: the handle packing + the slot ceiling) ----
// 20 bits of slot (1,048,576 — the pak reader's node-count guard is
// exactly 2^20, so any cookable scene fits) + 12 bits of generation
// (4,096 lives per slot before the counter wraps; the editor would
// need 4,096 retire/reuse cycles on ONE slot to alias a handle).
inline constexpr uint32_t kNodeSlotBits = 20;
inline constexpr uint32_t kNodeSlotMask = (1u << kNodeSlotBits) - 1u;
inline constexpr uint32_t kNodeGenBits = 32 - kNodeSlotBits;
inline constexpr uint32_t kNodeGenMask = (1u << kNodeGenBits) - 1u;
// The slot ceiling addNode enforces before appending (slot kNodeSlotMask
// is reserved so the packed all-ones value stays the invalid handle).
inline constexpr uint32_t kNodeSlotCountMax = 1u << kNodeSlotBits;

// ---- packed generational handle (long-lived node references) ----
// (generation << 20) | slot. kNodeInvalidHandle == the scene id
// sentinel UINT32_MAX, so a handle and a bare NodeId never mix by
// accident at the invalid value either.
using NodeHandle = uint32_t;
inline constexpr NodeHandle kNodeInvalidHandle = 0xFFFFFFFFu;

inline constexpr uint32_t NodeHandleSlot(NodeHandle h)
{
	return h & kNodeSlotMask;
}
inline constexpr uint32_t NodeHandleGen(NodeHandle h)
{
	return (h >> kNodeSlotBits) & kNodeGenMask;
}
inline constexpr NodeHandle PackNodeHandle(uint32_t slot, uint32_t gen)
{
	return (NodeHandle((gen & kNodeGenMask) << kNodeSlotBits))
		| NodeHandle(slot & kNodeSlotMask);
}

} // namespace tour::scene

// ---- GLOBAL compat aliases (the migration bridge; retire with the
// call sites — the boundary scanner guards against NEW global
// declarations, these are the tracked pre-existing set) ----
using NodeId = tour::scene::NodeId;
using NodeHandle = tour::scene::NodeHandle;
using tour::scene::kNodeSlotBits;
using tour::scene::kNodeSlotMask;
using tour::scene::kNodeGenBits;
using tour::scene::kNodeGenMask;
using tour::scene::kNodeSlotCountMax;
using tour::scene::kNodeInvalidHandle;
using tour::scene::NodeHandleSlot;
using tour::scene::NodeHandleGen;
using tour::scene::PackNodeHandle;
