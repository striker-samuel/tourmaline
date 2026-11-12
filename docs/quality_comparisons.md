# Scenario-based GI quality comparisons

## Why

A renderer A/B can improve frame time while introducing unacceptable lighting
leaks, darkening, or color shifts. Raw 8-bit screenshot PSNR/RMSE is not a
meaningful gate for HDR bounced light: exposure, tonemapping, AA, SSR and material
BRDF obscure irradiance errors. An overall average can also hide a small but
severe wall/doorway regression.

`tools/quality_compare.py` measures **linear HDR diffuse irradiance**
separately from uninstrumented GPU timings. It reuses the *same scenario ID*
from `benchmarks/scenarios.json` for identity and optionally attaches a
matching performance A/B to one JSON report.

No quality comparison runs in the timed benchmark process.

## Capture contract (v1)

Quality images are **not yet automatically exported by the renderer**. Capture
the reference and candidate with an identical scene/camera, resolution, GI
visibility configuration, fixed exposure, geometry, and deterministic rendering.
Export aligned `.npz` files containing these arrays:

| key | shape | required | meaning |
| --- | --- | --- | --- |
| `irradiance` | H,W,3 | yes | linear, finite, nonnegative HDR diffuse irradiance (before albedo / tonemap / AO / SSR) |
| `depth` | H,W | recommended | shared scene depth |
| `normal` | H,W,3 | recommended | shared scene normal |
| `surface_id` | H,W | recommended | stable per-surface/material-instance ID, not color |
| `valid_mask` | H,W | optional | pixels with meaningful surface irradiance |
| `wall_mask` | H,W | optional | geometrically determined wall-adjacent pixels |
| `boundary_mask` | H,W | optional | volume transitions / doorways |

Reference geometry/masks are authoritative. If candidate includes any of
these auxiliary fields they must match; mismatching resolution is an error.
Only numpy is required for the offline analysis (`pip install numpy`).

Example, from the repository root:

```powershell
python tools/quality_compare.py --scenario bistro-ssr-stress `
  --reference out/quality/ref.npz --candidate out/quality/oct.npz `
  --output out/quality/compare.json `
  --bench-baseline out/perf/ref/result.json `
  --bench-candidate out/perf/oct/result.json
```

Performance reports must have matching scenario, flags, pins, frame count,
and successful bench status. GPU timings belong to separate uninstrumented
harness runs (`perf_harness.py bench`); the optional joined report does
not assert that visual capture cost equals GPU cost.

### Implemented diagnostics

* **Absolute log2 luminance error (stops)** on linear irradiance:
  `abs(log2((Y_candidate + epsilon)/(Y_ref + epsilon)))`. Adjustable
  epsilon is **not a universal physical threshold**; calibrate it against
  reference exposure and the GI noise floor.
* **Normalized RGB chromaticity distance** catches hue shifts at similar
  luminance. It is **not** CIE DeltaE.
* **Positive log-luminance change (brightening)** helps surface suspicious
  light leaks but does not on its own diagnose physical wall transmission.
* **Mean, P95, P99 and maximum** for whole scene, dark reference pixels,
  and optional wall/boundary masks.
* **Same-surface multiscale block mean error** at 2/4/8 pixels when stable
  surface IDs exist. This summarizes spatial error without crossing surface
  IDs. It is **not** MS-SSIM or a geometry-aware filtered HDR image.

No universal pass/fail thresholds are imposed; calibrate using approved
reference/candidate pairs and review the worst-wall and dark-interior images.
Store the reports alongside perf results. Avoid interpreting a lower mean
error as proof of no leak.

## Proposed follow-ups (not implemented in this PR)

| Metric/tool | Why it belongs in scenario benchmarks |
| --- | --- |
| Edge-aware irradiance pyramid (depth/normal/surface guided) | Low-frequency bounce-field error without blurring across walls; requires same geometry and guided filtering |
| Deterministic angular sweep at probe positions (Fibonacci sphere) | SG9 vs ambient cube/oct comparisons independent of camera-facing normals; requires probe evaluator export |
| HDR-VDP or display-referred FLIP with fixed display config | Perceptual image comparisons complementary to physical irradiance; third-party dependencies and explicit display metadata |
| CIE Lab / Oklab color error after fixed display transform | More perceptually useful color-difference measure than normalized RGB |
| Wall-leak / doorway contrast ratio with sided surface pairs | Detects directional leak catastrophes that global means hide; needs geometry annotation and a trusted reference |
| Temporal flicker P95 and ghosting under camera motion | Detects reprojection instability for later half-res GI or TAA; requires aligned frame sequences |
| Volume-boundary continuity and derivative jumps | Catches popping and shading seams across authored GI volume overlaps |
| Performance / quality Pareto and regression gates | Compare encoding candidates at multiple camera scenes and resolutions without collapsing unlike metrics into one arbitrary scalar |

The immediate future engine-side work is a deterministic linear HDR irradiance
export at scenario camera/frame (optionally stable surface IDs and wall masks).
This PR intentionally does **not** claim that the existing viewer already emits
those buffers.
