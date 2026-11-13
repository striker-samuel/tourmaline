#!/usr/bin/env python3
"""RoughW-binned specular error for the compact rough-specular A/B.

Compares two linear-HDR specular captures (SG9 reference vs a candidate: the
frozen Oct8 cubemap path, or the compact octahedral specular field) binned by
the shader's ACTUAL crossover weight, not the G-buffer perceptual roughness:

    roughW = saturate((shadingRoughness - 0.45) / 0.25)

    roughW == 0        cubemap-only (compact specular never fetched)
    0 < roughW < 1     partial crossover (cubemap <-> SG/compact blend)
    roughW == 1        full crossover (SG/compact response only)

roughW comes from the DIAG capture (SPEC_GI_EXPORT_DIAG, channel 2), which is
bit-identical across permutations (it depends only on shadingRoughness), so a
single capture drives the bins for both the reference and the candidate.

Metrics per bin:
    abs_hdr          ||Sc - Sr||_2  (mean / P95 / P99 / P99.9)
    log_luma         |log2((Yc+e)/(Yr+e))|  (mean / P95 / P99 / P99.9)
    signed_mean      mean of signed log2 ratio (+ = candidate brighter)
    brightening_frac fraction of pixels > +0.01 stop
    exact_match_pct  bitwise-equal RGB fraction
    ref_luma_pXX     reference-luminance percentiles (where the error sits)
    energy_recovery  sum(Yc)/sum(Yr) over pixels with Yr > min_luma

The energy_recovery ratio (R_E) is reported separately for the partial and
full crossover bins: a value near 1 means the candidate does not system-
atically lose reflected energy (it does NOT guarantee directional accuracy).
"""
import argparse
import json
from pathlib import Path

import numpy as np

LUMA = (0.2126, 0.7152, 0.0722)
BINS = [
    ("roughw0", "roughW == 0 (cubemap)", lambda rw: rw == 0.0),
    ("roughw_partial", "0 < roughW < 1 (crossover)", lambda rw: (rw > 0.0) & (rw < 1.0)),
    ("roughw1", "roughW == 1 (full SG/compact)", lambda rw: rw == 1.0),
]
# Percentiles to report for the error tails.
TAILS = (95, 99, 99.9)


def stats(a):
    if a.size == 0:
        return {"count": 0, "mean": None, **{f"p{t}": None for t in TAILS}, "max": None}
    return {"count": int(a.size), "mean": float(np.mean(a)),
            **{f"p{t}": float(np.percentile(a, t)) for t in TAILS},
            "max": float(np.max(a))}


def luma(x):
    return np.tensordot(x.astype(np.float64), LUMA, axes=([2], [0]))


def load(path):
    d = np.load(path, allow_pickle=False)
    return {k: np.array(d[k]) for k in d.files}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--reference", required=True)
    p.add_argument("--candidate", required=True)
    p.add_argument("--roughw", required=True, help="DIAG capture npz (roughW in channel 2)")
    p.add_argument("--output", required=True)
    p.add_argument("--epsilon", type=float, default=0.01)
    p.add_argument("--min-luma", type=float, default=0.1,
                   help="reference-luminance floor for the energy-recovery ratio")
    args = p.parse_args(argv)

    r = load(args.reference)
    c = load(args.candidate)
    rw = load(args.roughw)
    if "irradiance" not in rw or rw["irradiance"].shape[-1] < 3:
        raise SystemExit(f"{args.roughw}: not a DIAG capture (need 3 channels)")
    roughw = rw["irradiance"][..., 2].astype(np.float64)

    e = args.epsilon
    yr = luma(r["irradiance"])
    yc = luma(c["irradiance"])
    logerr = np.abs(np.log2((yc + e) / (yr + e)))
    signed = np.log2((yc + e) / (yr + e))
    rgbr = r["irradiance"].astype(np.float64)
    rgbc = c["irradiance"].astype(np.float64)
    abs_hdr = np.linalg.norm(rgbc - rgbr, axis=2)
    exact = np.all(rgbc == rgbr, axis=2)

    valid = r.get("valid_mask", np.ones(yr.shape, dtype=bool)).astype(bool)
    metal = r.get("metal", np.full(yr.shape, 0.5))
    rough = r.get("roughness", None)

    report = {"epsilon": e, "min_luma": args.min_luma, "bins": {}}

    # A single roughW mask drives every bin (bit-identical across permutations).
    rw_valid = valid & (roughw >= 0.0) & (roughw <= 1.0)

    def energy_ratio(sel):
        m = sel & (yr > args.min_luma)
        if not m.any():
            return None
        return float(np.sum(yc[m]) / np.sum(yr[m]))

    for key, label, mask in BINS:
        sel = rw_valid & mask(roughw)
        le = logerr[sel]
        se = signed[sel]
        yr_sel = yr[sel]
        frac = {}
        for th in (0.25, 0.5, 1.0):
            frac[str(th)] = float(np.mean(le > th)) if le.size else None
        w = np.maximum(yr_sel, 1e-3)
        wmean = float(np.average(le, weights=w)) if le.size else None
        entry = {
            "label": label,
            "count": int(le.size),
            "coverage_pct": float(100.0 * le.size / max(rw_valid.sum(), 1)),
            "abs_hdr": stats(abs_hdr[sel]),
            "log_luma": stats(le),
            "signed_mean": float(np.mean(se)) if se.size else None,
            "brightening_fraction": float(np.mean(se > 0.01)) if se.size else None,
            "exceed_frac": frac,
            "exact_match_pct": float(100.0 * np.mean(exact[sel])) if le.size else None,
            "ref_luma_p50_p95_p99": [float(np.percentile(yr_sel, t)) if yr_sel.size else None
                                      for t in (50, 95, 99)],
            "ref_luma_weighted_mean": wmean,
            "energy_recovery": energy_ratio(sel),
        }
        # Secondary material-analysis metadata (perceptual roughness x metal)
        # inside each roughW bin, when the G-buffer twin is present.
        if rough is not None:
            sub = {}
            for rlo, rhi in ((0.0, 0.45), (0.45, 0.70), (0.70, 1.01)):
                for mname, mmask in (("dielectric", metal < 0.5), ("metallic", metal >= 0.5)):
                    ssel = sel & (rough >= rlo) & (rough < rhi) & mmask
                    if ssel.any():
                        sub[f"r{rlo:.2f}-{rhi:.2f} {mname}"] = {
                            "count": int(ssel.sum()),
                            "abs_hdr_mean": float(np.mean(abs_hdr[ssel])),
                            "log_luma_mean": float(np.mean(logerr[ssel])),
                        }
            if sub:
                entry["by_perceptual_roughness"] = sub
        report["bins"][key] = entry

    # Cross-bin coverage sanity (sums to 100% when roughW is well-formed).
    report["roughw_coverage"] = {
        "total_pct": float(100.0 * rw_valid.sum() / max(valid.sum(), 1)),
        "roughw0_pct": float(100.0 * (rw_valid & (roughw == 0.0)).sum() / max(rw_valid.sum(), 1)),
        "partial_pct": float(100.0 * (rw_valid & (roughw > 0.0) & (roughw < 1.0)).sum()
                             / max(rw_valid.sum(), 1)),
        "roughw1_pct": float(100.0 * (rw_valid & (roughw == 1.0)).sum() / max(rw_valid.sum(), 1)),
    }

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
