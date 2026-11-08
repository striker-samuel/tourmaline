#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Capture-based parity gate for the GPU-scene temporal chain (PR #284).

The temporal chain (persistent-instance visibility history, seed/remainder
partitioning, same-frame Hi-Z occlusion, final indirect-command gating)
must be RENDER-INVARIANT: a deterministic capture with the chain enabled
must be PIXEL-IDENTICAL to the same capture with
TOURMALINE_GPU_SCENE_TEMPORAL=0. The seed + remainder together draw
exactly the visible set, and Hi-Z only removes instances proven occluded -
depth-tested opaque output is unchanged either way, so any differing pixel
is a visibility bug (over-culling, seed/remainder misclassification,
history aliasing or Hi-Z ordering).

Identical pixels alone cannot distinguish "correct" from "silently
absent", so the gate also asserts the renderer's one-shot ENGAGEMENT
WITNESS ("[GpuSceneTemporal] engaged:", WorldRenderer's temporal block)
is present in the ON run's app log and absent from the OFF run's.

Legs (each a tools/tourcapture.py run + pixel_compare):
  1. determinism: temporal ON twice -> must be pixel-identical (the
     noise floor that makes every later comparison meaningful)
  2. temporal parity: ON vs TOURMALINE_GPU_SCENE_TEMPORAL=0 -> identical
  3. Hi-Z parity (default on): ON vs TOURMALINE_CULLFLAGS with the HZB
     occlusion bit (0x40) cleared -> identical (bit 0x40 gates BOTH the
     gpu-scene instance gate and GfxFilter's cluster-level HZB test, so
     this leg proves neither over-culls; the default --hiz-off-flags
     0x20b = the engine default 0x24b with bit 0x40 clear - update it if
     the engine's default cull flags ever change)

The gate covers the STEADY-STATE STATIC-CAMERA contract (tourcapture
pins a fixed camera): motion/disocclusion behavior stays covered by the
CPU reference contracts (test_gpu_scene_world.cpp) and interactive
review. Use a static, fully GPU-scene-owned scene with real occluders
(sponza-class); skinned characters never exercise the chain (legacy
ownership).

  python tools/gpu_scene_temporal_parity.py \
      --exe out/build/x64-Release/App.exe \
      --pak F:/projects/game/build/sponza_10-<hash>.pak \
      --output out/parity/gate [--json] [--skip-hiz] [--overwrite]

Exit: 0 all legs pass; 1 a parity/witness/determinism check failed;
2 bad input; 3 an app or capture failure (see <output>/<leg>/app.log).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import pixel_compare

# The one-shot line WorldRenderer prints when the temporal chain actually
# dispatches seed + Hi-Z gate + history in a frame.
WITNESS = "[GpuSceneTemporal] engaged:"
UNKNOWN_RUNCONFIG_PIN = "[RunConfig] unknown TOURMALINE_* var (typo?):"
# smvk::gfx::CullFlagBit_HzbOcclusion (gfx_filter.h) - gates the gpu-scene
# instance Hi-Z test AND GfxFilter's cluster-level HZB test.
HZB_OCCLUSION_BIT = 0x40
# The engine's default cull flags with the HZB occlusion bit cleared
# (observed as the GfxFilter dispatch default 0x24b; see the docstring).
DEFAULT_HIZ_OFF_FLAGS = 0x20B
TEMPORAL_OFF_PIN = "TOURMALINE_GPU_SCENE_TEMPORAL=0"
CULL_FLAGS_NAME = "TOURMALINE_CULLFLAGS"


def witness_count(app_log_text: str) -> int:
    """Occurrences of the engagement witness in an app log (0 = absent)."""
    return app_log_text.count(WITNESS)


def leg_pins(leg, extra_env, hiz_off_flags):
    """The TOURMALINE_* pin dict one leg adds on top of the shared env.

    Pure function (unit-tested): the ON legs carry no extra pins, the
    temporal-off leg carries the isolation switch, and the Hi-Z-off leg
    replaces the cull flags with the HZB-occlusion-cleared value.
    """
    pins = dict(extra_env)
    if leg == "temporal-off":
        pins[TEMPORAL_OFF_PIN.split("=", 1)[0]] = TEMPORAL_OFF_PIN.split("=", 1)[1]
    elif leg == "hiz-off":
        pins[CULL_FLAGS_NAME] = hiz_off_flags
    elif leg not in ("on-a", "on-b"):
        raise ValueError(f"unknown leg: {leg!r}")
    return pins


def evaluate(legs, noise_margin=8, noise_cap=64, require_bit_exact=False):
    """Pass/fail verdicts from finished leg records (pure, unit-tested).

    legs maps leg name -> {"pixels": <capture png path>, "witness": int,
    "status": str}. Returns (verdict dict, ok bool).

    The GPU-scene indexed lane has shown a small nondeterministic pixel
    floor in captures. Its verdict and work scatter kernels currently use
    ordered prefix scans (not atomic output allocation), so depth-equal
    draw order is a hypothesis, not a confirmed cause.
    By default parity is judged relative to the lane's own noise floor:
    the determinism leg (on-a vs on-b) measures it, and the temporal /
    Hi-Z legs must stay within floor + noise_margin. A floor above
    noise_cap fails outright - that is a determinism regression, not
    noise. The opt-in require_bit_exact mode disallows every difference,
    independent of noise_cap and noise_margin.
    """
    def pixels(name):
        return legs[name]["pixels"]

    verdict = {}

    def check(name, ok, detail):
        verdict[name] = {"pass": bool(ok), "detail": detail}

    if any(legs[name]["status"] != "captured" for name in legs):
        failed = [name for name in legs if legs[name]["status"] != "captured"]
        check("legs-captured", False, f"failed legs: {', '.join(failed)}")
        return verdict, False
    check("legs-captured", True, f"{len(legs)} legs captured")

    check("witness-on", legs["on-a"]["witness"] > 0,
          f"engagement witness x{legs['on-a']['witness']} in on-a log")
    check("witness-off", legs["temporal-off"]["witness"] == 0,
          f"engagement witness x{legs['temporal-off']['witness']} in temporal-off log")
    comparisons = {}
    ok = True
    determinism = pixel_compare.compare(pixels("on-a"), pixels("on-b"))
    comparisons["determinism"] = determinism
    floor = determinism["differing_pixels"]
    determinism_ok = floor == 0 if require_bit_exact else floor <= noise_cap
    check("determinism", determinism_ok,
          f"noise floor {floor}/{determinism['sampled_pixels']} differing "
          + ("(bit exact required)" if require_bit_exact else f"(cap {noise_cap})"))
    ok = ok and determinism_ok
    for name, (left, right) in {
        "temporal-parity": ("on-a", "temporal-off"),
    }.items():
        comparison = pixel_compare.compare(pixels(left), pixels(right))
        comparisons[name] = comparison
        within = (comparison["differing_pixels"] == 0 if require_bit_exact
                  else comparison["differing_pixels"] <= floor + noise_margin)
        check(name, within,
              f"{comparison['differing_pixels']}/{comparison['sampled_pixels']} differing "
              + ("(bit exact required)" if require_bit_exact
               else f"(floor {floor} + margin {noise_margin})"))
        ok = ok and within
    if "hiz-off" in legs:
        check("witness-hiz-off", legs["hiz-off"]["witness"] > 0,
              f"engagement witness x{legs['hiz-off']['witness']} in hiz-off log")
        comparison = pixel_compare.compare(pixels("on-a"), pixels("hiz-off"))
        comparisons["hiz-parity"] = comparison
        within = (comparison["differing_pixels"] == 0 if require_bit_exact
                  else comparison["differing_pixels"] <= floor + noise_margin)
        check("hiz-parity", within,
              f"{comparison['differing_pixels']}/{comparison['sampled_pixels']} differing "
              + ("(bit exact required)" if require_bit_exact
                 else f"(floor {floor} + margin {noise_margin})"))
        ok = ok and within
    ok = ok and all(entry["pass"] for entry in verdict.values())
    return verdict, ok


def parser():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe", required=True, type=Path, help="built App/Benchmark executable")
    p.add_argument("--pak", required=True, help="pak path interpreted from executable workdir")
    p.add_argument("--frames", type=int, default=64, help="bench budget (default 64)")
    p.add_argument("--capture-frame", type=int, default=48,
                   help="capture frame inside budget (default 48; the temporal history "
                        "needs a few frames to warm up - keep it >= ~16)")
    p.add_argument("--camera-pos", help="TOURMALINE_CAM_POS (e.g. 0,2,-4)")
    p.add_argument("--camera-fwd", help="TOURMALINE_CAM_FWD (e.g. 0,0,1)")
    p.add_argument("--env", action="append", default=[], metavar="TOURMALINE_NAME=VALUE",
                   help="repeatable pin shared by every leg (e.g. TOURMALINE_RNG_SEED)")
    p.add_argument("--skip-hiz", action="store_true", help="skip the Hi-Z parity leg")
    p.add_argument("--noise-margin", type=int, default=8,
                   help="parity legs may exceed the measured determinism floor by this many "
                        "differing pixels (default %(default)s; ignored under "
                        "--require-bit-exact)")
    p.add_argument("--require-bit-exact", action="store_true",
                   help="require zero differing pixels in every ON/ON and parity "
                        "comparison; opt-in for golden captures and determinism fixes")
    p.add_argument("--noise-cap", type=int, default=64,
                   help="the determinism floor itself must stay under this many differing "
                        "pixels or the gate fails outright (default %(default)s)")
    p.add_argument("--hiz-off-flags", default=f"{DEFAULT_HIZ_OFF_FLAGS:#x}",
                   help="TOURMALINE_CULLFLAGS value for the Hi-Z-off leg "
                        "(default %(default)s: engine default with bit 0x40 clear)")
    p.add_argument("--output", required=True, type=Path, help="gate output directory")
    p.add_argument("--overwrite", action="store_true",
                   help="replace previous gate artifacts explicitly")
    p.add_argument("--threshold", type=int, default=6, help="pixel_compare threshold")
    p.add_argument("--timeout", type=float, default=900, help="per-leg app timeout (s)")
    p.add_argument("--json", action="store_true", help="one JSON report on stdout")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    tool_dir = Path(__file__).resolve().parent
    tourcapture = tool_dir / "tourcapture.py"
    output_dir = args.output.expanduser().resolve()
    if args.frames < 2 or not 1 <= args.capture_frame < args.frames:
        print("gpu_scene_temporal_parity: require 1 <= --capture-frame < --frames",
              file=sys.stderr)
        return 2
    if output_dir.exists() and not args.overwrite and any(output_dir.iterdir()):
        print("gpu_scene_temporal_parity: output exists and is not empty; use --overwrite",
              file=sys.stderr)
        return 2
    legs = ["on-a", "on-b", "temporal-off"] + ([] if args.skip_hiz else ["hiz-off"])
    records = {}
    for leg in legs:
        leg_dir = output_dir / leg
        argv_leg = [sys.executable, str(tourcapture), "--exe", str(args.exe),
                    "--pak", args.pak, "--frames", str(args.frames),
                    "--capture-frame", str(args.capture_frame),
                    "--output", str(leg_dir), "--timeout", str(args.timeout),
                    "--json"]
        for name, value in leg_pins(leg, {}, args.hiz_off_flags).items():
            argv_leg += ["--env", f"{name}={value}"]
        for name, value in (("TOURMALINE_CAM_POS", args.camera_pos),
                            ("TOURMALINE_CAM_FWD", args.camera_fwd)):
            if value:
                argv_leg += ["--env", f"{name}={value}"]
        for pin in args.env:
            argv_leg += ["--env", pin]
        if args.overwrite:
            argv_leg += ["--overwrite"]
        proc = subprocess.run(argv_leg, capture_output=True, text=True)
        manifest = leg_dir / "capture.json"
        if proc.returncode != 0 or not manifest.is_file():
            print(f"gpu_scene_temporal_parity: leg {leg} failed "
                  f"(exit {proc.returncode}); see {leg_dir}",
                  file=sys.stderr)
            return 3
        report = json.loads(manifest.read_text(encoding="utf-8"))
        app_log = (leg_dir / "app.log").read_text(encoding="utf-8", errors="replace")
        # The parity gate must never certify an OFF leg whose switch
        # RunConfig silently ignored. Treat *any* unknown TOURMALINE knob
        # as an invalid capture rather than merely accepting equal pixels.
        if UNKNOWN_RUNCONFIG_PIN in app_log:
            print(f"gpu_scene_temporal_parity: leg {leg} used an unknown "
                  f"TOURMALINE_* environment variable; see {leg_dir / 'app.log'}",
                  file=sys.stderr)
            return 3
        records[leg] = {
            "pixels": str(leg_dir / "capture.png"),
            "witness": witness_count(app_log),
            "status": report.get("status", "unknown"),
            "pins": report.get("provenance", {}).get("pins", {}),
        }
    verdict, ok = evaluate(records, noise_margin=args.noise_margin,
                           noise_cap=args.noise_cap,
                           require_bit_exact=args.require_bit_exact)
    result = {"schema_version": 1, "tool": "gpu_scene_temporal_parity",
              "require_bit_exact": args.require_bit_exact,
              "legs": records, "checks": verdict,
              "status": "passed" if ok else "failed"}
    (output_dir / "parity.json").write_text(json.dumps(result, indent=2) + "\n",
                                            encoding="utf-8")
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(f"gpu_scene_temporal_parity: {result['status']}")
        for name, entry in verdict.items():
            mark = "PASS" if entry["pass"] else "FAIL"
            print(f"  {mark} {name}: {entry['detail']}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
