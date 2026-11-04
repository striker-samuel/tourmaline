# TODO — merged-as-draft follow-up work

PRs #136–#142 were merged while still drafts (owner request, 2026-11-04).
Each entry records the work the PR author explicitly flagged as incomplete
in its own description. Local gates (full all-targets build + ctest +
Python tooling tests) verified compilation and existing regressions; the
items below are the remaining author-declared gaps.

## #136 — HDR screen distortion / heat shimmer
- GPU validation not run in the authoring environment: Vulkan validation
  layers, resize/reformat lifecycle, and identity/neutral-map behavior
  need a GPU-capable pass.
- Output-quality and performance validation (pre-bloom/tonemap insertion
  point, optional flow-map binding) still to be reviewed on real scenes.

## #137 — Ground dust cloud particles
- Soft depth *fading* is not implemented (depth testing exists; the
  fragment shader needs a graph-safe sampled-depth path).
- Ground height assumes a locally flat plane at the emitter position;
  terrain-following heightfield sampling is a follow-up.
- Overlapping alpha sprites are not depth-sorted; consider sorting or
  weighted blended OIT for dense clouds.
- Native build, GPU validation and capture tests were never run by the
  author (static source review only) — the local merge gate compiled and
  ran the CPU suite; visual validation remains open.

## #139 — Day/night solar preview
- Save/reopen persistence and scrubbing interaction not yet validated.
- Shadow behavior when the sun is below the horizon not yet validated.
- Editor preview only: no cooked-runtime serialized cycle or gameplay
  clock — the app runtime will not autonomously advance time; a scene
  directional light override path needs defining.
- (Merge note: the standalone smoke test was unregistered and
  assert-only; it is now `EditorDayNightTests` with `#undef NDEBUG`.)

## #140 — Opaque thin-surface backlighting
- HLSL variant compilation verified by the merge gate, but render
  validation of sun/point/spot rear contributions on leaves, cloth and
  paper is still open.
- Rear-light contribution is unshadowed by design — verify strength
  stays modest on real assets.
- Author's C++/test-run checklist items beyond the merge gate (render
  tests) remain open.

## #141 — Particle source graph
- Stacked on #137; merged last after retarget. Author-declared validation
  gaps: see the PR body's checklist (typed graph topology coverage and
  editor brush workflow validation were not completed).

## #142 — Scoped 15-frame GPU snapshot capture
- Batch export still performs 15 individual synchronous image downloads
  (deferred to the boundary, not aggregated): a single aggregated
  Vulkan image-to-buffer transfer + one host readback is the follow-up
  RHI work.
- GPU/driver-dependent runtime tests were never run by the author
  (static review only) — a GPU capture regression (record + verify +
  compare against legacy mode) remains open.
