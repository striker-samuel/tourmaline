# DDGI specular fallback + temporal stability — open work

Status: rough-specular fallback shipped (proxy); several follow-ups tracked below.

## Shipped

- **Camera-following cascades** (fine 1.5 m + coarse distance-fitted, toroidal
  scroll, SDSM-style slow-hysteresis far fit).
- **Incremental tracing** (`DDGI_TRACE_SUBSET = 4` rotating subset; carried
  probes copy history, scrolled-in probes re-trace).
- **Smooth fine→coarse cross-fade** (camera-distance band, `m_ddgiCrossFade`).
- **Rough-specular fallback** (this change): when DDGI owns the ambient and a
  pixel has no baked cubemap probe, its rough specular blends toward the
  occluded DDGI field sampled at `R` (`E(R)/pi`) instead of the unoccluded
  analytic sky. `EvaluateSpecularEnv` now reports an `out covered` flag.

## Open / concerning

### 1. Rough-specular proxy, not a cone trace
`E(R)/pi` is a *diffuse-field* proxy. It ignores the DDGI distance moments, so
rough metals reflect a blurred mean, not the geometry they actually see (no
parallax, no cone distance). Follow-up: proper DDGI specular — sample the
distance moments along `R`, parallax-correct the reflection to the mean hit,
roughness-widen the cone. Sharp lobes should stay on cubemap/SSR.

### 2. Roughness ramp tuning
Fallback currently ramps in over roughness `0.2..0.7` (linear). Needs a scene
pass to decide: too-sharp lobes get a wrong proxy; too-rough lobes lose the
benefit. Also decide metals-only vs all rough surfaces (a rough dielectric's
specular is tiny anyway — `F0=0.04`).

### 3. Temporal instability / "throbbing"
User-reported (cafeteria). Two candidate sources, not yet isolated:

- **SMAA T2x 2-frame toggle** (root-caused earlier, pre-existing): specular/
  silhouette pixels where the two jittered subsamples differ flash at the
  2-frame period (~0.04% of pixels; vanishes with AA off / SMAA 1x / MSAA).
  The neighborhood-pass velocity-sign bug is already fixed (`17c9c35e`), but
  the specular flash itself remains — a luminance-based history clamp in the
  resolve is the likely fix.
- **DDGI scroll pop-in** (moving camera only): a scrolled-in probe re-traces
  with hysteresis 0 and "pops" in at full fresh radiance, then settles over
  ~20 frames — reads as a throbbing trail ahead of the camera. Candidate fix:
  seed new probes from the field (bilinear of neighbors) instead of resetting,
  or amortize the first trace over a few frames.

Fixed-camera steady-state measurement: no slow throb (8/16-frame deltas
< 0.2% mean); the only fixed-camera artifact is the T2x toggle above. So the
"throbbing" is either the T2x flash or the moving-camera scroll pop-in.

### 4. Incremental ray rotation
A probe re-traced every 4 frames uses rays rotated by ~3 rad since its last
trace, so consecutive fresh results are nearly uncorrelated and the 5%
hysteresis blend adds a small per-trace step. Fixed-camera measured impact is
< 0.2%, but worth confirming on moving/emissive scenes.

### 5. Cascade defaults
Cross-fade knob shipped; fine/coarse dims (16x8x16) + coarse spacing clamp
(2x..4x fine) are still hardcoded and want a scene pass to settle defaults.

## Reproduce / measure

- Fixed-camera temporal sequence:
  `App.exe --pak content/paks/bistro_sdf_color.pak --disable_control --snapshot-sequence <dir> --snapshot-count N --snapshot-keyframe 1`
  with `TOURMALINE_DDGI=1 TOURMALINE_CAM_POS=... TOURMALINE_CAM_FWD=...`.
- Compare AA modes for the T2x toggle: `TOURMALINE_AA=smaat2x|smaa1x|msaa2x|off`.
- Moving-camera scroll pop-in needs an input tape / camera path (not yet set up
  for headless).
