#!/usr/bin/env python3
"""Roughness/metallicity-conditioned specular (or irradiance) error.

Bins the log-luminance error of two captures by the per-pixel roughness and
metallicity (from the SSR thin G-buffer exported alongside the capture) so
the rough-specular tail is separated from the cubemap control region.

Bins match the shader crossover: roughW = saturate((r - 0.45) / 0.25).
    roughness 0.00-0.45  cubemap control (roughW == 0)
    roughness 0.45-0.70  partial SG crossover
    roughness 0.70-1.00  full SG replacement
Metallicity: dielectric (metal < 0.5) vs metallic (metal >= 0.5).
"""
import argparse
import json
from pathlib import Path

import numpy as np

LUMA = (0.2126, 0.7152, 0.0722)
ROUGH_BINS = [(0.0, 0.45), (0.45, 0.70), (0.70, 1.01)]


def stats(a):
    if a.size == 0:
        return {"count": 0, "mean": None, "p95": None, "p99": None, "max": None}
    return {"count": int(a.size), "mean": float(np.mean(a)),
            "p95": float(np.percentile(a, 95)), "p99": float(np.percentile(a, 99)),
            "max": float(np.max(a))}


def load(path):
    d = np.load(path, allow_pickle=False)
    out = {k: np.array(d[k]) for k in d.files}
    if "roughness" not in out or "metal" not in out:
        raise ValueError(f"{path}: missing roughness/metal (re-capture with the G-buffer on)")
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--reference", required=True)
    p.add_argument("--candidate", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--epsilon", type=float, default=0.01)
    args = p.parse_args(argv)
    args.output = Path(args.output)

    r = load(args.reference)
    c = load(args.candidate)
    e = args.epsilon
    yr = np.tensordot(r["irradiance"].astype(np.float64), LUMA, axes=([2], [0]))
    yc = np.tensordot(c["irradiance"].astype(np.float64), LUMA, axes=([2], [0]))
    logerr = np.abs(np.log2((yc + e) / (yr + e)))          # absolute (stops)
    signed = np.log2((yc + e) / (yr + e))                   # + = candidate brighter
    roughness = r["roughness"]
    metal = r["metal"]
    valid = r.get("valid_mask", np.ones(roughness.shape, dtype=bool)).astype(bool)

    report = {"epsilon": e, "bins": {}}
    # Absolute HDR RGB difference (the energy mismatch, separate from the
    # near-black-inflating log-luminance ratio).
    rgbr = r["irradiance"].astype(np.float64)
    rgbc = c["irradiance"].astype(np.float64)
    abs_rgb = np.linalg.norm(rgbc - rgbr, axis=2)
    exact_rgb = np.all(rgbc == rgbr, axis=2)
    for (rlo, rhi) in ROUGH_BINS:
        for metal_name, metal_mask in (("dielectric", metal < 0.5), ("metallic", metal >= 0.5)):
            sel = valid & (roughness >= rlo) & (roughness < rhi) & metal_mask
            le = logerr[sel]
            se = signed[sel]
            yr_sel = yr[sel]
            frac = {}
            for th in (0.25, 0.5, 1.0):
                frac[str(th)] = float(np.mean(le > th)) if le.size else None
            w = np.maximum(yr_sel, 1e-3)
            wmean = float(np.average(le, weights=w)) if le.size else None
            report["bins"][f"r{rlo:.2f}-{rhi:.2f} {metal_name}"] = {
                "count": int(le.size),
                "coverage_pct": float(100.0 * le.size / max(valid.sum(), 1)),
                "abs_log_luma": stats(le),
                "signed_mean": float(np.mean(se)) if se.size else None,
                "brightening_fraction": float(np.mean(se > 0.01)) if se.size else None,
                "exceed_frac": frac,
                "abs_rgb": stats(abs_rgb[sel]),
                "exact_match_pct": float(100.0 * np.mean(exact_rgb[sel])) if le.size else None,
                "ref_luma_p50_p99": [float(np.percentile(yr_sel, 50)) if yr_sel.size else None,
                                     float(np.percentile(yr_sel, 99)) if yr_sel.size else None],
                "ref_luma_weighted_mean": wmean,
            }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
