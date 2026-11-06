# Instanced indirect draw promotion: measurement baseline

The GPU scene DB already owns persistent mesh, LOD, submesh, instance and
generational identity rows. The gfx filter separately builds frame-local draw
requests, batches, and GPU-written indirect commands. **Measure before promoting
repeated single-instance commands to instanced indirect draws.** This PR does
not implement instanced draws or change the GPU scene DB.

## Reproducible candidate census

Run a representative repeat-mesh scene with
`TOURMALINE_INSTANCE_METRICS=1`. Every 300 frames, the renderer writes
`[InstanceMetrics]` to its trace log (including the first sampled frame):

- `filtered candidates`: CPU draw requests admitted to the filter, after its
  CPU-side gates, **before** GPU HZB/meshlet culling.
- `groups`: distinct compatible candidate keys.
- `repeated_candidates` / `repeated_groups`: requests and keys occurring
  more than once.
- `largest_group`: largest compatible candidate group.
- `candidate_command_upper_bound`: candidates minus groups. This is only a
  theoretical saving if every group could be reduced to one command.
- `census_us`: CPU time spent on this opt-in diagnostic (not recording time).

The compatibility key includes selected gfx mesh/LOD, material, draw flags,
draw-set bucket, lightmap chunk/chart/delta, and resolved userdata; this is a
conservative, *candidate-level* census, not a proof of instancing legality.
It deliberately does **not** merge across incompatible material variants,
pipeline state, lightmap mappings, or skinning requirements. Real aggregation
may need fewer/more split keys after validating shader and render-pass behavior.

**These are not GPU-visible instance counts, actual indirect command counts,
draw-call counts, nor GPU timings.** The existing filter HUD has
`draws`, `commands`, cluster counts, and GPU-kept readback; use those
alongside the census. GPU culling can remove some/all candidates. Do not
infer FPS improvements or quote a command reduction as measured from this log.

## Promotion decision

1. Capture repeatable scenarios (few repeats, many repeats, distinct meshes)
   using `tools/perf_harness.py`, holding view, resolution, GPU and settings
   fixed. Record frame and GPU timings via the existing benchmark/profiler.
2. Compare the opt-in census to the existing filtered command and GPU-kept
   metrics. High repeated-candidate concentration suggests where to inspect.
3. Before an instanced-draw implementation, add GPU-accurate post-cull
   visible-instance and emitted-command counters/readback (or capture via a
   GPU debugger), and measure CPU draw-recording time separately from
   GPU cull/compact/dispatch timings. The existing batch statistics alone
   cannot establish per-instance visibility.
4. Test an A/B instanced command generator against the baseline; retain
   instancing only if CPU/GPU frame timings improve without regressions.

## GPU scene DB fit

Keep `gpu_scene::Instance` and `gpu_scene::Handle<Instance>` slot +
generation as the durable identity. Leaf members already refer to these
generational IDs; do not replace them with transient draw indices. The
aggregation/compaction outputs should be **per-frame**, per-flight
visible-instance references and indirect commands derived from current
valid DB rows; preserve generation validation, mesh residency/complete
bounds, per-instance LOD/culling, material/pipeline/skin variants, and
the current direct fallback for nonfilterable objects. Reusing DB identities
avoids a second authoritative instance registry, and keeps updates and
destruction independent of the draw-command layout.

A practical future layout is a visible-instance index buffer sorted by
compatible mesh/material/LOD state, plus an indirect command buffer with
`instanceCount` and `firstInstance` selecting each compacted range.
Shaders then use the instance index to fetch the correct DB row; be careful
about Vulkan `InstanceIndex` / `SV_InstanceID` first-instance semantics.
The current gfx filter fetches frame-local draw constants by draw index;
its shader contract must be migrated before enabling real instancing.
