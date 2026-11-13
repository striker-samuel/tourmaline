# Tourmaline — GI Volume System: Hardcoded Tolerances & System Properties

This is the reference for every **hardcoded tolerance, threshold, and
system property** in the convex GI volume pipeline that cannot be
tuned at runtime or from data. Changing any of these requires editing
the source + rebuilding — they are **properties of the system**, not
user-facing knobs. Each entry documents: the value, where it lives,
what it controls, and why it has that value.

---

## 1. The 6-axis visibility model (the through-wall rejection)

### 1a. The HYBRID REACH — `max(dist * 1.5, 0.5)`

**File:** `engine/shaders/sg_gi_volume.hlsli` (function `SgGiVolumeVisAxis`)
**CPU twin:** `engine/src/GI/cubemap_volume.cpp` (inside `GiVolume::Sample`)

```
reach = max(dist * 1.5, 0.5)     // spacing units
```

What it controls: how far a probe's stored obstacle distance allows it
to contribute to a sample position, per-axis.

- `dist * 1.5`: the proportional component — a probe 1 m from a wall
  reaches 1.5 m (full isolation); a probe at the 2.5 m cap reaches
  3.75 m (full open-space coverage).
- `0.5` floor: the coverage component — near-surface probes always
  reach at least 0.5 spacing units (0.25 m at 0.5 m spacing). Without
  this, wall-adjacent pixels lose their nearest probes and render as
  hard black shadows.

**Why 0.5:** at 0.5 m probe spacing, a wall pixel is typically
0.2–0.3 m from the nearest valid probe. The 0.25 m reach gives those
samples FULL contribution inside 0.25 m; samples between 0.25 m and
0.4 m ride the rejection ramp (section 1b — 0.15 m at near-wall
probes) with progressively attenuated contribution rather than a hard
cutoff. Lower (0.3) leaves black bands past the ramp; higher (0.7)
lets through-wall probes dilute the blend too broadly.

**Known limitation:** the floor spans thin walls (~0.2–0.3 m).
A probe on one side of a thin wall reaches ~50% to the other side
(the ramp midpoint). This is inherent to a uniform dense grid —
the octree solved it by relocating probes away from surfaces.

### 1b. The rejection RAMP width — `max(0.5 * dist, 0.45)`

**File:** same as 1a.

```
ramp_width = max(0.5 * dist, 0.45)  // spacing units
```

What it controls: how gradually the visibility factor transitions
from 1.0 to 0.0 past the reach boundary.

- Proportional (0.5 × dist): open-space probes ramp smoothly.
- Floor (0.45): near-surface probes ramp over at least 0.45
  spacing units (0.225 m at 0.5 m spacing). Narrower ramps can
  produce lattice-aligned dark cliffs when nearby corner weights
  differ sharply.

**Tradeoff:** the wider floor softens dark-square grid artifacts, but
also extends partially-visible weights across thin walls. Validate
thin-wall leakage before keeping it as the permanent default.

### 1c. The visibility distance CAP — `2.5 m`

**File:** `engine/include/Engine/GI/cubemap_volume.h` (`GiVolumeBakeConfig::visibilityMaxDistance`)

The 6-axis raycast cap. A miss (no obstacle within 2.5 m) records
the cap = "fully open." The cap must exceed any in-cell tap offset
(probe spacing × √3 ≈ 0.87 m at 0.5 m spacing) so that "capped"
reads as fully open to the runtime factor.

**Why 2.5:** the octree parity value. Comfortably exceeds any
in-cell offset; low enough that the unorm8 quantization (over
[0, 15] spacing units) has ~3 cm granularity at 0.5 m spacing
(15/255 × 0.5 m ≈ 0.029 m).

### 1d. The VISIBILITY QUANTIZATION — `[0, 15]` spacing units, unorm8

**File:** `engine/src/GI/cubemap_volume.cpp` (`PackGpuVisLayers`)
**Shader decode:** `engine/shaders/sg_gi_volume.hlsli` (`kFull = 15.0`)

Each per-voxel 6-axis distance is stored as unorm8 over [0, 15]
spacing units. At 0.5 m spacing: 15 units = 7.5 m > the 2.5 m cap
(so "capped" is well within the range), and the granularity is
15/255 × 0.5 m ≈ 0.029 m (3 cm — well below the ~0.5 m rejection
precision).

---

## 2. The dense probe grid (the bake)

### 2a. The probe VALIDITY clearance — `0.05 m`

**File:** `engine/include/Engine/GI/cubemap_volume.h` (`GiVolumeBakeConfig::probeMinClearance`)

A probe center closer than 0.05 m to any geometry surface is
INVALID (no direct trace). Its coefficients are filled by the
same-side dilation, and its visibility distances are all-zero
(blocked). At runtime, blocked probes have the minimum reach
(0.5 units from the hybrid floor).

**Why 0.05:** thin enough that the grid resolves surfaces at
typical spacing (0.25–1.0 m); thick enough that the BVH
ClosestDistance query finds the nearby surface reliably.

### 2b. The same-side DILATION gate — `visOpen(u, k^1) >= spacing - 0.35 * spacing`

**File:** `engine/src/GI/cubemap_volume.cpp` (function `sameSide`)

A valid neighbor `u` can fill an invalid target `v` only if `u`'s
obstacle distance toward `v` is at least `spacing - 0.35 * spacing`
(0.65 × spacing). This ensures the source is on the same side of
any wall between them.

**Why 0.35 × spacing:** paired with the runtime factor's slack
design — the same proportional band. At 0.5 m probe spacing, the gate
is 0.325 m of unobstructed VISIBILITY: the valid source probe must
see at least 0.325 m along the axis toward its adjacent target, which
is one full 0.5 m spacing away. The gate is on the SOURCE's obstacle
distance (section 2b), not on the pair's separation — adjacent probes
are always 0.5 m apart, so a "within 0.325 m of each other" reading
would make the gate unsatisfiable by construction.

### 2c. The Laplacian SMOOTHING — 3 passes, lambda 0.4

**File:** `engine/include/Engine/GI/cubemap_volume.h` (`GiVolumeBakeConfig::smoothingPasses`, `smoothingLambda`)

Post-bake value smoothing over same-side neighbor pairs. 3 passes
at lambda 0.4 (40% toward the neighbor average per pass). The
purpose: soften the transition between directly-traced probes and
dilated (filled) probes, and smooth the per-probe Monte Carlo
noise.

### 2d. The CLEAN-CELL threshold — `0.75` spacing units

**File:** `engine/src/GI/cubemap_volume.cpp` (the clean-flag pass)

A cell [v, v+1]³ is "clean" (fast path: plain trilerp, no
rejection) only if all 8 corners have all 6 axis distances
≥ 0.75 spacing units. The hybrid-reach factor uses
`reach = max(dist * 1.5, 0.5)`, so the theoretical no-rejection
threshold is **~0.667** (maximum axis offset 1.0 / reach factor 1.5).

**Why 0.75:** a modest margin above the theoretical threshold also
accounts for the runtime's UNORM8 visibility quantization. Cells
previously in the [0.75, 1.35) range can use the hardware-trilinear
fast path without changing their visibility weights.

### 2e. The FIREFLY CLAMP — `8×` the probe's finite-sample mean

**File:** `engine/src/GI/cubemap_volume.cpp` (inside `BakeGiVolume`)

Each probe's radiance samples are clamped to 8× the probe's own
finite-sample mean luminance. This is the standard firefly filter
(sun-pool outliers tamed per-probe, not globally).

### 2f. The SG directional-mean correction

**Files:** `engine/src/GI/cubemap_volume.cpp` (after `SgFitAccumulator::Solve`),
`engine/src/GI/sg.cpp` (`SgMatchMeanRadiance`)

The nine SG basis lobes overlap. The production diagonal fit is robust
against high-contrast rays, but it counts that overlap more than once:
adding more rays can converge to an overly bright result rather than the
correct energy. After the per-probe firefly clamp and SG fit, each RGB
channel's lobes are scaled so their *analytic spherical mean* matches
that probe's **post-clamp** sample mean. This corrects the deterministic
amplitude bias without spatially blurring walls or raising grid density.
It preserves the directional shape of the fit, but cannot recover energy
already removed by firefly clamping; inspect `clampedEnergyRatio` when
comparing bakes. The correction is applied to dense GI-volume probes,
not to the legacy regular-volume or lightmap fitting paths.

### 2g. The probe-count CAP — `256` per-axis

**File:** `engine/include/Engine/GI/cubemap_volume.h` (`kGiVolumeMaxDim`)

The maximum per-axis grid dimension. A 256³ volume contains
16.8 million probes, requiring approximately **1.69 GiB** of
FP32 coefficients (27 fp32 per probe = 9 SG lobes × RGB vec3
× 4 bytes), or **2.06 GiB** including 6 FP32 visibility
distances per probe — excluding clean flags, metadata, and
temporary bake allocations. Real volumes are far smaller
(sponza's whole-building is 62×28×37 = 64k probes = ~8.5 MB).

---

## 3. The file format (no version scheme)

### 3a. NO VERSION FIELD

**Policy:** the magic is the identity. No version members, no
conversions. Files refuse at any other magic — re-bake (the standing
`GVOL`→`GV6H` break is the working example: GV6H is the ONLY emitted
format; the legacy GVOL form is still read on intake for old content
to re-bake from, exactly because no conversion path exists).

> STATUS (as of the GV6H landing): the canonical `.givolume` format is
> **GV6H** (BC6H_UFloat, ~9 bytes/probe) — packaged as `GV6H`; the
> `GVOL` row below is the LEGACY reference form (read-only intake for
> pre-GV6H content + the quality-comparison baseline), so the "no
> legacy readers" note originally described a policy against FORMAT
> CONVERSION, not against accepting old files verbatim for re-bake.

| Format | Magic | Notes |
|--------|-------|-------|
| `.givolume` | `GV6H` | the post-visibility shape — BC6H_UFloat per-lobe 9-layer (`GVOL` is the legacy RGBA16F form, still ACCEPTED on read as the reference/rebake input, never emitted) |
| `.sgrv` | `RGVL` | the regular SG lattice |
| `.sglm` | `SGLA` | the SG lightmap atlas |
| `.reach` | `REAX` | the capture-reach sidecar |

### 3b. The on-disk VISIBILITY payload — 6 fp32 distances + clean flags per voxel

The .givolume file stores per-voxel: 6 fp32 obstacle distances
(meters, capped at `visibilityMaxDistance`; 0 = blocked) + 1 byte
clean-cell flag (4-aligned tail). These are the FULL-PRECISION
bake outputs. The GPU representation (2 RGBA8_UNORM textures
created at load time by `PackGpuVisLayers`) is a QUANTIZED
DERIVATIVE — not what the file stores.

---

## 4. The runtime composite (the volume blending)

### 4a. The volume PRIORITY — upload order (low first)

**File:** `engine/src/Renderer/WorldRenderer.cpp` (`LoadGiVolumes`)

Volumes are stable-sorted by priority (LOW first in upload order).
The shader walks HIGH first (reverse index). Ties keep the manifest
order. A volume with weight 1 (deep inside every fade) zeroes the
transmittance and the walk breaks — lower priorities never contribute.

### 4b. GI hull fades — retired (hard ownership)

**File:** `engine/include/Engine/GI/cubemap_volume.h` (`GiVolumePlane`)

Each hull plane carries an inside fade (full weight before the
surface) and an outside fade (weight beyond the surface). The
per-pixel weight is the min over planes of the smoothstep across
the fade band. Authored in the tour (`-t` packs `insideFade outsideFade`).

### 4c. The MAX VOLUME count — `8`

**File:** `engine/shaders/sg_lightmap_types.hlsli` (`SG_GI_VOLUME_MAX`)

The per-frame uniform array cap. The forward+ tile mask is a
uint32 bitmask (8 bits used of 32). The priority sort drops
excess volumes (pathological only).

---

## 5. The forward+ tile cull

### 5a. The TILE SIZE — `16` pixels

**File:** `engine/include/Engine/Renderer/WorldRenderer.h` (`giVolumeTileSize`)

The per-tile volume bitmask tile size. At 1920×1080: 120×68 tiles.
The sphere-vs-frustum broad-phase is conservative (a superset —
can only be too permissive, never too restrictive).

---

## 6. Tuning guidance

If you need to adjust any of these:

| Symptom | Likely knob | Direction |
|---------|-------------|-----------|
| Light leak through thin walls | 1a reach floor | lower (0.3–0.4) |
| Black shadows near walls | 1a reach floor | higher (0.6–0.8) |
| Splotchy / per-probe variation visible | 2c smoothing passes | more (4–5) |
| Grid-aligned dark squares | 1b ramp floor | higher than old 0.3 (default 0.45; check thin walls) |
| Too-dim overall | 2c smoothing lambda | lower (less averaging) |
| Wall seams (hard transitions) | 2b same-side gate | widen the slack component |

All of these are **compile-time constants** — edit the source, rebuild.
They are properties of the algorithm, not user-facing settings. The
runtime has no UI for them by design (changing them affects the
bake's output semantics, not just the display).


## 7. Visibility weight-sum debug view (2026-11-03)

Use `TOURMALINE_GI_DEBUG=4` or select **Visibility weight sum
(wSum)** in the GI debug combo (R/F8 cycle). For the first
highest-priority A-set volume covering a surface pixel, the shader
shows the *pre-normalization* sum of visibility-gated trilinear corner
weights. Red = 0 (no contributing taps), yellow = 0.5, green = 1
(clean cell / no visibility rejection). Magenta means no GI volume
covers the pixel. The sampler's all-blocked black fallback uses
`wSum <= 1e-4`, so red regions in this view are the primary places
to inspect. The view uses the same 6-axis rejection rule as the
surface sampler, but does not visualize final lighting or AO.

This repurposes the retired octree mode 4; the mode count stays five.
It is an A-set diagnostic (the same volume selection as probe-distance
mode 3), not a combined A/B blend visualizer.

## 8. Volumetric GI filtering (2026-11-02)

The fog's precomputed isotropic RGB mean is now sampled with the same
visibility rule as the surface SG coefficients. In clean cells it remains
one hardware-trilinear lookup; near walls it gathers eight mean texels,
uses the existing six-axis visibility factor for each corner, and
renormalizes the surviving weights. If all corners are blocked the result
is black, matching the surface GI fallback.

This applies to both GI blend sets A and B. The mean remains an RGBA16F
3D texture; no GI bake format or shader descriptor layout changes are
required. The extra reads are limited to cells marked non-clean by the
bake. Use `TOURMALINE_GI_VIS=0` and the volumetric GI A/B toggles to
compare through-wall fog leakage and cost in enclosed scenes.

---

*This file is the system reference. Update it whenever a hardcoded
tolerance changes. The values are load-bearing — each one was
chosen to resolve a specific artifact observed on real content
(sponza, bistro). Changing them without re-validating on those
scenes risks reintroducing the artifact they were tuned to fix.*
