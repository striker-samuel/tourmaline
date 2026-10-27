# World Partition & Streaming — the unified large-world architecture

The single reference for the spatial streaming stack. The WorldFields
system (milestones 1-4 of its own plan) is the FIRST live provider; this
document is the umbrella every later client (terrain, ecology, scene,
collision, navigation) plugs into.

```
WorldPartition            — WHAT parts of the world exist in memory:
  spatial hierarchy, streaming interests, priorities, lifecycle,
  budgets (Engine/WorldPartition/WorldPartition.h)
        |
Resource providers       — HOW each subsystem stores/renders its data
        |
Specialized resident     — the physical allocators (2D terrain atlases,
caches                     3D field atlas pools, geometry buffers, CPU
                           collision tiles)
        |
Renderer / Simulation
```

**The rule that keeps this sane:** the partition never forces one
physical representation on a client. Terrain is predominantly a 2D
heightfield (2D pages + a quadtree); fog/wind are sparse 3D bricks;
scene geometry is instances + meshlets. The SHARED elements are the
world addressing, the streaming interest flow and the residency
accounting — not the memory layout.

## The world grid (1 world unit = 1 metre)

| Level | Size | Responsibility |
|---|---|---|
| Root world cell | 256 x 256 m (XZ) | scene streaming, object ownership, coarse terrain data |
| Subcell | 64 x 64 m | terrain patches, vegetation groups, local streaming |
| Terrain sample | ~1 m | heightmap resolution near the camera |
| Fog brick | 64^3 voxels @ 1 m | one brick per 64 m subcell footprint |
| Wind brick | 64^3 voxels @ 4 m | one brick per 256 m root cell footprint |

`WorldCellKey`/`WorldSubcellKey` are signed 64-bit grid indices with
FLOOR semantics (`GetWorldCell` — -1 m is cell -1; exactly -256 m is
cell -1; the field-brick addressing uses the same discipline so the
schemes agree at shared boundaries). Interests carry `dvec3`
positions + velocities.

The alignment relationships (`BricksPerRootCell`) are RELATIONSHIPS, not
ownership: a fog volume spanning several cells is one logical field
asset whose bricks stream independently. World cells are 2D for terrain
and ordinary scene content; 3D addressing exists for world-field bricks
and future stacked/cave environments — do NOT make scene object
partitioning 3D because one resource type is volumetric.

## The streaming model

```
Unloaded -> Requested -> IO/Generate -> CpuReady -> GPU upload ->
GpuReady -> Active -> Retiring
```

A resource being loaded is NOT safe to use. CPU-only resources
(collision, navigation) activate from CpuReady; GPU resources need
upload completion, barriers and valid descriptor/page-table publication
(the WorldFields publication contract — region uploads, then the page
table, one transfer batch that executes only after every previously
submitted frame retired — is ONE instance of this rule).

Interests (`StreamingInterest`) carry position, velocity, radius,
priority, a layer mask and a desired residency. The prefetch rule:
`d_prefetch >= v_max * t_worstReady + d_safety` — the WorldFields
clipmaps implement it as a velocity lead on the window centers
(`interestLeadSeconds`); tune from measured end-to-end latency. Do not
prioritize by distance alone (motion lookahead, visibility, gameplay
criticality all belong in `P`).

The frame sequence (`WorldPartition::UpdateFrame`, run BEFORE frame
render data is built):

1. `SetInterests` — the frame's streaming sources (camera, players, teleports)
2. providers respond to the resolved interest set (`OnStreamingInterests`)
3. gather + resolve (`GatherRequests` — dedupe/priority engine lands here)
4. dispatch CPU work/IO/uploads within budgets
5. publish only completed, safe-to-use resources (`CommitFrame`)
6. retire, respecting in-flight GPU references (`RetireFrame`)

Worker threads do IO/decode/generation (the WorldFields brick jobs on
the dedicated ThreadPool); GPU publication and scene mutation happen
ONLY at the commit sync point. Resources finishing after the
publication point defer to the next frame — by design.

## What exists today (the first vertical slice)

- **WorldPartition v1** (`Engine/WorldPartition/`): cell/subcell
  addressing, interests (priority-first), the provider seam
  (`IWorldStreamProvider`), the request currency, the frame sequence,
  active-cell accounting. CPU-pure, test-pinned
  (`tests/test_world_partition.cpp`).
- **WorldFields as the first provider** (`WorldFieldProvider`):
  `CommitFrame -> PrepareFrameInterests` — the clipmap centers follow
  the highest-priority WorldField interest, led by velocity; async brick
  generation on a dedicated 2-worker pool (milestone 3: in-flight cap,
  stale-result drops on teleport/source-swap, the publish state
  machine — all invariant-tested under churn + teleports).
- **Cooked bricks (milestone 4)** (`WorldFieldAsset`): the indexed
  `.wfb` form — per-brick BC4_UNORM blocks + occupancy rows, sqrt
  density encoding, binary-searched lookups, byte-identical payload
  identity through the pager, fidelity-tested against a dense reference
  (including exact brick-boundary positions and the
  sqrt-filtering-noncommutativity bound).

The App wiring: `WorldRenderer` owns the partition + the provider +
the work pool; the frame builds the camera interest (position +
measured velocity) and calls `UpdateFrame` before `UpdateFrameUniforms`
(the field table BDA rides `FrameUniforms::worldFields`).

## Milestones

1. **[DONE]** Partition core — cells, interests, providers, sequence,
   debug map accounting; synthetic cells, no terrain.
2. **Streamed terrain** — 2D height/material atlases (R16_UNORM
   heights, splat weights), a reusable patch grid displaced in the
   vertex shader from the height atlas, a quadtree patch selector with
   parent-LOD fallback (a patch splits only when its fine children are
   resident), neighbor LOD difference <= 1 + skirts/geomorphing, and
   neighbors deriving shared edge samples from ONE canonical
   heightfield. Terrain PAINTING edits pages + dirties neighbors +
   border regions; the CPU source data stays authoritative (edits
   survive eviction via persisted pages/deltas).
3. **[DONE for fields]** WorldFields provider — residency follows the
   same interests as everything else (fog + wind register as
   independent fields on the shared infrastructure).
4. **Scene streaming** — per-cell manifests referencing SHARED assets
   (instances, not duplicated meshes); batched activation at sync
   points. Do NOT stream by loading/unloading whole clusters
   (`World::UnloadCluster`'s `ResolveAndFixup_clusters()` is a load-time
   path). An incremental instance registry with generational handles +
   reusable slots + deferred GPU retirement replaces per-cell fixups;
   persistent gameplay entities stay OUTSIDE disposable streamed
   presentation.
5. **Collision, navigation, persistence** — CPU-authoritative world
   data synchronized with visual streaming; fast movement, teleports,
   edited terrain, persistent entity state.

The ecology layer (biomes/ecotopes as another partition data layer:
R8_UINT biome IDs at 8 m/texel, ecotope IDs + RGBA8 factors at 2 m/texel
— a streamed 2D surface atlas feeding deterministic world-space
candidate placement, NOT per-frame scattering) and the GPU
instance generator (candidates -> suitability -> compact 16-24 B
instance payloads -> culling -> indirect draws) build on the same
provider seam after terrain. Format the ecology data BEFORE the
instancer — then the instancer is a generic placement-and-rendering
system, not vegetation-specific rules.

## Memory budgets (engineering targets, not measurements)

Separate caps per resident cache, shared accounting:

| Cache | Physical form | Eviction unit |
|---|---|---|
| Terrain heights | R16 2D atlas | height page |
| Terrain materials | 2D atlas | material page |
| Ecology surface | 2D atlases | page |
| Fog/wind/etc | 3D atlas pools | volume brick |
| Geometry | GPU buffers | mesh/cluster allocation |
| Instances | GPU instance buffers | cell instance range |
| Collision | CPU physics structures | collision tile |
| Navigation | CPU nav data | nav tile |

Starting profile to test: ~64 MiB world fields (the fog BC4 pool of
180 slots is ~27 MiB + the wind RGBA8 pool ~22 MiB), ~96 MiB terrain
atlases, ~256 MiB streaming geometry. The constant-zero page state for
empty fog bricks (an occupancy row, never a physical slot) is the
`.wfb` EMPTY form.

## Instrumentation

Residency affects the entire game; expose it first-class in the editor:
the partition residency map (active/prefetched/coarse cells), the
memory-residency bars, and per-provider latency histograms (time in
each lifecycle state, bytes read/uploaded per frame, atlas misses,
fallback LOD frequency, evictions, cancellations, GPU retirement
delays) on the existing Optick profiling.

## Validation scenario (the gate for every milestone)

A camera travelling continuously across several kilometres, then a
teleport to a distant location, under a deliberately small memory
budget: never uninitialized tiles, invalid atlas pages, destroyed
scene resources, or cracks where LODs meet; coarse terrain stays
present while finer resources fetch. (The WorldFields instance of this
gate is `tests/test_world_fields.cpp`'s churn + teleport invariants
plus `WorldFieldsGpuTests`' device readbacks.)
