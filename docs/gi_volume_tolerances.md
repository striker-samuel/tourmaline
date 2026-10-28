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
0.2–0.3 m from the nearest valid probe. A reach of 0.5 units
(0.25 m) covers this. Lower (0.3) leaves black bands; higher (0.7)
lets through-wall probes dilute the blend too broadly.

**Known limitation:** the floor spans thin walls (~0.2–0.3 m).
A probe on one side of a thin wall reaches ~50% to the other side
(the ramp midpoint). This is inherent to a uniform dense grid —
the octree solved it by relocating probes away from surfaces.

### 1b. The rejection RAMP width — `max(0.5 * dist, 0.3)`

**File:** same as 1a.

```
ramp_width = max(0.5 * dist, 0.3)   // spacing units
```

What it controls: how gradually the visibility factor transitions
from 1.0 to 0.0 past the reach boundary.

- Proportional (0.5 × dist): open-space probes ramp smoothly.
- Floor (0.3): near-surface probes always ramp over at least
  0.3 units (0.15 m) — without the floor, the ramp is near-instant
  and produces lattice-aligned cliffs (the dark-square grid).

**Why 0.3:** at 0.5 m spacing, 0.3 units = 0.15 m. Below this, the
rejection is effectively binary and the cell-boundary transition
is visible as a grid. Above this, the rejection is too gradual to
provide isolation.

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
design — the same proportional band. At 0.5 m spacing, the gate
is 0.325 m: neighbors within 0.325 m of each other (with no
obstacle between) are considered same-side.

### 2c. The Laplacian SMOOTHING — 3 passes, lambda 0.4

**File:** `engine/include/Engine/GI/cubemap_volume.h` (`GiVolumeBakeConfig::smoothingPasses`, `smoothingLambda`)

Post-bake value smoothing over same-side neighbor pairs. 3 passes
at lambda 0.4 (40% toward the neighbor average per pass). The
purpose: soften the transition between directly-traced probes and
dilated (filled) probes, and smooth the per-probe Monte Carlo
noise.

### 2d. The CLEAN-CELL threshold — `1.35` spacing units

**File:** `engine/src/GI/cubemap_volume.cpp` (the clean-flag pass)

A cell [v, v+1]³ is "clean" (fast path: plain trilerp, no
rejection) only if all 8 corners have all 6 axis distances
≥ 1.35 spacing units. This is conservative — the theoretical
no-rejection threshold for the hybrid reach is **~0.667**
(max offset 1.0 / reach factor 1.5), so cells in the
[0.667, 1.35] range take the weighted path unnecessarily (same
values, more fetches — a perf cost, not a correctness issue).

**Why 1.35:** = 1.0 (max in-cell offset) + 0.35 (the original
absolute slack). The hybrid reach's actual threshold is lower
(1.0 / 1.5 ≈ 0.667), but the conservatism is harmless.

### 2e. The FIREFLY CLAMP — `8×` the probe's finite-sample mean

**File:** `engine/src/GI/cubemap_volume.cpp` (inside `BakeGiVolume`)

Each probe's radiance samples are clamped to 8× the probe's own
finite-sample mean luminance. This is the standard firefly filter
(sun-pool outliers tamed per-probe, not globally).

### 2f. The probe-count CAP — `256` per-axis

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

**Policy:** the magic is the identity. No version members, no legacy
readers, no conversions. Old files refuse at the magic — re-bake.

| Format | Magic | Notes |
|--------|-------|-------|
| `.givolume` | `GVOL` | the post-visibility shape |
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

### 4b. The hull FADE — authored per-plane, `insideFade` + `outsideFade`

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
| Grid-aligned dark squares | 1b ramp floor | higher (0.4–0.5) |
| Too-dim overall | 2c smoothing lambda | lower (less averaging) |
| Wall seams (hard transitions) | 2b same-side gate | widen the slack component |

All of these are **compile-time constants** — edit the source, rebuild.
They are properties of the algorithm, not user-facing settings. The
runtime has no UI for them by design (changing them affects the
bake's output semantics, not just the display).

---

*This file is the system reference. Update it whenever a hardcoded
tolerance changes. The values are load-bearing — each one was
chosen to resolve a specific artifact observed on real content
(sponza, bistro). Changing them without re-validating on those
scenes risks reintroducing the artifact they were tuned to fix.*
