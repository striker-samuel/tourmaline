# Probe pre-mix plan (Remember Me architecture, GPU-only selection)

Design record for the next reflection-probe iteration. Reference: Lagarde/
Zanuttini, "Local Image-based Lighting with Parallax-corrected Cubemaps"
(SIGGRAPH/GPU Pro 4, the Remember Me renderer).

## Scope decisions (owner constraints)

- **No CPU probe classification/selection.** The GPU system we already
  have is the selection: the resident probes ride the Forward+ light
  buffer tail (type-2 entries, position/influence/fade/box packed in
  `MakeProbeLightEntry`), refreshed by the existing cull + streaming.
  A GPU compute pass reads that tail and computes the blend weights at
  the point of interest. The CPU only uploads what it already uploads.
- **No planar (floor) reflection system.** The floor gets the same
  blended cubemap as everything else; the dedicated planar path is
  deferred to the future signed-field representation.
- **The existing per-pixel walk stays** as the A/B comparison path
  (env pin), not deleted.
- Per-pixel cost DROPS: the walk's <=8 cubemap fetches per lit pixel
  become ONE fetch of the pre-blended cubemap.

## What we keep from the current system

- The bake: 126-cell octree (surface-first priority growth, viability
  gating), depth-fitted parallax boxes, ambient closure, GGX prefiltered
  mip chains (mips are blended mip-to-mip, never re-prefiltered - the
  Remember Me rule).
- The streaming: nearest-K residency within the 64/48 budgets.
- The probe tail packing (forwardplus.h) - the GPU weight pass reads it
  directly, so no new per-probe CPU upload exists anywhere.

## Architecture (per frame)

```
[existing] FP cull + streaming -> probe tail in the light buffer
[NEW A]    Compute "probe weights" (1 thread):
             read probe tail, evaluate weights at POI (camera pos),
             special cases, select top-K, write {indices[K], weights[K],
             count, blended bindless index, dominant parallax box}
[NEW B]    Compute "cubemap pre-mix" (8x8):
             for each (face, mip, texel): sum(w_i * sample(cubemap_i,
             dir, mip)) -> the blended cubemap image (128^2 x 6 x 8 mips)
[existing] Main lit pass:
             env pin OFF -> the current per-pixel walk (<=8 fetches)
             env pin ON  -> ONE fetch of the blended cubemap
                           (+ box parallax from the dominant box)
```

The blended cubemap is allocated once (128x128 RGBA16F, 6 faces, full
mip chain, bindless). Pass B rewrites it every frame (131k texels x K
fetches - a fraction of a ms; Remember Me measured 0.08 ms/probe on a
PS3, modern hardware is noise). Frame-to-frame the weights are
continuous in the camera position, so the blend morphs smoothly - no
per-pixel probe-set switching anywhere, which is the root of every
tile/block artifact class we have fought so far.

## Phase 1 - pre-mix with the CURRENT weights (isolate the mechanism)

Blend the top-K probes (K=4) at the camera POI using the exact current
weighting (`wEff = fadeWeight^3 * influence^2` - the validated look).
Everything else unchanged. Deliverable: the A/B (`TOURMALINE_PROBE_PREMIX=0|1`)
with deterministic captures + the raw-cubemap debug view. Question to
answer: does ONE uniform mixture kill the remaining blockiness? (It must
by construction - all pixels sample the same texture - the question is
whether the POI approximation stays believable at the hall's far end.)

## Phase 2 - the Remember Me NDF weights

Swap the weight function to the published transition-region formula:

```
d_i   = (dist - influence_i) / fade_i          (NDF; <=0 inner, >=1 out)
S_d   = sum(d_j),  S_r = sum(1 - d_j)
a_i   = (1 - d_i / S_d) / (N - 1)
b_i   = (1 - d_i) / S_r
W_i   = a_i * b_i / sum(a_j * b_j)
```

Special cases (GPU pass, deterministic tie-breaks by weight then
influence then index):
- inner hit (d_i <= 0): the dominant probe gets exclusive weight
  (multiple inner hits: the designated dominant wins - the documented
  Remember Me behavior, including its known limitation at inner-region
  exits)
- one candidate: weight 1
- zero candidates: the blended index is invalidated; the material falls
  back to the existing global-cubemap/analytic-sky path untouched
- K selection: top-K by W, break ties by influence (wide wins - the
  validated policy)

CAUTION (from the reference): Remember Me favored SMALL local volumes
over wide ones - the opposite of our validated wide-first. Copying the
NDF alone without keeping our influence-width tie-break could re-aggrav
the banner-red. Phase 2 explicitly A/Bs both tie-breaks.

## Status (2026-10-17)

- Phase 1 DONE (commit 202d2643): mechanism A/B'd - 0.00% differing
  pixels ON vs OFF (max delta 0.3), bench 4.72ms ON vs 5.02ms OFF
  (256 frames), raw-blend debug view uniform (no tiles by construction).
- Phase 2 DONE (commit 2439faba): the NDF weights + the CPU mirror
  (`ComputeNdfWeights` in probepremix.h, pinned by 6 unit tests incl.
  the published two-probe 85.7%/14.3% example) + the two-phase GPU
  weights pass (gather-64, then dominant/one/zero cases, then a*b +
  top-4). Same A/B contract: 0.00% differing pixels, bench 5.08ms ON vs
  5.25ms OFF. The inner-hit tie-break stays WIDE-first (the validated
  policy; the NDF's small-volume preference would re-aggrav the
  banner-red). The current Sponza POI sits inside one influence volume,
  so the NDF degenerates to the dominant probe there and matches the
  walk's blend - the formula's spatial variation shows up only in
  overlap-transition regions (fade rings between volumes).
- Phase 3 (the dominant-box lerp is NOT done - the material uses slot
  0's box as packed): FrameUniforms tail DONE, environment.hlsli branch
  DONE, the pin + AGENTS.md/README.md notes DONE, the unit tests DONE;
  the Debug HUD readout of the selected indices/weights remains open
  (the weights pass writes them - display only).

## Phase 3 - plumbing, observability, bench

- FrameUniforms: `blendedCubemapIndex`, the dominant parallax box
  (weight-lerped across the selected set), the pin flag.
- `environment.hlsli`: the pre-mix branch is ONE fetch + the existing
  BoxParallaxCorrect with the dominant box; the walk stays as the
  fallback path (pin off / zero candidates).
- Debug HUD: the selected indices/weights readout (the weight pass
  already wrote them - display in the cubemaps panel).
- Bench rows: `premix on/off` (the mix-pass cost vs the per-pixel walk
  savings on lit-heavy views), capture determinism (fixed-dt bench pin
  already in place), the debug views on the blended content.
- The `CubemapTests` unit tests for the weight-pass special cases
  (inner/one/zero/K-cap/tie-break) - the math is pure and mirrors on
  the CPU for the test.

## Explicitly out of scope

- CPU-side probe classification or per-object probe lists.
- The planar reflection texture, mirrored camera, image proxies - the
  floor's exact reflection comes with the signed-field representation.
- Re-prefiltering the mixed base mip (the mips blend mip-to-mip; the
  GGX chains are already baked per capture).

## Files (expected)

- `engine/include/Engine/Rendergraph/probepremix.h` + impl: the two
  compute passes + the blended cubemap resource (the module owns the
  image, the bindless registration, the selection buffer).
- `engine/src/Renderer/WorldRenderer.cpp`: pass ordering (A then B then
  the main chain), the frame-uniform fields, the env pin.
- `engine/shaders/environment.hlsli`: the pre-mix branch.
- `engine/shaders/probepremix_weights.cs.hlsl` + `probepremix_mix.cs.hlsl`.
- `tests/test_cubemap.cpp`: the weight-pass mirror tests.
- `AGENTS.md` / `README.md`: the new pin + the architecture note.
