# Cascaded world-tile cubemap captures (draft integration)

This implementation builds on the existing `CubemapOctreeOptions` /
`BuildOctreeProbePositions` code. It does **not** restore the removed
`.sggi` *diffuse GI* octree; that system was explicitly retired in October.

## Intended user contract

Choose a finite world-space AABB and a capture density in metres. The
CPU planner subdivides its XZ footprint along the **existing 256 m
WorldPartition root cells** (including negative coordinates) and runs the
existing sparse placement octree per cell. Its Y range is the authored
volume. By default the cascade schedule is:

| Camera distance | Offline capture target |
| --- | --- |
| 0–64 m | 2 m cells |
| 64–256 m | 3 m cells |
| 256–512 m | 5 m cells |
| 512 m and beyond | **Existing environment IBL / sky** (no extra captures) |

The same tile may have multiple pre-baked capture tiers. `SelectCubemapCascade`
selects a tier from distance; the final return value is `cascades.size()`,
which means **sky fallback** and requires no baked KTX. No runtime octree
regrowth is needed when the camera moves. The authored AABB and the
schedule are independent of the camera.

`CubemapTileProvider` is a CPU-only `IWorldStreamProvider` for
`WorldPartition` with `StreamLayer::Environment`. It addresses
`WorldTileKey`, tracks pending/completed tile capture-set requests with
monotonic tickets and safely rejects late completion after cancellation.
The host's retirement callback must reject teardown until GPU references
have completed. The provider does **not** invent a pak-per-tile layout.

## How to use the planning API

```cpp
engine::worldpartition::CubemapCaptureVolume volume;
volume.min = {-50, 0, -50};
volume.max = {300, 40, 300};
volume.cascades = {{64, 2}, {256, 3}, {512, 5}};

std::vector<engine::worldpartition::CubemapTileCaptureSet> sets;
std::string error;
const bool ok = engine::worldpartition::PlanCubemapTileCaptures(
    volume, triangleBvhOcclusionRay, sets, error);
```

The octree keeps the existing budget-aware coverage behavior. A tight
density target is not a hard guarantee: budget limits may prevent complete
refinement. Callers should report emitted counts and coarse cells rather
than silently claiming exact coverage.

## Integration work still required

This PR is a **draft foundation, not a finished production capture system**:

1. Persist authored bounded capture volumes and cascade schedules in the
   scene's export settings / editor UI and the `.tour` command schema.
2. Have the cook pipeline emit tiered `CubemapTileCaptureSet` capture
   manifests and KTX rows keyed by WorldTileKey, retaining the existing
   deterministic bake input hash and artifact identity rules.
3. Publish these rows as independently readable logical tile records
   using the existing SMP8 / ContentRegistry paths (not one pak per tile).
4. Register the cubemap provider with the renderer's partition at attach,
   forward its camera/map interests, and bind callbacks to the existing
   cubemap texture loader and safe GPU deferred-retirement system.
5. Select a tile's appropriate **already-baked** cascade per interest
   (the current provider requests a tile capture-set ID and does not yet
   select per-cascade KTX). Implement distance hysteresis and a smooth
   blend to the existing sky IBL at the last tier.
6. Preserve pinned captures' authoring priority, world/cell ownership,
   and existing runtime resident limits. Verify no old 96-row manifest
   truncation loses far tiles; tile-owned capture manifests must be
   independently indexed.
7. Run the headless tests plus a full native build and a multi-kilometre
   Vulkan walk/teleport with small resident budgets before enabling
   production behavior.

Existing single-scene cubemap cooks and runtime streaming remain
unchanged while this work is incomplete.

## Tests

`scene_tile_provider_tests` now exercises the new planner's cell
boundaries and sky tier, plus provider request deduplication, ticket
validation and retirement. The tests have been added but are **not yet
executed in this environment**.
