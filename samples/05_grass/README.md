# 05_grass — million-blade GPU terrain placement stress sample

Build the `05_grass` target using the project's normal engine + DXC
setup, then run the executable from the build root (where the engine's
`content/shaders/*.spv` outputs live).

```sh
cmake --build out/build/ci --target 05_grass
cd out/build/ci
./05_grass
```

The executable creates its own 256 m × 256 m rolling heightfield and
fully painted ecotope channel. No imported assets or packaged world
are required. It demonstrates the **existing** placement compute
path, not a million `World` nodes or per-frame CPU draw calls:

1. Sixteen 64 m tiles: GPU placement-scatter into persistent buffers
2. Per-camera GPU frustum and per-species-distance culling
3. GPU prefix scan + visible-index compaction
4. GPU-built indexed indirect draw command per resident tile
5. A rotating camera and depth-tested grass blade quads

The blade prototype is a two-triangle, double-sided, narrow vertical
quad with random yaw and size. One candidate produces one blade.
By default there are 16 candidates/m² × 256² m² = **1,048,576 blades**
before camera culling, subject to the GPU's existing 65,536-instance
per-tile bound.

## Knobs

- `GRASS_DENSITY`: integer from 1 to 16 (default 16). For example,
  `GRASS_DENSITY=4` yields 262,144 candidates.
- `GRASS_DISTANCE`: max species draw distance in metres, 32–512
  (default 190). The GPU culler applies this before generating
  per-tile visible-index lists.

The demo does **not** read back per-blade buffers. Do not use the
raw candidate count as a measured visible-blade count.

## WorldRenderer hookup

The production WorldRenderer also exposes the *opt-in*
`SetGpuPlacementRuntime(runtime, seeds, seedCount, interestRadius)`:
pass an external `PlacementWorldRuntime` whose `layer` and `masks`
remain alive and whose optional `hfield` points at the world
heightfield (or is null to borrow WorldRenderer's built terrain).
The `seeds` table contains absolute static index ranges from the
mesh heap; the renderer copies these small command headers, not
instance data. The renderer then drives
`BeginFrame → RecordCullChain → Main DrawIndexedIndirect`.
`InvalidateGpuPlacementTerrain()` increments the terrain dependency
revision on edits.

The integration is intentionally feature-off until configured, and
the stock GfxFilter and GPU-scene diagnostic paths are unchanged.
The new Main renderer consumption is diagnostic-unlit only; complete
lit-material binding, motion-vector/prepass/shadow submissions,
authoritative double-world camera transforms, GPU-scene LOD
transitions and streaming budgets remain future work.

## Validation before treating it as a benchmark

Build on Windows/MSVC and Linux/GCC, run `PlacementGpuTests` on a
Vulkan-capable GPU, measure frame times across densities, zoom in on
tile seams, test camera movement across tile residency boundaries,
and verify GPU counters report no placement overflow. The resulting
visual sample is a load test, not a promise of a target FPS.
