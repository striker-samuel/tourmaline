#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Capture one fresh Tourmaline renderer frame and optionally compare pixels.

Uses App/Benchmark's existing --bench and TOURMALINE_CAPTURE mechanisms.
Runs from the executable build directory so relative pak/shader paths resolve.
Never accepts a pre-existing screenshot as a successful new capture.

  python tools/tourcapture.py --exe out/build/linux-Release/App \
      --project content/paks/game.tapp --frames 64 --capture-frame 48 \
      --camera-pos 0,2,-4 --camera-fwd 0,0,1 --output out/captureA --json
  python tools/tourcapture.py --exe out/build/linux-Release/App \
      --project content/paks/game.tapp --frames 64 --capture-frame 48 \
      --camera-pos 0,2,-4 --camera-fwd 0,0,1 --gpu-tag 'adapter_driver' \
      --baseline out/captureA/capture.json --output out/captureB --json

With --baseline, compare only compatible frame/camera/env/GPU configs.
Pillow is mandatory for decoded RGB hashing. A GPU/display and an already
built App or Benchmark are required for an actual capture.

Exit: 0 success/equal; 1 pixel regression or App failure; 2 bad input or
incompatible baseline; 3 missing Pillow; 4 no new capture; 5 timeout.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pixel_compare

DEFAULT_PINS = {"TOURMALINE_AA": "off", "TOURMALINE_AUTO_EXPOSURE": "0"}
PIN_NAME = re.compile(r"^TOURMALINE_[A-Z0-9_]+$")
SCHEMA = 1


class CaptureError(Exception):
    pass


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe", required=True, type=Path, help="built App/Benchmark executable")
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument("--pak", help="pak path interpreted from executable workdir")
    source.add_argument("--project", help="tapp path interpreted from executable workdir")
    p.add_argument("--scene", help="optional scene name for pak lane")
    p.add_argument("--workdir", type=Path, help="defaults to executable directory")
    p.add_argument("--frames", type=int, default=64, help="bench budget (default 64)")
    p.add_argument("--capture-frame", type=int, default=48, help="capture frame inside budget")
    p.add_argument("--camera-pos", help="TOURMALINE_CAM_POS (e.g. 0,2,-4)")
    p.add_argument("--camera-fwd", help="TOURMALINE_CAM_FWD (e.g. 0,0,1)")
    p.add_argument("--env", action="append", default=[], metavar="TOURMALINE_NAME=VALUE",
                   help="repeatable explicit renderer pin")
    p.add_argument("--app-arg", action="append", default=[], metavar="--FLAG",
                   help="extra renderer argument, e.g. --app-arg=--no-dlss")
    p.add_argument("--gpu-tag", help="explicit GPU+driver identity for comparable baselines")
    p.add_argument("--output", required=True, type=Path, help="run output directory")
    p.add_argument("--overwrite", action="store_true", help="replace previous capture artifacts explicitly")
    p.add_argument("--baseline", type=Path, help="prior tourcapture capture.json (strict pin comparison)")
    p.add_argument("--reference-image", type=Path, help="manual PNG comparison (no provenance guarantee)")
    p.add_argument("--threshold", type=int, default=6)
    p.add_argument("--stride", type=int, default=1)
    p.add_argument("--timeout", type=float, default=900)
    p.add_argument("--dry-run", action="store_true", help="plan command without launching or writing")
    p.add_argument("--json", action="store_true", help="one JSON report on stdout")
    return p


def _vectors(value, name):
    if value is None:
        return None
    try:
        numbers = [float(x.strip()) for x in value.split(",")]
    except ValueError as exc:
        raise CaptureError(f"{name} must be three numeric components") from exc
    if len(numbers) != 3 or not all(-1e12 < n < 1e12 for n in numbers):
        raise CaptureError(f"{name} must be three finite numeric components")
    if name == "--camera-fwd" and all(n == 0 for n in numbers):
        raise CaptureError("--camera-fwd cannot be zero")
    return value


def config(args):
    if args.frames < 2 or not 1 <= args.capture_frame < args.frames:
        raise CaptureError("require 1 <= --capture-frame < --frames")
    if args.threshold not in range(766) or args.stride <= 0:
        raise CaptureError("--threshold must be 0..765 and --stride >= 1")
    if not 0 < args.timeout <= 3600:
        raise CaptureError("--timeout must be >0 and <=3600 seconds")
    if args.project and args.scene:
        raise CaptureError("--scene is not supported with --project (manifest owns entry)")
    if args.baseline and args.reference_image:
        raise CaptureError("choose --baseline or --reference-image")
    if args.gpu_tag is not None and (not args.gpu_tag.strip() or len(args.gpu_tag) > 128):
        raise CaptureError("--gpu-tag must be a nonempty tag <=128 characters")
    pins = dict(DEFAULT_PINS)
    for raw in args.env:
        name, sep, value = raw.partition("=")
        if not sep or not PIN_NAME.fullmatch(name) or "\x00" in value or "\n" in value:
            raise CaptureError(f"invalid --env: {raw!r} (expected TOURMALINE_NAME=value)")
        if name == "TOURMALINE_CAPTURE":
            raise CaptureError("TOURMALINE_CAPTURE is owned by --capture-frame")
        pins[name] = value
    for name, value in (("TOURMALINE_CAM_POS", args.camera_pos),
                        ("TOURMALINE_CAM_FWD", args.camera_fwd)):
        if value is not None:
            pins[name] = _vectors(value, "--camera-pos" if name.endswith("POS") else "--camera-fwd")
    if ("TOURMALINE_CAM_POS" in pins) != ("TOURMALINE_CAM_FWD" in pins):
        raise CaptureError("pin both camera position and forward direction")
    for name in ("TOURMALINE_CAM_POS", "TOURMALINE_CAM_FWD"):
        if name in pins:
            _vectors(pins[name], name)
    forbidden = {"--pak", "--project", "--scene", "--scene-index", "--bench",
                 "--disable_control", "--present", "--capture"}
    for opt in args.app_arg:
        if not opt.startswith("--") or "\x00" in opt or opt.split("=", 1)[0] in forbidden:
            raise CaptureError(f"unsafe/conflicting --app-arg: {opt!r}")
    exe = args.exe.expanduser().resolve()
    cwd = (args.workdir or exe.parent).expanduser().resolve()
    source_flag = "--pak" if args.pak else "--project"
    content = args.pak or args.project
    argv = [str(exe), source_flag, content]
    if args.scene:
        argv += ["--scene", args.scene]
    argv += ["--disable_control", "--bench", str(args.frames),
             "--present", "immediate", *args.app_arg]
    pins["TOURMALINE_CAPTURE"] = str(args.capture_frame)
    provenance = {"frames": args.frames, "capture_frame": args.capture_frame,
                  "scene": args.scene, "source_type": source_flag.lstrip("-"),
                  "pins": pins, "app_args": args.app_arg, "gpu_tag": args.gpu_tag}
    return argv, cwd, provenance


def _fresh_files(export_dir, old):
    result = []
    for path in export_dir.glob("screenshot_*.png"):
        if not path.is_file():
            continue
        stat = path.stat()
        signature = (stat.st_size, stat.st_mtime_ns)
        if old.get(path.name) != signature:
            result.append(path)
    return sorted(result, key=lambda p: (p.stat().st_mtime_ns, p.name))


def _snapshot(export_dir):
    return {p.name: (p.stat().st_size, p.stat().st_mtime_ns)
            for p in export_dir.glob("screenshot_*.png") if p.is_file()}


def _fingerprint(path):
    try:
        from PIL import Image
    except ImportError as exc:
        raise ImportError("Pillow is required: python -m pip install pillow") from exc
    with Image.open(path) as image:
        rgb = image.convert("RGB")
        return {"width": rgb.width, "height": rgb.height,
                "rgb_sha256": hashlib.sha256(rgb.tobytes()).hexdigest()}


def _read_baseline(path, provenance):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CaptureError(f"cannot read baseline manifest: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema_version") != 1 or not data.get("capture"):
        raise CaptureError("baseline must be a successful tourcapture capture.json")
    existing = data.get("provenance", {})
    if not provenance["gpu_tag"] or not existing.get("gpu_tag"):
        raise CaptureError("strict baseline comparisons require --gpu-tag and baseline gpu_tag")
    for name in ("frames", "capture_frame", "scene", "source_type", "pins", "app_args", "gpu_tag"):
        if provenance[name] != existing.get(name):
            raise CaptureError(f"baseline {name} differs; use an equivalent scenario and GPU/driver")
    shot = Path(data["capture"]["image"])
    if not shot.is_absolute():
        shot = path.parent / shot
    if not shot.is_file():
        raise CaptureError(f"baseline screenshot is missing: {shot}")
    prior = _fingerprint(shot)
    if prior["rgb_sha256"] != data["capture"].get("rgb_sha256"):
        raise CaptureError("baseline image does not match recorded decoded RGB hash")
    return shot


def _emit(report, json_mode):
    if json_mode:
        print(json.dumps(report, sort_keys=True))
    else:
        print(f"tourcapture: {report['status']} ({report.get('message', '')})")
        if report.get("capture"):
            print(f"  image: {report['capture']['image']}")
        if report.get("comparison"):
            c = report["comparison"]
            print(f"  different: {c['differing_pixels']}/{c['sampled_pixels']}")


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        argv, cwd, provenance = config(args)
        report = {"schema_version": SCHEMA, "command": argv, "workdir": str(cwd),
                  "provenance": provenance, "status": "planned", "capture": None}
        if args.dry_run:
            report["message"] = "no process launched; no output written"
            _emit(report, args.json)
            return 0
        if not cwd.is_dir():
            raise CaptureError(f"workdir does not exist: {cwd}")
        if not Path(argv[0]).is_file():
            raise CaptureError(f"executable not found: {argv[0]}")
        source = Path(args.pak or args.project)
        if not (source if source.is_absolute() else cwd / source).is_file():
            raise CaptureError(f"content source missing relative to {cwd}: {source}")
        if args.baseline:
            baseline = _read_baseline(args.baseline, provenance)
        elif args.reference_image:
            baseline = args.reference_image
            if not baseline.is_file():
                raise CaptureError(f"reference image missing: {baseline}")
        else:
            baseline = None
        # Fail fast before the expensive Vulkan session if Pillow is absent.
        try:
            from PIL import Image  # noqa: F401
        except ImportError as exc:
            raise ImportError("Pillow is required: python -m pip install pillow") from exc
        output_dir = args.output.expanduser().resolve()
        if args.baseline and args.baseline.expanduser().resolve().parent == output_dir:
            raise CaptureError("baseline and output directories must differ")
        if output_dir.exists() and not args.overwrite and any(
                (output_dir / name).exists() for name in ("capture.json", "capture.png", "diff.png")):
            raise CaptureError("output contains a prior capture; choose a new directory or --overwrite")
        if output_dir == cwd / "exports":
            raise CaptureError("output must differ from the renderer exports directory")
        output_dir.mkdir(parents=True, exist_ok=True)
        if args.overwrite:
            for name in ("capture.json", "capture.png", "diff.png"):
                (output_dir / name).unlink(missing_ok=True)
        export_dir = cwd / "exports"
        before = _snapshot(export_dir)
        log_file = output_dir / "app.log"
        env = {k: v for k, v in os.environ.items() if not k.startswith("TOURMALINE_")}
        env.update(provenance["pins"])
        try:
            with log_file.open("w", encoding="utf-8") as log:
                proc = subprocess.run(argv, cwd=cwd, env=env, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=args.timeout,
                                      check=False)
            report["app_exit_code"] = proc.returncode
            if proc.returncode != 0:
                report.update(status="app_failed", message="renderer returned nonzero (see app.log)")
                (output_dir / "capture.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
                _emit(report, args.json)
                return 1
        except subprocess.TimeoutExpired:
            report.update(status="timed_out", message="renderer exceeded timeout")
            (output_dir / "capture.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            _emit(report, args.json)
            return 5
        fresh = _fresh_files(export_dir, before)
        if not fresh:
            report.update(status="no_capture", message="renderer emitted no new screenshot_*.png")
            (output_dir / "capture.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            _emit(report, args.json)
            return 4
        shot = fresh[-1]
        dest = output_dir / "capture.png"
        if dest.resolve() == shot.resolve():
            raise CaptureError("capture destination conflicts with renderer output")
        shutil.copy2(shot, dest)
        metadata = _fingerprint(dest)
        report["capture"] = {"image": "capture.png", "source_image": str(shot),
                             "fresh_file_count": len(fresh), **metadata}
        if baseline:
            compare = pixel_compare.compare(baseline, dest, threshold=args.threshold,
                                            stride=args.stride, diff_out=output_dir / "diff.png")
            # The comparator emits absolute paths: store paths relative to
            # report folder so captures can be moved with their manifests.
            compare["diff_image"] = "diff.png"
            report["comparison"] = compare
            report["status"] = "passed" if compare["equal"] else "different"
        else:
            report["status"] = "captured"
        report["message"] = "fresh screenshot captured"
        (output_dir / "capture.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        _emit(report, args.json)
        return 1 if report["status"] == "different" else 0
    except ImportError as exc:
        print(f"tourcapture: {exc}", file=sys.stderr)
        return 3
    except (CaptureError, OSError, ValueError) as exc:
        print(f"tourcapture: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
