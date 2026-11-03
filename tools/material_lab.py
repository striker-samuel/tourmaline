#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Environment-aware PBR image regression suites over Tourmaline's tourcapture.

The tool does not implement shading: it invokes production App/WorldRenderer
through tourcapture's fresh-frame and strict GPU provenance checks.
No GPU is required for list or plan.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

SCHEMA = 1
ID = re.compile(r"^[a-z][a-z0-9_-]*$")
PIN = re.compile(r"^TOURMALINE_[A-Z0-9_]+$")
VIEWS = {
    "lit": None, "health": "pbrhealth", "base": "pbrbase",
    "diffuse": "pbrdiffuse", "roughness": "pbrrough",
    "metalness": "pbrmetal", "f0": "pbrf0",
}
DEFAULT_SUITE = Path(__file__).resolve().parents[1] / "material_lab" / "suite.json"
CAPTURE = Path(__file__).with_name("tourcapture.py")


class SuiteError(ValueError):
    pass


def _object(value, what):
    if not isinstance(value, dict):
        raise SuiteError(f"{what} must be an object")
    return value


def _id(value, what):
    if not isinstance(value, str) or not ID.fullmatch(value):
        raise SuiteError(f"{what} must be a lowercase slug")
    return value


def _vec(value, what, nonzero=False):
    if not isinstance(value, list) or len(value) != 3 or any(
        isinstance(n, bool) or not isinstance(n, (int, float)) or
        not (-1e9 < n < 1e9) for n in value
    ) or (nonzero and all(n == 0 for n in value)):
        raise SuiteError(f"{what} must be three finite numeric components")
    return ",".join(str(n) for n in value)


def _source(value, what):
    value = _object(value, what)
    if set(value) not in ({"pak"}, {"pak", "scene"}, {"project"}):
        raise SuiteError(f"{what}: expected pak [+ scene] or project")
    key = "project" if "project" in value else "pak"
    p = value[key]
    if (not isinstance(p, str) or not p or "\\" in p or "\x00" in p or
        Path(p).is_absolute() or any(x == ".." for x in Path(p).parts)):
        raise SuiteError(f"{what}: content path must be relative to App workdir")
    if "scene" in value and (not isinstance(value["scene"], str) or not value["scene"]):
        raise SuiteError(f"{what}: scene name must be nonempty")
    return value


def load_suite(path):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SuiteError(f"cannot read suite {path}: {exc}") from exc
    _object(data, "suite")
    if data.get("schema_version") != SCHEMA:
        raise SuiteError(f"suite schema_version must be {SCHEMA}")
    materials = _object(data.get("materials"), "materials")
    environments = _object(data.get("environments"), "environments")
    if not materials or not environments:
        raise SuiteError("suite needs materials and environments")
    for name, material in materials.items():
        _id(name, "material ID")
        _object(material, f"material {name}")
        camera = _object(material.get("camera"), f"material {name} camera")
        _vec(camera.get("position"), f"material {name} camera.position")
        _vec(camera.get("forward"), f"material {name} camera.forward", nonzero=True)
    for name, env in environments.items():
        _id(name, "environment ID")
        _object(env, f"environment {name}")
        _source(env.get("source"), f"environment {name} source")
        if not isinstance(env.get("external", False), bool):
            raise SuiteError(f"environment {name} external must be boolean")
        pins = _object(env.get("pins", {}), f"environment {name} pins")
        for key, val in pins.items():
            if (not PIN.fullmatch(key) or key in {
                    "TOURMALINE_CAPTURE", "TOURMALINE_CAM_POS",
                    "TOURMALINE_CAM_FWD", "TOURMALINE_DEBUGVIEW"} or
                not isinstance(val, str) or any(x in val for x in "\x00\r\n")):
                raise SuiteError(f"environment {name} has invalid pin {key}")
        args = env.get("app_args", [])
        if not isinstance(args, list) or any(not isinstance(a, str) or not a.startswith("--")
                                             for a in args):
            raise SuiteError(f"environment {name} app_args must be --flags")
    opts = _object(data.get("defaults", {}), "defaults")
    frames = opts.get("frames", 64)
    capture_frame = opts.get("capture_frame", 48)
    threshold = opts.get("threshold", 6)
    stride = opts.get("stride", 1)
    fraction = opts.get("max_difference_fraction", 0.0)
    if any(isinstance(x, bool) or not isinstance(x, int)
           for x in (frames, capture_frame, threshold, stride)):
        raise SuiteError("frames, capture_frame, threshold and stride must be integers")
    if not (frames >= 2 and 1 <= capture_frame < frames and 0 <= threshold <= 765 and stride >= 1):
        raise SuiteError("invalid frame budget or comparison tolerance")
    if isinstance(fraction, bool) or not isinstance(fraction, (float, int)) or not 0 <= fraction <= 1:
        raise SuiteError("max_difference_fraction must be between 0 and 1")
    selected_views = data.get("views", ["lit", "health", "roughness", "metalness", "f0"])
    if (not isinstance(selected_views, list) or not selected_views or
        any(not isinstance(v, str) or v not in VIEWS for v in selected_views) or
        len(selected_views) != len(set(selected_views))):
        raise SuiteError("views must be unique supported view names")
    data["defaults"] = {"frames": frames, "capture_frame": capture_frame,
                        "threshold": threshold, "stride": stride,
                        "max_difference_fraction": fraction}
    data["views"] = selected_views
    return data


def _choice(names, option, category):
    if not option:
        return list(names)
    values = option.split(",")
    if not values or len(set(values)) != len(values) or any(v not in names for v in values):
        raise SuiteError(f"unknown or duplicate {category} selector: {option}")
    return values


def plans(suite, environment=None, material=None, view=None):
    env_names = _choice(suite["environments"], environment, "environment") if environment else [
        n for n, e in suite["environments"].items() if not e.get("external", False)]
    mat_names = _choice(suite["materials"], material, "material")
    view_names = _choice(suite["views"], view, "view")
    result = []
    for e in env_names:
        env = suite["environments"][e]
        for m in mat_names:
            camera = suite["materials"][m]["camera"]
            for v in view_names:
                pins = dict(env.get("pins", {}))
                if VIEWS[v] is not None:
                    pins["TOURMALINE_DEBUGVIEW"] = VIEWS[v]
                elif "TOURMALINE_DEBUGVIEW" in pins:
                    raise SuiteError("lit view cannot inherit a debug-view pin")
                name = f"{e}/{m}/{v}"
                identity = {"name": name, "source": env["source"],
                            "camera": camera, "pins": pins,
                            "app_args": env.get("app_args", []),
                            "defaults": suite["defaults"]}
                signature = hashlib.sha256(json.dumps(identity, sort_keys=True,
                    separators=(",", ":")).encode("utf-8")).hexdigest()
                result.append({**identity, "signature": signature,
                               "external": env.get("external", False)})
    return result


def capture_command(case, exe, out_dir, baseline=None, gpu_tag=None, overwrite=False):
    source = case["source"]
    cmd = [sys.executable, str(CAPTURE), "--exe", str(exe),
           "--output", str(out_dir), "--frames", str(case["defaults"]["frames"]),
           "--capture-frame", str(case["defaults"]["capture_frame"]),
           "--threshold", str(case["defaults"]["threshold"]),
           "--stride", str(case["defaults"]["stride"]),
           "--camera-pos", _vec(case["camera"]["position"], "camera.position"),
           "--camera-fwd", _vec(case["camera"]["forward"], "camera.forward"), "--json"]
    for key in ("pak", "project"):
        if key in source:
            cmd += [f"--{key}", source[key]]
    if "scene" in source:
        cmd += ["--scene", source["scene"]]
    for key, value in sorted(case["pins"].items()):
        cmd += ["--env", f"{key}={value}"]
    for arg in case["app_args"]:
        cmd += ["--app-arg", arg]
    if baseline:
        cmd += ["--baseline", str(baseline)]
    if gpu_tag:
        cmd += ["--gpu-tag", gpu_tag]
    if overwrite:
        cmd.append("--overwrite")
    return cmd


def _baseline(case, base):
    folder = base / case["name"]
    meta = folder / "case.json"
    try:
        previous = json.loads(meta.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SuiteError(f"missing or bad baseline metadata {meta}: {exc}") from exc
    if previous.get("signature") != case["signature"]:
        raise SuiteError(f"incompatible baseline case: {case['name']}")
    manifest = folder / "capture.json"
    if not manifest.is_file():
        raise SuiteError(f"missing baseline capture: {manifest}")
    return manifest


def contact_sheet(output, results, filename="capture.png"):
    """Best-effort thumbnail index; full-resolution captures remain canonical."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    captures = [(r["name"], output / r["directory"] / filename) for r in results]
    captures = [(name, path) for name, path in captures if path.is_file()]
    if not captures:
        return
    columns, thumb, label = 4, 224, 35
    rows = (len(captures) + columns - 1) // columns
    image = Image.new("RGB", (columns * thumb, rows * (thumb + label)), (30, 30, 30))
    draw = ImageDraw.Draw(image)
    for index, (name, path) in enumerate(captures):
        with Image.open(path) as src:
            tile = src.convert("RGB")
            tile.thumbnail((thumb, thumb))
        x = (index % columns) * thumb
        y = (index // columns) * (thumb + label)
        image.paste(tile, (x + (thumb - tile.width) // 2, y + (thumb - tile.height) // 2))
        draw.text((x + 4, y + thumb + 3), name[:36], fill=(235, 235, 235))
    image.save(output / "contact-sheet.png")


def run(args, suite, cases):
    output = args.output.resolve()
    baseline_root = args.baseline_dir.resolve() if args.baseline_dir else None
    if baseline_root and (output == baseline_root or baseline_root in output.parents or
                          output in baseline_root.parents):
        raise SuiteError("output and baseline directories must be separate trees")
    if baseline_root and not args.gpu_tag:
        raise SuiteError("baseline comparison requires --gpu-tag (GPU + driver)")
    if not CAPTURE.is_file():
        raise SuiteError("missing tools/tourcapture.py")
    # Every baseline is checked before any Vulkan work begins.
    baselines = {c["name"]: _baseline(c, baseline_root) for c in cases} if baseline_root else {}
    if args.dry_run:
        return {"schema_version": 1, "status": "planned", "cases": [
            {"name": c["name"], "command": capture_command(
                c, args.exe, output / c["name"], baselines.get(c["name"]),
                args.gpu_tag, args.overwrite)} for c in cases]}
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for case in cases:
        folder = output / case["name"]
        command = capture_command(case, args.exe, folder, baselines.get(case["name"]),
                                  args.gpu_tag, args.overwrite)
        proc = subprocess.run(command, capture_output=True, text=True, check=False)
        try:
            record = json.loads(proc.stdout)
        except ValueError:
            record = {"status": "error", "message": (proc.stderr or proc.stdout).strip()[:1000]}
        fraction = record.get("comparison", {}).get("difference_fraction")
        # tourcapture exits 1 for ANY difference; a suite tolerance can
        # admit a bounded fraction only after a successful comparison.
        accepted = (proc.returncode == 0 or (proc.returncode == 1 and
                    record.get("status") == "different" and
                    isinstance(fraction, (float, int)) and
                    fraction <= case["defaults"]["max_difference_fraction"]))
        status = ("passed" if baseline_root else "captured") if accepted else "failed"
        results.append({"name": case["name"], "status": status,
                        "capture_status": record.get("status"), "exit_code": proc.returncode,
                        "difference_fraction": fraction, "directory": case["name"],
                        "message": record.get("message", "")})
        if record.get("capture"):
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "case.json").write_text(json.dumps({"signature": case["signature"],
                "name": case["name"], "source": case["source"]}, indent=2) + "\n",
                encoding="utf-8")
        if proc.returncode not in (0, 1):
            print(f"material_lab: {case['name']}: {record.get('message', proc.stderr.strip())}",
                  file=sys.stderr)
        print(f"{status.upper():8s} {case['name']}", file=sys.stderr)
        if not accepted and args.fail_fast:
            break
    failed = sum(r["status"] == "failed" for r in results)
    report = {"schema_version": 1, "status": "failed" if failed else (
                  "passed" if baseline_root else "captured"),
              "case_count": len(results), "failed": failed, "cases": results}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf-8")
    contact_sheet(output, results)
    return report


def reference_command(case, exe, project, out_dir, resolution=256,
                      spp=64, bounces=3, exposure=0.001):
    """Run the engine's CPU reference backend on the matching fixture."""
    env, mat, view = case["name"].split("/")
    if view != "lit" or case.get("external"):
        raise SuiteError("CPU reference requires a bundled lit fixture")
    source = Path(project) / "content" / "models" / env / (env + ".refscene.json")
    camera = case["camera"]
    xyz = lambda values: ",".join(str(v) for v in values)
    return [str(exe), "--scene", str(source), "--output",
            str(out_dir / "reference.png"), "--camera-pos", xyz(camera["position"]),
            "--camera-fwd", xyz(camera["forward"]),
            "--width", str(resolution), "--height", str(resolution),
            "--spp", str(spp), "--bounces", str(bounces),
            "--exposure", str(exposure), "--seed", "8230"]


def run_reference(args, cases):
    if args.resolution < 16 or args.resolution > 4096 or not 1 <= args.spp <= 4096:
        raise SuiteError("reference resolution/spp out of range")
    if not 0 <= args.bounces <= 16 or not 0 < args.exposure <= 1:
        raise SuiteError("reference bounces/exposure out of range")
    output = args.output.resolve()
    commands = []
    for case in cases:
        command = reference_command(case, args.exe, args.project,
                                    output / case["name"],
                                    args.resolution, args.spp,
                                    args.bounces, args.exposure)
        if not args.dry_run and not Path(command[2]).is_file():
            raise SuiteError("reference fixture missing: " + command[2] +
                             " (run material_lab_fixtures.py)")
        commands.append((case, command))
    if args.dry_run:
        return {"schema_version": 1, "status": "planned", "cases": [
            {"name": c["name"], "command": cmd} for c, cmd in commands]}
    results = []
    for case, command in commands:
        folder = output / case["name"]
        folder.mkdir(parents=True, exist_ok=True)
        proc = subprocess.run(command, capture_output=True, text=True, check=False)
        good = proc.returncode == 0 and (folder / "reference.png").is_file()
        status = "captured" if good else "failed"
        if good:
            (folder / "reference.json").write_text(json.dumps({
                "case_signature": case["signature"], "command": command,
                "source": command[2], "backend": "Embree",
                "reference_model": "engine_GI_metallic_roughness",
                "exposure": args.exposure, "spp": args.spp,
                "bounces": args.bounces}, indent=2) + "\n", encoding="utf-8")
        results.append({"name": case["name"], "status": status,
                        "directory": case["name"], "exit_code": proc.returncode,
                        "message": (proc.stderr or proc.stdout).strip()[:1000]})
        print(f"{status.upper():8s} reference {case['name']}", file=sys.stderr)
        if not good and args.fail_fast:
            break
    failed = sum(r["status"] == "failed" for r in results)
    report = {"schema_version": 1, "status": "failed" if failed else "captured",
              "case_count": len(results), "failed": failed, "cases": results}
    output.mkdir(parents=True, exist_ok=True)
    (output / "reference-report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    contact_sheet(output, results, filename="reference.png")
    return report


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--suite", type=Path, default=DEFAULT_SUITE)
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="list available materials, environments and views")
    for name in ("plan", "run", "reference"):
        q = sub.add_parser(name, help="plan or execute a deterministic capture matrix")
        q.add_argument("--environment", help="comma-separated IDs (default: bundled)")
        q.add_argument("--material", help="comma-separated material IDs")
        q.add_argument("--view", help="comma-separated view IDs")
        q.add_argument("--json", action="store_true")
        if name in ("run", "reference"):
            q.add_argument("--exe", type=Path, required=True)
            q.add_argument("--output", type=Path, required=True)
            q.add_argument("--fail-fast", action="store_true")
            q.add_argument("--dry-run", action="store_true")
        if name == "run":
            q.add_argument("--baseline-dir", type=Path)
            q.add_argument("--gpu-tag")
            q.add_argument("--overwrite", action="store_true")
        if name == "reference":
            q.add_argument("--project", type=Path, required=True,
                           help="generated source project root from material_lab_fixtures.py")
            q.add_argument("--resolution", type=int, default=256)
            q.add_argument("--spp", type=int, default=64)
            q.add_argument("--bounces", type=int, default=3)
            q.add_argument("--exposure", type=float, default=0.001)
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        suite = load_suite(args.suite)
        if args.command == "list":
            print(json.dumps({"materials": list(suite["materials"]),
                "environments": {k: "external cooked IBL" if v.get("external") else "bundled fixture"
                                 for k, v in suite["environments"].items()},
                "views": suite["views"]}, indent=2))
            return 0
        cases = plans(suite, args.environment, args.material,
                      "lit" if args.command == "reference" else args.view)
        if not cases:
            raise SuiteError("selection is empty")
        if args.command == "plan":
            data = {"case_count": len(cases), "cases": cases}
        elif args.command == "reference":
            data = run_reference(args, cases)
        else:
            data = run(args, suite, cases)
        if args.json or args.command == "plan" or (
                args.command in ("run", "reference") and args.dry_run):
            print(json.dumps(data, indent=2))
        elif args.command in ("run", "reference"):
            report = "reference-report.json" if args.command == "reference" else "report.json"
            print(f"material_lab: {data['status']}, {data['case_count']} cases, "
                  f"{data['failed']} failed. Report: {args.output / report}")
        return 1 if data.get("status") == "failed" else 0
    except (SuiteError, OSError) as exc:
        print(f"material_lab: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
