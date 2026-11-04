# Lighting tile diagnostics in the editor

The **View > Light tile diagnostics (Live Lit)** menu exposes the
retained deferred-shadow tile visualizers through the editor's borrowed-world
`WorldRenderer`. These diagnose the tile lists fed by the Forward+ light
set, without reproducing culling or inventing light counts in the editor.

## Why the original implementation was hidden

The editor includes `DS_DEBUG_VIZ` code in all top-level development
configurations (Debug and optimized Release). Shader phases 2 and 3 have
separate clean and diagnostic SPIR-V variants. The clean versions remain
selected when visualization is Off, so their extra telemetry stores are
absent rather than guarded by a per-tile branch.

- Turning on a visualization selects the `.viz.spv` variants for phases 2/3.
- Turning it Off selects the regular production-equivalent `.spv` variants.
- Embedded builds without an editor still honor `TOURMALINE_DS_VIZ` and the
  Debug auto default. The explicit `shipping` profile excludes diagnostics.
- Optimized editor builds do not require `-DTOURMALINE_DS_VIZ=ON`.
- The remaining Off cost is compiled shader size and loaded module/pipeline
  memory; there are no phase-2/3 diagnostic stores or debug visualization reads.

## How to use

1. Open a scene with point/spot lights and select **Fully Lit** (F1).
2. Open **View > Light tile diagnostics (Live Lit)**.
3. Choose **Approximate count**, **Accurate count**, **Final light count**,
   or **Overestimate** to see phase-by-phase light-list density. A high
   approximation-to-accurate difference indicates wasted candidates.
4. Use **Cull stage**, **List membership**, or **Visibility** with a
   **phase-1 sorted light index** to diagnose an individual light. This
   index is **not** an outliner node ID; it may change when lights move.
5. Toggle **Overlay on lit** to compare the colored diagnostic with the
   scene, or show the debug readout alone. Adjust **Count heat ceiling**
   to resolve useful gradients in a dense scene.
6. Set **Off** to disable instrumentation. The controls are session-only:
   no `.tour` commands or authored scene settings are modified.

## Limits and safeguards

- Requires the **deferred-shadow** pipeline (the original DS debug buffer
  producers); it is not a visualization of the separate classic Forward+
  fallback tile list used when DS is disabled.
- Requires the editor's **Live Lit** view, which uses the full
  `WorldRenderer`, not the legacy editor preview shader.
- Baked irradiance takes precedence over tile diagnostics.
- Counts are **light-list occupancy / culling diagnostics**, not a
  GPU-time cost estimator. Measure performance separately with the
  benchmark harness and GPU profiling.
- No extra GPU readback loop or new render pass was introduced: the
  existing visualizers and their compile-time gating are reused.

## Suggested manual verification

- Configure/build engine, Editor and shaders in both Debug and Release;
  confirm the menu and every visualization mode appear without a special flag.
- Verify the editor shader output contains both the normal phase-2/3 SPIR-V
  modules and the `.viz.spv` variants; inspect a normal frame with GPU
  tooling to confirm the clean variants are bound when Off.
- Toggle modes **Off / heat / selected-light / Off** while moving a light;
  confirm the scene resumes its normal lit view and editor transforms persist.
- Compare approx/accurate/final under a dense light crowd; test empty scenes.
- Disable deferred shadows and confirm the view does not display stale tiles.
- Build a `shipping` profile: no diagnostic shader variants should be
  required, and the normal shader path must remain unaffected.
