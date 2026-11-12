#!/usr/bin/env python3
"""Scenario-keyed offline GI image-quality comparison. Requires numpy.

Inputs are aligned .npz files with mandatory irradiance (H,W,3) float32
linear HDR arrays and optional depth (H,W), normal (H,W,3), surface_id
(H,W), wall_mask (H,W), boundary_mask (H,W), and valid_mask (H,W).
The reference supplies shared geometry; candidate geometry must match
when supplied. Captures are produced independently from timed bench runs.
"""
import argparse
import json
import math
from pathlib import Path
import sys

try:
    import numpy as np
except ImportError:
    np = None

LUMA = (0.2126, 0.7152, 0.0722)


def require_numpy():
    if np is None:
        raise RuntimeError("quality comparison requires numpy (pip install numpy)")


def load_capture(path):
    require_numpy()
    with np.load(path, allow_pickle=False) as data:
        if "irradiance" not in data:
            raise ValueError(f"{path}: missing irradiance")
        out = {name: np.array(data[name]) for name in data.files}
    e = out["irradiance"]
    if e.ndim != 3 or e.shape[2] != 3 or e.shape[0] == 0 or e.shape[1] == 0:
        raise ValueError(f"{path}: irradiance must be nonempty HxWx3")
    if not np.all(np.isfinite(e)) or np.any(e < 0):
        raise ValueError(f"{path}: irradiance must be finite nonnegative linear RGB")
    h, w = e.shape[:2]
    for name in ("depth", "surface_id", "wall_mask", "boundary_mask", "valid_mask"):
        if name in out and out[name].shape != (h, w):
            raise ValueError(f"{path}: {name} must have shape {(h, w)}")
    if "normal" in out and out["normal"].shape != (h, w, 3):
        raise ValueError(f"{path}: normal must have shape {(h, w, 3)}")
    for name in ("depth", "normal"):
        if name in out and not np.all(np.isfinite(out[name])):
            raise ValueError(f"{path}: nonfinite {name}")
    return out


def stats(a):
    if not a.size:
        return {"count": 0, "mean": None, "p95": None, "p99": None, "max": None}
    return {"count": int(a.size), "mean": float(np.mean(a)),
            "p95": float(np.percentile(a, 95)),
            "p99": float(np.percentile(a, 99)), "max": float(np.max(a))}


def ensure_geometry(ref, cand):
    if cand["irradiance"].shape != ref["irradiance"].shape:
        raise ValueError("capture resolutions differ")
    for key in ("depth", "normal", "surface_id", "valid_mask", "wall_mask", "boundary_mask"):
        if key in cand:
            if key not in ref or not np.allclose(cand[key], ref[key], atol=1e-6, rtol=0):
                raise ValueError(f"candidate {key} differs from reference geometry/mask")


def block_aggregate(errors, ref, mask, scale):
    """Same-surface block aggregation, NEVER spatially filter across geometry.
    This groups scalar errors (not HDR values). It is an error-scale summary,
    not SSIM/FLIP or a physical lighting-field spatial low pass.
    """
    h, w = errors.shape
    labels = ref.get("surface_id")
    if labels is None:
        return None  # no safe region identity
    values = []
    for y in range(0, h, scale):
        for x in range(0, w, scale):
            ids = labels[y:y+scale, x:x+scale]
            local_mask = mask[y:y+scale, x:x+scale]
            local_error = errors[y:y+scale, x:x+scale]
            if not np.any(local_mask):
                continue
            for ident in np.unique(ids[local_mask]):
                selected = local_mask & (ids == ident)
                if np.any(selected):
                    values.append(float(local_error[selected].mean()))
    return stats(np.asarray(values, dtype=np.float64))


def compare(ref, cand, epsilon=0.01, dark_threshold=0.05):
    require_numpy()
    if epsilon <= 0 or dark_threshold < 0:
        raise ValueError("epsilon must be positive and dark threshold nonnegative")
    ensure_geometry(ref, cand)
    r = ref["irradiance"].astype(np.float64)
    c = cand["irradiance"].astype(np.float64)
    yr = np.tensordot(r, LUMA, axes=([2], [0]))
    yc = np.tensordot(c, LUMA, axes=([2], [0]))
    logerr = np.abs(np.log2((yc + epsilon) / (yr + epsilon)))
    # Exposure-independent chromaticity. Suppress near-black chroma noise
    # through a reference luminance floor rather than mislabeling it DeltaE.
    rc = r / (r.sum(axis=2, keepdims=True) + 3 * epsilon)
    cc = c / (c.sum(axis=2, keepdims=True) + 3 * epsilon)
    chroma = np.linalg.norm(cc - rc, axis=2)
    mask = ref.get("valid_mask", np.ones(yr.shape, dtype=bool)).astype(bool)
    sections = {"all": mask, "dark": mask & (yr < dark_threshold)}
    for name in ("wall_mask", "boundary_mask"):
        if name in ref:
            sections[name[:-5]] = mask & ref[name].astype(bool)
    out = {"schema_version": 1, "width": int(r.shape[1]), "height": int(r.shape[0]),
           "epsilon": epsilon, "dark_threshold": dark_threshold,
           "regions": {}, "multiscale": {}, "notes": [
               "Linear HDR irradiance, not tonemapped display pixels",
               "Chroma is normalized-RGB distance, NOT perceptual DeltaE",
               "Scale summaries aggregate per-surface error; they are NOT SSIM/FLIP",
               "Masks are supplied by capture; no wall leak can be inferred from image error alone",
           ]}
    for name, selected in sections.items():
        out["regions"][name] = {"log_luma_stops": stats(logerr[selected]),
                                "chroma_distance": stats(chroma[selected]),
                                "brightening_stops": stats(np.maximum(
                                    np.log2((yc[selected]+epsilon)/(yr[selected]+epsilon)), 0))}
    for scale in (2, 4, 8):
        v = block_aggregate(logerr, ref, mask, scale)
        if v is not None:
            out["multiscale"][str(scale)] = v
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--scenario", required=True)
    p.add_argument("--catalog", type=Path, default=Path(__file__).resolve().parents[1] / "benchmarks/scenarios.json")
    p.add_argument("--reference", required=True, type=Path)
    p.add_argument("--candidate", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--epsilon", type=float, default=0.01)
    p.add_argument("--dark-threshold", type=float, default=0.05)
    p.add_argument("--bench-baseline", type=Path)
    p.add_argument("--bench-candidate", type=Path)
    args = p.parse_args(argv)
    try:
        catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
        if catalog.get("schema_version") != 1 or args.scenario not in [
            s["id"] for s in catalog["scenarios"]]:
            raise ValueError("scenario not in version-1 benchmark catalog")
        report = compare(load_capture(args.reference), load_capture(args.candidate),
                         args.epsilon, args.dark_threshold)
        report.update({"scenario": args.scenario,
                       "reference": str(args.reference.resolve()),
                       "candidate": str(args.candidate.resolve())})
        if bool(args.bench_baseline) != bool(args.bench_candidate):
            raise ValueError("supply both benchmark JSON reports or neither")
        if args.bench_baseline:
            a = json.loads(args.bench_baseline.read_text(encoding="utf-8"))
            b = json.loads(args.bench_candidate.read_text(encoding="utf-8"))
            if any(x.get("mode") != "bench" or x.get("ok") is not True or
                   x.get("scenario") != args.scenario for x in (a,b)):
                raise ValueError("benchmark reports must be successful bench runs for this scenario")
            if any(a.get(k) != b.get(k) for k in
                   ("frames", "source_args", "app_args", "environment")):
                raise ValueError("benchmark workloads differ")
            t0 = float(a["summary"]["median_ms"])
            t1 = float(b["summary"]["median_ms"])
            report["performance"] = {"baseline_ms": t0, "candidate_ms": t1,
                                     "change_percent": 100*(t1/t0-1)}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, ZeroDivisionError) as exc:
        print(f"quality comparison failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
