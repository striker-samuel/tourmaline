# Experimental interleaved CSM work batches

## Scope / hypothesis

This is a **raster-order pilot**, not implementation of world-tile streaming or async compute. The existing shared coarse selection and fused multi-view GPUScene paths already produce per-cascade indirect work. The pilot slices the existing class-strided command buffers into three contiguous ranges and visits cascades round-robin (12 directional PCF passes instead of 4). It neither changes caster selection nor guarantees spatially contiguous world tiles: command ordering follows existing compaction/work expansion. All cascade compute builds still precede raster. This establishes the cost/benefit of interleaving before investing in actual tile-ready producer/consumer scheduling. No claimed performance gain.

## Startup knob

- Unset or `TOURMALINE_SHADOW_INTERLEAVE=0`: original schedule.
- `TOURMALINE_SHADOW_INTERLEAVE=1`: experimental schedule when all four GPUScene cascade submissions are ready and have room for three slices, and directional EVSM is inactive. Otherwise use baseline, including direct-only and partial GPUScene fallback.
- The pin is read once per process; restart to change. Set GPUScene shadows explicitly (`TOURMALINE_GPU_SCENE_SHADOWS=1`) and use PCF. Log `[ShadowInterleave] batches=3` confirms engagement; batches=1 is fallback, not a valid performance sample for the experiment.

The first visit clears its own atlas tile; later visits load/store prior depth. Each class's original command interval is covered once; direct/non-GPUScene casters run only on the last visit. No shader changes or persistent ABI changes. Spot, point and volumetric paths are not split. The existing PCF timestamp spans first through last raster pass (including local shadows when enabled).

## Validation gates (integrator)

1. Build App/Benchmark and ShadowWorkBatchTests; run `ctest --test-dir <build> -R 'shadow_work_batch_tests|cascade_shadow_bias_tests|rendergraph_contract|static_rendergraph' --output-on-failure`. Run normal CI too. CPU tests check the knob, fallback, round-robin ordering, first/last flags, empty/odd/large ranges and exact chunk coverage. They do not test GPU attachments.
2. Capture baseline and experiment with identical camera, sun, frame, LOD, SDSM/HZB state, pak, resolution, GPUScene ownership and frozen animation/time. Use Vulkan validation + synchronization validation; zero new errors or hazards. Inspect a GPU capture: 4 vs 12 directional passes, clear each atlas rect exactly once, subsequent visits LOAD/STORE and preserve every sibling rect; verify indirect/work/view dependencies, no premature sampling, same per-cascade indirect command addresses and total command slots (count zero padding separately), direct fallback once, correct timer endpoints. Check local-light and fog consumers.
3. Compare raw D32 atlas per cascade (expect bit-identical deterministic captures; investigate every differing texel, do not waive systematic mismatches), screen-space shadow mask and final image. Include overlapping occluders, grazing sun, tile boundaries, masked/two-sided foliage, mixed direct/skinned/GPUScene casters, empty and distant cascades, camera cuts and streaming transitions. Repeat with EVSM, missing GPUScene, and shadows off; verify baseline/fallback and no extra passes. Run legacy and fused cull paths separately if available.
4. Bench identical cooked scenarios: dense repeated meshes (e.g. bistro grid), landscape/vegetation, sparse control; stable camera and moving camera. Use 5+ repeats after warmup, identical clocks/config, alternating off/on, record GPU/driver and logs. Example commands from repo root (replace SCENARIO and BUILD):

```sh
cmake --build BUILD --target Benchmark ShadowWorkBatchTests
python tools/perf_harness.py bench --scenario SCENARIO --exe BUILD/Benchmark --repeat 5 --env TOURMALINE_GPU_SCENE_SHADOWS=1 --env TOURMALINE_SHADOW_INTERLEAVE=0 --output out/perf/shadow-off
python tools/perf_harness.py bench --scenario SCENARIO --exe BUILD/Benchmark --repeat 5 --env TOURMALINE_GPU_SCENE_SHADOWS=1 --env TOURMALINE_SHADOW_INTERLEAVE=1 --output out/perf/shadow-on
```

The stock compare mode rejects differing environment pins. Preserve both raw result.json files; compare manually or with an explicitly reviewed A/B comparator that permits ONLY this pin difference. Never edit the archived environment to trick the comparator.
5. Report median and p95 frame/CPU/GPU times, directional-only raster timings from GPU capture (existing PCF stage can include local lights), GPUScene build time, pass/barrier count, nonzero indirect draws, VS invocations, depth/early-Z rejection and depth/DRAM traffic where supported. Report missing counters as unavailable. Inspect GPU queue timeline; this pilot cannot prove compute/raster overlap.

Suggested promotion gate (not measured): all correctness gates pass, reproducible improvement >=5% in directional shadow GPU time on at least one heavy target scene, no >2% median GPU frame regression or >5% p95 regression on control workloads. Report noise/repeats; retain default OFF until reviewed. If raster cost grows, keep the negative result and do not enable by default.

## Next phase: actual spatial producer/consumer experiment

If the pilot establishes a tolerable attachment/revisit cost, use resident spatial leaves/tiles (not atlas tiles) with unique instance ownership, conservative caster bounds and per-tile cascade masks. Keep baseline and pilot controls; add a separate mode for tile-ready compute/raster. Preserve GPU-only ownership on fused visibility path; do not reintroduce CPU instance walks. Partition by tile identity rather than arbitrary command indices, preserve order or use light-front-to-back group sorting, and guarantee each (instance, cascade) appears at most once. Bound scratch memory and avoid reusing in-flight command buffers; declare compute-write/indirect-read/vertex-read and depth LOAD/STORE dependencies. Compare group sizes and actual queue overlap; no split should repeat full-cascade cull or geometry processing. Validate exact caster coverage and atlas equivalence against the baseline before comparing performance.
