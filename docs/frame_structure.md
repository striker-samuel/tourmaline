# Frame structure (render graph pass pipeline)

Steady-state main-view frame as assembled by `SceneRenderer` (the frame
function in app/src/scene_renderer.cpp, the gfx block around `depthSplit`).
Pass names are the rendergraph labels — `VULKANTOY_BARRIER_TRACE=on`
prints the executed list per frame ("[RG] pass pos=…"), which is the ground
truth for what actually ran and in what order.

Two depth assemblies exist; which one runs is decided per frame:

| Condition | Assembly |
| --- | --- |
| gfx filter ON **and** temporal seed primed depth | **Northlight depth split** (default steady state) |
| filter OFF, or first frame / invalidated history (no seed) | **legacy assembly** (full prepass) |

`VULKANTOY_SPLIT=0` forces the legacy assembly even when the seed primed
depth (A/B bisect switch). `VULKANTOY_HIZ=0` disables the Option A
occluder-depth source. See "Debug knobs" below.

## Northlight depth split (steady state)

```
G1 graphics   EvsmCascade0..3, SpotShadow0..3        shadow depth (normal-Z)
C  compute    EvsmBlurH/V x8                          EVSM moment blur (async)
---------------------------------------------------------------------------
G2 graphics   GfxSeedDepth                            occluders (T3)
G2 compute    GfxHiz x12                              pyramid over SEED depth (T2)
G2 compute    GfxFilter                               GPU cluster cull (T1/T4)
G2 graphics   ForwardPlusDepth                        fallback-only prepass
G2 graphics   GfxRemainderDepth                       newly visible (T5, when pending)
G2 compute    ForwardPlusCull / Ds phases / particles tile lists + light cull
G2 graphics   MotionVectors, Ssao(+Blur/Compose), Main, ParticlesDraw,
              MotionBlur, Bloom chain, Tonemap, SMAA, ImGui, UiComposite
```

The rule the split implements (from the Alan Wake 2 / Northlight design):
**visibility can be temporal; occluder coverage cannot be stale.** The
previous frame decides what to rasterize *first* (the occluder set), the
current frame's own depth decides what gets culled:

1. **GfxSeedDepth** — replays the *prev∩curr* cluster set (same cluster id
   + bit-identical model, so the depth is bit-identical to what the same
   geometry would rasterize this frame). LOD flips and moved instances are
   excluded by construction (sibling cluster ids are disjoint / model
   differs), so stale coverage never enters the pyramid. Opaque clusters
   draw VS-only; MASK clusters draw the real alpha test with material cull.
   Clears renderDepth (reversed-Z, clear 0 = far).
2. **GfxHiz** — builds the MIN-reduced pyramid over the seed-only depth.
   Because the pyramid does *not* contain the rest of the frame's geometry,
   the occlusion test can actually cull. (Over the full prepass depth the
   test is a structural no-op: every CPU-visible cluster's own depth is
   inside its footprint texels and the MIN-reduce keeps through it — the
   reason T4 occlusion looked permanently silent before the split.)
   Option A (`VULKANTOY_HIZ` occluder depth on): a separate
   `ForwardPlusDepth`-labelled occluder prepass fills hizDepth with big
   solid opaque occluders at their *selected* LOD (occluder coverage ==
   the rendered surface; the old crudest-LOD pick relied on "simplifier
   only shrinks", which meshopt does not guarantee) and the pyramid reads
   that instead.
3. **GfxFilter** — one workgroup per surviving (draw × cluster) batch:
   degenerate / backface / frustum / small-prim geometric tests plus the
   live HZB occlusion test (and contribution, off by default). Writes
   per-batch `VkDrawIndexedIndirectCommand`s (indexCount = 0 = culled) and
   the compacted per-batch triangle lists. MASK draws can be excluded from
   the occlusion test via `CullFlagBit_HzbSkipMasked` (default ON, UI
   checkbox "Exclude MASK from HZB test").
4. **ForwardPlusDepth (fallback-only)** — the direct prepass walk, but
   skipping every item whose (mesh, selected LOD) the filter owns
   (`fc.gfxLodFilterable`): it only covers unregistered meshes/LOD levels,
   material-less meshes and shader-LOD vertex-lit instances. Loads the
   seeded depth. Masked items are NOT exempted from the skip — their depth
   comes from the seed/remainder pair, and re-walking the full masked set
   here would double the alpha-tested raster the split exists to avoid.
5. **GfxRemainderDepth** (appended when `RemainderPending()`) — the
   *curr−prev* cluster set: disocclusions, LOD flips, frustum entries.
   Both buckets; MASK clusters run the real alpha test with material cull.
   This is the second half of the standard two-pass HZB technique (the
   newly-visible set the seed-only pyramid could not know about). It
   ignores the GPU cull verdicts — acceptable because that set is exactly
   the disocclusion candidates the seed-only HZB cannot prove occluded.
6. Everything downstream (tile cull, deferred-shadow phases, SSAO/GTAO,
   motion, Main) runs over the completed depth. Main draws the
   filter-kept batches through the per-material indirect ranges (culled
   batches are zero-indexCount commands) plus the direct-fallback legs,
   read-only tested in 1x.

### Depth ownership (each filterable cluster exactly once)

| Set | Who depths it | Machinery |
| --- | --- | --- |
| prev∩curr (unchanged instances) | GfxSeedDepth | CPU commands, cluster-ordered indices |
| curr−prev (newly visible) | GfxRemainderDepth | CPU commands, cluster-ordered indices |
| unregistered meshes/LODs, material-less, shader-LOD vertex-lit | ForwardPlusDepth (fallback) | direct walk, push-constant models |

**No indirect alpha-tested depth draws.** Drawing the MASK ranges through
the filter's GPU-written indirect commands (any of seed VS / filtered VS
pairings) misrasterizes depth — surfaced as masked foliage rendering
opaque-white (sky through the holes) and bisected down to that one
combination: opaque indirect draws rasterize correctly, the same masked
ranges via CPU commands rasterize correctly, only *masked + the filter's
GPU commands + a depth write* poisons Main's 1x read-only test. The
seed/remainder pair covers masked depth with the proven CPU-command
machinery, one draw per cluster, so the split never issues masked indirect
depth draws.

### Masked depth phases follow the material raster

Every depth/shadow/motion phase that draws MASK cutout (Forward+ prepass,
seed, remainder, shadow-atlas seed ranges, direct shadow casters, EVSM
moments, motion vectors) resolves the material's `pipeline.raster.cullMode`
instead of a blanket Cull None. Single-sided cutout no longer writes
backface depth the shaded pass never draws; double-sided materials keep
Cull None everywhere. Resolver: `GfxRenderer::ConfigureMaterialDepth`
(wired in SceneRenderer's init), consumed by the range-based recorders.

## Legacy assembly (fallback)

```
GfxSeedDepth (if history valid)          clears + seeds depth
ForwardPlusDepth (FULL prepass)          every CPU-visible opaque+masked item
GfxHiz                                   pyramid over the FULL depth
GfxFilter                                geometric cull; occlusion is a no-op
GfxRemainderDepth (when pending)         newly visible clusters
```

Runs when the filter is off, or on the first frame / after history
invalidation (no seed → the prepass clears). With a full-depth pyramid the
occlusion test cannot cull (self-coverage keeps every cluster), which is
correct but makes T4 dead weight — that is the perf motivation for the
split.

## LOD selection (feeds every consumer)

- Error-driven: coarsest cooked sibling whose world simplifier error
  projects under the pixel budget (`World::SelectLodIndex`), shared by the
  filter push, prepass, main pass and shadows per node per frame.
- **Hysteretic**: a node keeps its level while the projected error stays
  inside `kLodSelectHysteresis` (±20% band) around the budget — switch
  distances no longer flip siblings frame to frame.
- **Full-rate by default**: every node re-evaluates every frame.
  `VULKANTOY_LODFULL=0` restores the halfrate parity split (decisions up
  to 1 frame stale) as a perf A/B. `VULKANTOY_LOD=0` forces LOD0.

## Debug knobs

| Knob | Effect |
| --- | --- |
| `VULKANTOY_SPLIT=0` | force legacy depth assembly (A/B) |
| preset `x64-Release-ShaderDebug` | Release C++ with shaders compiled `-O3 -Zi -fspv-debug=vulkan` (SPIR-V debug info for RenderDoc & friends; `VULKANTOY_SHADER_DEBUG=ON`, DXC forbids `-Zi`+`-Zs` together so `-Zi` it is) |
| `VULKANTOY_CULLFLAGS=hex` | override the GPU filter cull flags (bits mirror `smvk::gfx::CullFlagBits`; 0x24b = default incl. MASK exclusion) |
| `VULKANTOY_AUDIT=<frame>` | one-shot `[GfxAudit]` indirect-verdict dump to stderr (per-batch kill reasons + kept/killed summary; stalls the GPU) |
| `VULKANTOY_HIZ=0` | kill the Option A occluder-depth source |
| `VULKANTOY_BARRIER_TRACE=on` | per-frame pass order + every barrier decision |
| `VULKANTOY_CAPTURE=<frame>` | PNG export to `exports/` (headless A/B) |
| `VULKANTOY_LODFULL=0` / `VULKANTOY_LOD=0` / `VULKANTOY_SHADERLOD=0` | LOD cadence / LOD kill / shader-LOD kill |
| UI: culling panel | HZB occlusion (T4), Exclude MASK from HZB test, occluder-depth mode + thresholds |
| UI: HZB status | live pyramid state; `src=seed` marks the split active |

File anchors: `GfxRenderer::AppendPasses` / `FilterMainView`
(app/src/renderer.cpp), the `depthSplit` block (app/src/scene_renderer.cpp),
`ForwardPlusRenderer::AppendPrepass(fallbackOnly)` (app/src/forwardplus_renderer.cpp),
the engine gfx state machine (engine/src/Renderer/gfx_filter.cpp),
cull flags (engine/include/Engine/Renderer/gfx_filter.h).
