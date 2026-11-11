# One canonical GPU Scene — gfx-filter retirement

## Scope of this draft increment
The new `gpu_scene_render_route.h` provides a device-free, tested
classification contract for one final raster backend per GPU-scene work row.
It does not create a second instance database, duplicate material/LOD state,
or alter GPU draws yet. **gfx-filter has NOT been removed in this increment.**

Backend routing: 0=opaque back-cull, 1=opaque two-sided,
2=MASK back-cull, 3=MASK two-sided all prefer task/mesh, then indexed.
Genuine BLEND must select a dedicated transparent pass, not MASK or opaque
PSOs. Unsupported/unadmitted sources remain legacy-owned until parity.

## Migration stages before deleting gfx-filter
1. Feed the canonical routing decision from existing material flags and
   GPU-scene work records into GPU class-specific work generation. Enforce
   exactly one owner at each pass boundary, preserving source generations.
2. Make indexed fallback consume the SAME selected GPU-scene work records
   rather than running another candidate/visibility pipeline.
3. Move masked, skinned, lightmapped, layered, dithered and other legacy
   geometry across only when all needed depth/G-buffer/Main/shadow/motion
   variants are ready. Failure must leave whole-source compatibility ownership.
4. Add the true BLEND pass after opaque; depth test ON, writes OFF; choose
   sorting or OIT and validate overlapping/transmissive materials.
5. Once no sources depend on it, delete gfx-filter's candidate generation,
   temporal state, filtered-index scratch, pass dispatch and shader resources.
   Keep per-flight lifetime and fallback safety until every replacement passes.

## Validation
- [x] Unit contract: all 4 opaque/MASK classes, indexed fallback, stale/
      unadmitted sources, BLEND isolation.
- [ ] Integrate classification in WorldRenderer runtime and shader producers.
- [ ] Verify exactly-once submissions and no missing source when pipelines fail.
- [ ] Bistro/complex mixed-material visual parity and Vulkan validation.
- [ ] Preserve baseline task/mesh 0.825 ms draw / 2.61 ms total GPU.
- [ ] Remove gfx-filter and its bookkeeping after zero compatibility users.

This PR must remain **draft** until the runtime migration is implemented.
