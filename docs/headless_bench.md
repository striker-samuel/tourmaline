# Headless running + bench/capture reference

The full A/B-pin and debug-view reference. All `TOURMALINE_*` knobs are
declared, parsed and WarnUnknown-covered in `engine/App/RunConfig.h/.cpp`
(one knob, one place).

- `App.exe` runs from the build directory (`out/build/x64-Release`),
  loading `content/paks/*.pak` and `content/shaders/*.spv` relative to
  CWD. Redirect `*.log` output to `out/logs/` (root is for sources, not
  logs).

## The bench/capture contract

Headless-ish deterministic capture (auto-exits after the bench budget):

```powershell
$env:TOURMALINE_SSR='1'; $env:TOURMALINE_AA='off'; $env:TOURMALINE_AUTO_EXPOSURE='0'
$env:TOURMALINE_CAM_POS='24.26,1.82,-9.52'; $env:TOURMALINE_CAM_FWD='-0.947,-0.022,-0.321'
$env:TOURMALINE_CAPTURE='48'
& .\App.exe --pak content/paks/bistro.pak --disable_control --bench 64
```

- `--bench <n>` runs 10 warmup + n measured frames, then exits (vsync
  forced off — uncapped immediate present; an explicit
  `--present <mode>` wins).
- `TOURMALINE_CAPTURE=<frame>` exports `exports/screenshot_*.png` — the
  frame number must land INSIDE the bench budget (capture < bench;
  e.g. capture 12 with bench 32), otherwise the auto-exit happens
  before the export.
- Combined with `TOURMALINE_SSR`, `TOURMALINE_EVSM`,
  `TOURMALINE_TRANSPARENCY` the pins below span the full A/B matrix.

## Quality A/B pins

- `TOURMALINE_AO=off|ssao|sao` pins the AO mode headless (default sao;
  the O key / UI combo stay live).
- `TOURMALINE_SSR_DUMP=<frame>` dumps SSR stage stats + PNGs to `exports/`.
- `TOURMALINE_GI_INTENSITY=<0..2>` pins the GI intensity slider headless
  (scales the baked-GI ambient and the Sky IBL fallback; 0 leaves
  direct sun only, 1 is authored, 2 doubles it).
- `TOURMALINE_NORMAL_FIX=0|1` pins the JHFAA-style shading-normal
  validity correction headless (experimental, default off — the
  silhouette reflection-vector constraint; docs/jhfaa_normal_fix.md);
  `TOURMALINE_NORMAL_FIX_DEBUG=1` is the red/orange violation overlay
  (red = back-facing shading normal, orange = reflection below the
  geometric horizon — works with the correction on or off).
- `TOURMALINE_VOLUMETRIC=0|1` forces the raymarched fog on/off headless
  (scene fog settings lose); `TOURMALINE_VOLUMETRIC_STEPS=<4..128>`
  pins the march steps for heavy fog benches; `TOURMALINE_VOLUMETRIC_DENSITY=<0..2>`
  scales the extinction density (heavy densities isolate the scatter
  sources from the lit base for A/B); `TOURMALINE_VOLUMETRIC_SUN=<0..4>`
  and `TOURMALINE_VOLUMETRIC_GI=<0..4>` scale the sun / GI in-scatter
  contributions; `TOURMALINE_VOLUMETRIC_GI_VOLUMES=0`
  is the fog-GI quality bypass (NO convex-volume walks — the
  world-field/sky fog-GI base still samples; only the volume composite
  is skipped — pairs with `TOURMALINE_VOLUMETRIC` for volume-walk
  cost A/B); `TOURMALINE_FOG_GI_DEBUG=2` is the transmittance view
  (throughput as grayscale — white = no extinction, black = complete).
- `TOURMALINE_PROBE_PREMIX=0|1` pins the Remember Me probe pre-mix
  (default on): `1` = the per-frame GPU weights + mix passes (a blended
  cubemap + a GPU-owned selection; the material does ONE fetch +
  dominant-box parallax), `0` = the per-pixel walk fallback (≤8
  fetches + per-pixel box parallax). `0.00%` differing pixels is the
  A/B contract between the two.
- `TOURMALINE_FP_MASKS=0|1` pins the Forward+ HIERARCHICAL TILE-LIGHT
  CULLING (FFXVI "Shadow Techniques" 2.1–2.3; default on): `1` = the
  forward+ tile cull consumes the phase-1 artifacts — lights
  CPU-sorted by near distance + zBins + 64×64px coarse tile masks
  uploaded per flight; the cull intersects coarse mask × zBin window
  at the 32-bit word level and runs the exact side-plane/slab tests
  only on surviving bits; `0` = the linear scan fallback (the A/B
  reference). NO-MISSING-CONTRIBUTOR is the A/B contract (the paper's
  invariant). The fog track uses its own zBin window. Probes ride a
  linear tail. Under deferred shadows the F+ cull keeps the linear
  scan (the DS ring owns the artifacts; the F+ light leg is dead work
  there). Invariant unit-tested in test_forwardplus_masks.cpp.
- `TOURMALINE_HZB_SPD=0|1` A/Bs the SSGI MAX-HZB build (default 0 = the
  per-mip dispatches; `1` = the single-dispatch SPD build — same BDA
  buffer, byte-identical output). `TOURMALINE_HZB_SPD_VERIFY=1` builds
  BOTH from the same depth and byte-compares every mip post-Execute
  (stalls; the validation harness — 0 mismatching texels is the
  contract). `TOURMALINE_HIZ_SPD=0|1` / `TOURMALINE_HIZ_SPD_VERIFY=1`
  are the same A/B + verify pair for the gfx filter's culling MIN-HZB
  (the verify compare reports as [GfxHizSpdVerify]). Extents > 4096
  always fall back to the per-mip path.

## Fog sun-shadow lane (`TOURMALINE_FOG_CSM`)

The raymarched fog's sun-occlusion feed has three modes:

- `TOURMALINE_FOG_CSM=0|off` — fully-lit fog (no sun occlusion).
- `TOURMALINE_FOG_CSM=1|on|csm` — the raw MAIN cascade atlas: ONE
  single-tap depth compare per march step. Two hard rules it rides
  (see the AddVolumetricPass comment): never `light.shadow_map_index`
  (that flips to the EVSM moments) and PCF mode only — under EVSM the
  caster passes write moments, not raw depth, so mode 1 stays off.
- `TOURMALINE_FOG_CSM=2|coarse` — the DEDICATED coarse atlas: a
  one-raster pass renders ONLY the CastVolumetricShadow-marked large
  occluders into its own D32 atlas, then ONE compute SPD dispatch
  (`volumetric_shadow_spd.cs.hlsl`) builds the full R32F MIN mip
  pyramid; the march samples a distance-derived discrete mip with
  unfiltered texel loads (nearest blocker — neither bilinear nor
  trilinear preserves it). Works under PCF AND EVSM alike. When no
  marked casters exist the leg falls back (mode 2 → fully lit) rather
  than sampling a stale tile.

The headless A/B rig (no authored content needed):

```powershell
$env:TOURMALINE_VOLUMETRIC='1'; $env:TOURMALINE_VOLUMETRIC_DENSITY='0.6'
$env:TOURMALINE_FOG_CSM='2'; $env:TOURMALINE_VOLUMETRIC_SUN='1'
$env:TOURMALINE_FOG_SHADOW_MARK_ALL='1'                     # TEST RIG: every opaque surface becomes a marked caster
& .\App.exe --pak content/paks/sponza.pak --disable_control --bench 64
```

- `TOURMALINE_FOG_SHADOW_MARK_ALL=1` marks EVERY opaque surface as a
  volumetric caster (parsed once at the cook, world.cpp) so the tile +
  pyramid machinery is exercisable without authored content; the
  authoring surface is the editor's per-node checkbox.

## Sun contact shadows

- `TOURMALINE_CONTACT_SHADOWS=0|1` pins the sun contact shadows on/off
  headless (contact_shadow.h — the pre-Main depth raymarch toward the
  sun, min'd with the CSM/EVSM factor in the lit body; the panel
  checkbox stays live). `TOURMALINE_CONTACT_SHADOWS_STEPS=<4..32>` pins
  the march samples; `_DIST=<0.05..2>` the march length (m);
  `_THICKNESS=<0.005..0.2>` the acceptance band (m); `_RES=1|2` the
  visibility resolution (1 full, 2 half — the A/B to benchmark, half
  saves tracing but can lose thin occluders). Defaults: 0.35 m march /
  0.03 m thickness / 0.01 m bias, 12 steps, half-res.
- `TOURMALINE_CONTACT_SHADOWS_DUMP=<frame>` one-shot dumps the
  visibility texture's stats + an ASCII occlusion map to stderr
  (sync download, debug runs only).
- `TOURMALINE_DEBUGVIEW=contact` selects the contribution view (the
  Debug-view combo's "Sun contact shadows (contribution)"): R = the
  contact march's visibility, G = the CSM/EVSM factor — green = the
  march's contribution, red = the march missed what the cascade
  caught, dim blue = NdotL<=0 (inert), magenta = the pass not running.
  The contact pass stays LIVE under this view.

## Debug views (`TOURMALINE_DEBUGVIEW=`)

- SSGI compose views: `ssgi_confidence|ssgi_raw|ssgi_layer|
  ssgi_baseline|ssgi_contribution` (the chain must be on —
  `TOURMALINE_SSGI=1`). The four HDR-scale views write real radiance
  through the frozen scene exposure, so they carry a DIAGNOSTIC +EV
  GAIN — the Debug panel's "SSGI debug exposure (+EV)" slider, default
  +4 (x16), headless pin `TOURMALINE_SSGI_DEBUG_EV=<0..12>` (shared
  across the four so switching preserves relative brightness; the
  confidence view self-normalizes via 1/exposure and ignores it).
  Background pixels read BLACK under every SSGI view (no diagnostic
  signal exists in the sky).
- PBR Material Validation Debugger: `pbrhealth|pbrbase|pbrdiffuse|
  pbrrough|pbrmetal|pbrf0` — the six validation/readout FSs over the
  SHARED material evaluator (`engine/shaders/material_evaluation.hlsli`
  — the lit body's exact material-input math, raw pre-clamp values
  retained). Material Health = the combined heatmap (green passes the
  configured checks, yellow suspicious — albedo luma/dielectric edge/
  intermediate metallic/roughness below the renderer's 0.04 floor; red
  out-of-range/NaN input, magenta non-finite in the value views);
  pbrbase/pbrdiffuse/pbrrough/pbrmetal/pbrf0 are flat value readouts
  (the 1/exposure debug pre-scale means intensities are display-only).
  `sss|hair|water` are NAIVE shading-model previews (development test
  beds — d'Eon transmission SSS, Kajiya-Kay hair, Fresnel+wave water;
  sun/sky shaded like the POM view, deliberately first-cut, owner
  develops further).
- Image clipping: `clipping|clipdark|clipbright` — IMAGE-LEVEL
  diagnostics in the tonemap pass (the FINAL mapped+graded image is
  annotated after exposure/bloom/tonemap/LUT; BLUE = display-encoded
  SDR luma at/below the shadow threshold, RED = at/above the highlight
  threshold, in-range pixels keep their normal color). A NEAR-CLIPPING
  warning, not proof of irreversible loss. The post-fx chain +
  exposure metering stay LIVE under these views (the
  DebugVisualizationActive/ExposureMeteringActive exclusions) —
  otherwise the view would inspect a different image than the player
  sees. Thresholds: the Debug panel sliders (shadow 0..0.1, highlight
  0.9..1.0, defaults 0.01/0.99) or headless `TOURMALINE_CLIP_DARK=<0..0.1>`
  / `TOURMALINE_CLIP_BRIGHT=<0.9..1>`.

## Per-material gates

- MATERIAL_RECEIVE_CONTACT_SHADOWS (the MaterialValues::emissiveFactor.w
  flags word, mesh.h kMaterialFlag*): the SMVK_materials_contactShadows
  glTF extension `{"receive": false}` opts out at import; the editor
  material panel's Contact checkbox + the setMatAttr contactShadows
  tour attr at runtime. OPT-IN bit — paks cooked before the flag read
  w=0 and receive NOTHING (recook).

## GI internals (probe spheres, bake layout, formats)

- `TOURMALINE_GI_POINTS=1` arms the GI PROBE SPHERES (the Debug-panel
  "Probe points" checkbox's headless twin; Rendergraph/giprobe.h): every
  convex-GI-volume probe renders as an instanced 0.12 m icosphere
  (one DrawIndexed instanced call per volume over a static per-volume
  instance table) whose VERTEX shader Loads the probe's EXACT texel
  from the volume's 7 SG layers (`Texture3D.Load`, never the filtered
  Sample) and evaluates SgGiDiffuseIrradiance against the sphere
  direction — each sphere is a tiny irradiance ball showing the baked
  field's directional answer at that probe (FS = pure HDR
  pass-through). Slot contract: HDR tail (bloom/tonemap grade the
  spheres), scene depth TEST-ONLY (occluded probes hide behind
  geometry). Gates on the same condition the FrameUniforms
  giVolumeLayers slots publish under.
- Probe-sphere COLOR MODES (the Debug panel's "Probe color mode" combo
  or `TOURMALINE_GI_PROBE_MODE=0|1|2`): 0 = baked GI (the
  white-Lambertian answer — physical radiance, never exposure-scaled),
  1 = VISIBILITY (the probe's min 6-axis obstacle distance from the
  vis layers, exact texel — RED < 0.05 m blocked/inside, YELLOW < 1 m
  near-surface, GREEN open, flat gray = no vis data), 2 = the PRE-BAKE
  LATTICE (a flat marker color, NO bake data read — the AUTHORED
  volume defs' would-be grids, cook-parity via BuildGiProbeLatticeRows;
  rebuilt every armed frame so live editor moves show). The marker
  modes' colors are display-authored and pre-scaled by 1/exposure. The
  EDITOR's LIGHTING 'J' preview cycle has a 5th stop ("lattice
  (pre-bake)") rendering the same 3D spheres per viewport
  (EditorApp::RecordGiProbeLatticePass). BuildGiProbeLatticeRows is
  unit-tested in test_gi.cpp (bake-parity dims/positions + cap/
  degenerate edges).
- GI HOT-SWAP (the live-authoring milestone): LoadGiVolumes split into
  the transactional `BuildGiVolumeSet` + `ReloadGiVolumesHotSwap` (the
  old generation's Holders retire through the deferred-destroy queue
  behind the in-flight frames — nothing a recording or executing frame
  references is destroyed under it). The editor's bake-completion edge
  calls the hot swap instead of the WYSIWYG relaunch. Headless
  frame-safety regression: `TOURMALINE_GI_HOTSWAP=<frame>` swaps the
  generation MID-BENCH.
- AUTOMATIC GI PROBE-GRID LAYOUT OPTIMIZATION
  (cubemap_volume.h/.cpp ChooseGiVolumeGridLayout): a BVH-only
  candidate search (NO path tracing) over spacings × lattice phases,
  scored on invalid probes (clearance + a ray-parity SOLID-INTERIOR
  test), visibility-aware surface coverage (area-weighted; covered 10x
  heavier than invalid per the contract), and probe count/memory.
  Candidates exceeding the cap are REJECTED, never silently truncated.
  Authored on the volume (SceneGiVolume + the editor's inspector,
  persisted as setGiVolumeAttr tour verbs): `spacingLow`/`spacingHigh`
  DENSITY GATES bound the bake-time search (0 = off), and the
  `optlayout` SEED (one verb: number = spacing + vec4.xyz = phase) is
  the editor's "Optimize grid layout" button result — committed as ONE
  undoable step; consumed seed > gates > the LEGACY fixed layout (so
  every pre-optimization volume bakes bit-exact). The button runs on
  the LIVE editor world (NO tour snapshot, NO replay: the bake-input
  hash folds the gates + seed; BuildTraceSceneFromWorld = the SAME
  shared assembly as the GiBaker's cluster path, pinned by
  `trace.world.*`). Unit-tested in test_gi.cpp (`glayout.*`).
- BC6H IS THE CANONICAL GI-volume cooked format ("GV6H" — no flag, no
  toggle, a HARD ENGINE REQUIREMENT): per-lobe 3D textures compressed
  at cook time only (~9 bytes/probe vs RGBA16F's 56). The device init
  REFUSES to run without BC6H_UFloat 3D sampled + linear filtering (no
  fallback by design). The reader takes GV6H (blocks verbatim + the
  lossy CPU decode mirror for CPU consumers) AND the legacy GVOL (old
  content rebakes; also the RGBA16F reference form for quality
  comparisons). The shader branches per volume. Round-trip unit-tested
  in test_gi.cpp (`gv6h.*`).

## Geometry LOD streaming

- `TOURMALINE_GEOMETRY_STREAMING=1` gates GPU geometry LOD streaming
  (geometry_streamer.h — DEFAULT OFF): cluster load defers the middle
  LOD siblings (LOD0 bases + each group's coarsest fallback stay
  resident; skinned siblings never defer), the LOD picker selects over
  the RESIDENT subset only (a missing middle LOD degrades to the
  resident neighbor), and the per-frame tick promotes demand-missed
  siblings (throttled: 16 MiB + 4 promotes/frame) and EVICTS the idlest
  over-budget siblings back to deferred (nothing drawn or demanded
  within the idle window) — every rebuild between frames is
  in-flight-safe. `TOURMALINE_GEOMETRY_BUDGET_MB` pins the promoted
  working-set ceiling, `TOURMALINE_GEOMETRY_EVICT_IDLE` the idle window
  in frames (small budgets force churn-cycling in A/B). Eviction frees
  arena RANGES for reuse, the monoblocks keep their committed size.
  Stage 6 (disk-range streaming) stays the noted TODO in
  geometry_streamer.h.
