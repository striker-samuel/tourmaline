# WorldChannels foundation (incremental implementation)

Status: **foundation implemented; runtime migration not complete**.

## Purpose

World data is not intrinsically a heightfield. `WorldChannels` models a set of
world-qualified logical datasets independently of `tour::Map` (authored
scene placement) and `WorldPartition` (spatial demand and stream lifecycle).
`WorldPartition` remains the sole coordinator; no second scheduler exists.

## API

`Engine/WorldPartition/WorldChannels.h` provides:

- `ChannelDesc`: stable ID, world, name, domain, sample representation,
  streaming classification, independent cell dimension and sample count.
- `WorldChannels`: unique per-world registry, discovery by ID/name, typed
  lookup, explicit provider attachment and unregister.
- `IWorldChannel`: logical channel contract with optional existing
  `IWorldStreamProvider` integration.
- `SurfaceChannel<T>`: simple CPU-resident scalar tile store, explicit
  missing/not-resident sampling states, editing revisions and eviction.
- `HeightfieldChannel` and `BiomeChannel`: demonstration of two independent
  formats and spatial resolutions using the same implementation.

## Lifetime and correctness

`WorldPartition::AddProvider` borrows provider pointers. Hosts must detach
providers before unregistering channels or tearing down a world. Do not assume
the channel registry owns the active `WorldPartition`. The registry does not
automatically publish uploaded textures, schedule IO, or recook edited tiles.

Each data source must preserve durable edits **outside** transient resident
tiles before eviction. The foundation stores an edit revision to support
invalidation but **does not yet implement persistence**. Do not evict edited
tiles without a host persistence/flush step.

`SurfaceChannel<T>::At` is a nearest-lower-grid-sample query; it is not a
replacement for the existing heightfield's filtering, seam topology, normal
calculation or high-performance GPU path.

## What remains before a full migration

1. Integrate the existing heightfield asset/paint source as an adapter rather
   than maintaining a parallel authoritative height copy.
2. Define channel-qualified resource keys and full typed 2D/3D/objects demand
   independently of the fixed `StreamLayer` enum and root-cell `StreamRequest`.
3. Implement real asynchronous channel IO/cook, fallback, budgets, dependency
   tracking and fence-safe GPU retirement through `WorldPartition`.
4. Design world-authored channel manifests and lossless on-disk edit persistence.
5. Make runtime terrain, physics, placement, rendering, and editor queries use
   channel facades while preserving the optimized internal heightfield path.
6. Retain historical `.tour` `CreateTerrain`, `TerrainPaint` and
   `kCreateTerrainRefBase` meanings; migration requires separate fixtures.
7. Add GPU, large-world, teleport, multi-world, seam and persistence tests.

This is intentionally an incremental PR. Neither the application renderer nor
the terrain editor is redirected to the new registry yet. See
`docs/world_partition.md`, `docs/terrain_world_semantics.md` and
`docs/terrain_legacy_migration_contract.md` for existing constraints.
