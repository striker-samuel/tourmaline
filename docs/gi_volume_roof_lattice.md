# GI-volume roof lattice: investigation record

**Status:** the `dilationBrightness` donor weighting (§5) is an
**experimental prototype, not the fix**. §7 showed it barely moves the image;
§9.1 then measured that dilated probes contribute **~0** of the roof's sampled
irradiance — so the dark-roof lattice is **not** driven by dilated-probe
darkness at all. The pattern is baked into the *traced* field (or the
interpolation / directional SG response), which is the open question.

This document records the diagnosis of the "dark roof blotches" lattice in
the bistro GI-volume bake, the hypothesis space explored, the one
intervention trialled, and the quantitative result that weakens the initial
explanation. It exists so the experiment is reproducible and the next step
is unambiguous.

---

## 1. Symptom

Looking up at the diagonal roof of the bistro interior, the baked GI shows a
**regular lattice of darker blotches** aligned with the probe grid's axes
rather than with the roof surface. It reads as a staircase-like banding that
marches diagonally across the sloped ceiling.

- Scene: `content/models/bistro/bistro.geometry` (tour
  `content/scenes/bistro.tour`, project `F:\projects\game`).
- View: irradiance debug view (`TOURMALINE_GI_DEBUG=1`, `m_giDebug == 1`
  flat-irradiance).
- Camera: pos `(24.72, 26.60, -7.02)`, fwd `(-0.157, -0.754, 0.638)`.

## 2. Initial root-cause hypothesis

The probe grid is axis-aligned; the roof is not. Probes that fall *inside*
the roof slab (below the diagonal surface, within the solid) are invalid and
must be filled by **validity dilation** from their same-side valid
neighbours. The hypothesis was:

> Dilated (inside-roof) probes are dark relative to traced probes, and their
> periodically-staggered arrangement against the diagonal roof is what paints
> the lattice onto the ceiling.

The bake-time measurement supported the first half: **dilated probes trace
to a mean luminance of 185.3 vs 312.4 for traced probes — 59% of traced**
(§7, config-0). So the dilated probes really are dark. The open question was
whether that darkness is what the camera sees.

## 3. Hypotheses ruled out before the intervention

| Hypothesis | Test | Result |
| --- | --- | --- |
| Runtime visibility rejection darkens dilated probes | `GI_DEBUG=4` (visibility wSum view) | no rejection shown at the roof; **ruled out** |
| The SG fit differs from raw per-probe data | `GI_DEBUG=5` (raw SG) vs `GI_DEBUG=1` (weighted) | identical; **ruled out** |
| Grid resolution is the cause | spacing A/B `0.45 m` → `0.225 m` (roof-local) | shrinks the band but per-probe darkness persists; **not the fix** |

`GI_DEBUG` modes (the `m_giDebug` byte in `WorldRenderer`): `0` off, `1` flat
irradiance, `2` source, `3` probe distance, `4` visibility wSum, `5` raw SG,
`6` redistribution D. Modes `5` and `6` were added for this investigation
(commit `1fdb2b1a`).

## 4. Diagnostic tooling (landed, committed)

Two diagnostic surfaces were added to make the probe state observable:

- `1fdb2b1a` — *debug(gi): add raw-SG and visibility-redistribution views*
  (`TOURMALINE_GI_DEBUG` modes 5/6).
- `efbaa8c4` — *debug(gi): add staged probe diagnostics to baker*
  (`GiBaker --diag-probes <dir>` writes three `.gidg` snapshots:
  `probes_posttrace.gidg`, `probes_postdilation.gidg`,
  `probes_postsmoothing.gidg`; each records per-probe classification
  (`0` traced / `1` dilated / `2` unfilled), the spherical mean, and the
  pre-fit sample mean).

The `.gidg` header records `dims (nx,ny,nz)`, `spacing`, `gridMin`, and the
stage, which is what makes the config-0 vs config-1 comparison below a valid
isolation.

## 5. The intervention (experimental prototype, uncommitted)

The chosen fix candidate is **surface-side-aware dilation donor selection**:
instead of averaging all same-side donors uniformly, weight each donor by its
mean luminance raised to a power, so the brightest (exterior/sky-side) donor
dominates the fill.

Prototype (working tree, **uncommitted** — kept experimental by design):

- `engine/include/Engine/GI/cubemap_volume.h` — `GiVolumeBakeConfig::
  dilationBrightnessPower` (`0.0` = the classic uniform fill).
- `engine/src/GI/cubemap_volume.cpp` — the validity-dilation loop now
  accumulates `coeffs * donorW` and divides by `sum(donorW)`, where
  `donorW = max(luminance(donor), 1e-6) ^ dilationBrightnessPower`.
- `tools/gi_baker/gi_baker.h` / `tools/gi_baker/main.cpp` —
  `--dilation-brightness <power>` CLI flag wired into the bake config.

The knob is **not** part of the GI bake freshness hash (§6), so it can be
trialled without invalidating the canonical config.

## 6. The pak gate (why the first config-1 bake was refused)

`Pak.exe` recomputes the per-volume bake-input hash with
`CanonicalGiVolumeCfgHash` (`engine/include/Engine/GI/bake_inputs.h`), which
pins **`captureDirs == 1536`** (the engine default). A bake made with
`--gi-volume-dirs 256` therefore hashes differently and is refused
(`STALE BAKE REFUSED`, exit 1, no pak written).

`dilationBrightnessPower` is **not** an input to that hash, so it is a free
knob — but the bake must be re-run at the canonical 1536 directions to pass
the gate.

## 7. A/B result

Two bakes of the same grid, differing only in `dilationBrightnessPower`
(`0.0` vs `1.0`):

- Grid: `243 × 71 × 256` probes @ `0.45 m` spacing (identical for both), with
  `45,686` dilated probes (identical classification) and essentially
  identical traced means (`312.4` vs `312.2`).

| Metric | config-0 (power 0) | config-1 (power 1.0) | Δ |
| --- | ---: | ---: | ---: |
| Dilated-probe mean luminance | 185.3 | 275.5 | **+48.7%** |
| Dilated / traced ratio | 0.593 | 0.883 | +0.290 |
| Roof-region luminance (screenshot, lower-left) | 194.6 | 196.5 | **+1.0%** |
| Roof local contrast (lattice amplitude, std) | 3.91 | 3.82 | **−2.3%** |

The bake-side change is large: dilated probes recovered from 59% to 88% of
traced energy. The **image-side change is near noise**: the roof is ~1%
brighter and the lattice amplitude is ~2% lower.

Screenshots (irradiance view, roof camera, `TOURMALINE_GI_DEBUG=1`):

| Capture | File | rgb_sha256 |
| --- | --- | --- |
| control | `out/capture-roof-ctrl/capture.png` | `1f0bf4a9c0edcdb9f0ec4fd78cd392da918ccbf9d236a05a62272891e6fc2537` |
| config-1 | `out/capture-roof-b1/capture.png` | `eb6d4cbf691bce46f35c12516529dc047d488ccfa9682dbdf47d46771d8979fb` |

## 8. Conclusion

**Raising dilated-probe energy by ~49% did not materially remove the
artifact.** That weakens the explanation that dark dilated probes are
directly responsible for most of the visible lattice — and §9.1 settles it:
the dilated probes do not contribute to the roof's sampled field at all.

The next experiment must establish where in the *traced* field the lattice
arises (directional SG evaluation vs interpolation), rather than chasing the
dilated fill.

## 9. The W_dilated diagnostic view (implemented, diagnostic-only)

To settle §8, a per-pixel dilated-probe contribution view was added:

```
                 Σ_i  w_i · I_dilated,i
W_dilated  =  --------------------------
                 Σ_i  w_i
```

`I_dilated,i` is 1 for a dilated probe and 0 otherwise; `w_i` is the
*ordinary* trilinear interpolation weight (the same eight-corner weights as
the raw-SG view, **no visibility rejection** — Σ`w_i` = 1). It renders as a
blue→red heatmap via `TOURMALINE_GI_DEBUG=7` (new `m_giDebug` mode 7).

Design, per the investigation record:

- **Provenance is persisted separately from validity.** The bake derives a
  3-way class (`0` traced / `1` dilated / `2` unfilled) from `validTraced`
  and `valid` *before* the dilation mutates the mask — the final `valid[]`
  alone cannot tell traced apart from dilated.
- **Same eight-corner ordinary interpolation as mode 5**, so the measure
  isolates the bake-stage population's share of the raw field.
- **Coverage, not luminance**: a scalar 0..1 heatmap.
- **The mask must match the exact rendered artifact.** The sidecar header
  carries dims/gridMin/spacing + the FNV-1a64 of the `.givolume` bytes; the
  renderer verifies all four before trusting the mask, and logs
  `[GIV] class-volume mismatch` otherwise (the view reads 0).

Run workflow (diagnostic-only; the production `.givolume` format and the
production sampler `SgGiVolumeSample` are untouched):

```
GiBaker --tour ... --dilation-brightness 1.0 --class-volume out/bistro.classvol
Pak.exe ... (re-cook the pak with the freshly stamped volume)
TOURMALINE_GI_DEBUG=7 \
TOURMALINE_GI_CLASS_VOLUME=<abs path>/bistro.classvol \
Benchmark.exe --pak ... --bench 64 ...
```

`--class-volume` also bypasses the bake's freshness skip (a diagnostic that
demands a fresh trace). The R8 classification volume is a **temporary**
sidecar format (`GCLS`), not part of `.givolume` — deliberately so a stale
or misaligned dump fails loudly instead of silently skewing a correlation.
The per-pixel counterfactual `|control − config1|` is computed from the two
irradiance captures (§7) and compared against the `W_dilated` heatmap
(`out/analyze_wdilated.py`).

### Interpretation

| Observation | Interpretation |
| --- | --- |
| Dark lattice aligns with high `W_dilated` | Dilated probes remain implicated; their directional radiance or local distribution needs investigation |
| Dark lattice aligns with low `W_dilated` | The pattern is primarily from directly traced probes or interpolation |
| `W_dilated` high but the brightness intervention had little effect | Compare the directional SG response, not just spherical-mean luminance |
| `W_dilated` itself forms the same lattice | A geometric connection, not necessarily causation |

Only after this measurement should the fix direction be chosen among
dilation, surface-aware relocation, or a different GI representation. Do not
promote `dilationBrightness` from prototype to fix on the strength of §7.

### 9.1 Measured result (the roof lattice is **not** from dilated probes)

`TOURMALINE_GI_DEBUG=7` over the roof camera (`24.72,26.60,-7.02` →
`-0.157,-0.754,0.638`), with the classification sidecar verified against the
exact baked `.givolume`:

| Region | W_dilated (share of interpolation from dilated probes) |
| --- | --- |
| Roof (lower-left, where the lattice is) | **≈ 0.000** |
| Small top-left patch (thick wall/column) | up to 0.416 |
| Full frame — samples with W_dilated > 0.1 | 3,388 / 129,600 (**2.6%**) |

`W_dilated` is **~0 across the entire roof**: the renderer's eight-corner
interpolation at the roof surface uses *traced* probes, not the dark dilated
ones. So the dilated probes' bake-time darkness (185 vs 312, §7) never reaches
the roof — which is *why* the brightness intervention barely moved the image.
Full-frame `r(W_dilated, irradiance luminance) = −0.43` (the only W>0 region
is a small dark wall patch).

This is the second row of the interpretation table:

> **Dark lattice aligns with low `W_dilated` → the pattern is primarily from
> directly traced probes or interpolation.**

Combined with §7, the "dark dilated probes paint the lattice" hypothesis is
**refuted** as the primary driver. The remaining suspects are the directional
SG response (`SgGiDiffuseIrradiance` at the roof normal vs the axis-aligned
lobes) and the axis-aligned trilinear interpolation itself — a "traced-only"
A/B (mask dilated probes to zero and confirm the lattice persists) is the
decisive next measurement.

Captures: `out/capture-roof-wdilated/capture.png` (W_dilated heatmap,
`rgb_sha256 9a12776a…`), `out/capture-roof-irr/capture.png` (irradiance of the
matching bake, `d500249a…`). Analysis: `out/analyze_wdilated.py`.

## 10. Reproducibility

**Bakes** (grid `243×71×256 @ 0.45 m`, `4,416,768` probes):

- control — the pre-existing canonical bake (default
  `dilationBrightnessPower == 0`); this is the bake that produced the
  reported lattice.
- config-1 (exact command):

  ```
  GiBaker.exe --content-dir F:\projects\game \
              --tour F:\projects\game\content\scenes\bistro.tour \
              --dilation-brightness 1.0 \
              --diag-probes out\diag-bistro-b1
  ```

  Result: `4,416,768` probes traced in `511.3 s`; wrote
  `F:\projects\game\gi\32cfe0c72f7189eb8ee0914da1f977bb.givolume`
  (`46,107,489 B`).

**Probe diagnostics** (ephemeral, under `out/`):

- config-0: `out/diag-bistro/` (diagnosis phase; produced before the knob
  existed).
- config-1: `out/diag-bistro-b1/`.
- The `.gidg` dump does not record the capture-direction count; the A/B above
  is a valid isolation because both dumps share the identical grid,
  classification, and traced mean — only the dilation weight differs. (The
  config-0 dump may have been produced at a non-canonical direction count
  during the diagnosis phase; this does not affect the integrated mean.)

**Package / tour state:**

- `out/build/x64-Release/content/paks/bistro.pak` now embeds the config-1
  bake (`gi=172c32f559667ed48a5dc80dc9316131.givolume`, the `--class-volume`
  run) — the `W_dilated` view (§9.1) renders against this exact artifact.
- The classification sidecar is `out/bistro.classvol` (4,416,812 B =
  44 B header + 4,416,768 class bytes; 4,370,354 traced / 45,686 dilated /
  728 unfilled).
- The original control pak is backed up at
  `out/build/x64-Release/content/paks/bistro.ctrl.bak`.
- The tour is stamped to config-1; the pre-investigation tour backup is
  `F:\projects\game\content\scenes\bistro.tour.diagbak`.

**Capture pins** (both captures, via `tools/tourcapture.py`):

```
TOURMALINE_GI_DEBUG=1  TOURMALINE_AA=off  TOURMALINE_AUTO_EXPOSURE=0
TOURMALINE_CAM_POS=24.72,26.60,-7.02
TOURMALINE_CAM_FWD=-0.157,-0.754,0.638
TOURMALINE_CAPTURE=48
Benchmark.exe --pak content/paks/bistro.pak --disable_control --bench 64 --present immediate
```

GPU: `rtx5070ti-617.42`. The `rgb_sha256` in each `capture.json` is the
encoding-independent pixel-content identity; use it (not the PNG file bytes)
to verify a reproduced capture.
