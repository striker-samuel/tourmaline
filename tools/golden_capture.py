#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

"""Headless golden capture: deterministic screenshot + PIXEL hash.

Runs App.exe headless (fixed camera, fixed bench budget) and hashes the
exported screenshot so THE GOLDEN GATE catches pixel regressions in the
14k-line WorldRenderer core that has no GPU unit tests (a LOCAL/dev-GPU
gate; the CI workflows compile + run CPU tests — they neither invoke
this script nor own a GPU):

  python tools/golden_capture.py --config out/build/x64-Release \
      --pak content/paks/sponza.pak [--bench 64] [--capture 48] [--update]

Environment pinned per AGENTS.md (SSR on, AA off, manual exposure,
frozen camera). --update writes the new hash to tools/golden_hashes.txt;
without it the run FAILS on mismatch. The capture frame must land INSIDE
the bench budget (capture < bench) or auto-exit happens first.

Hardness rules (guards against a green run claiming more than it did):

  1. FRESH CAPTURE REQUIRED. The run compares the screenshot App
     produced THIS run: the tool diffs exports/screenshot_*.png names
     before vs after and fails when the run produced none (an old
     screenshot is never re-hashed silently).
  2. DECODED-PIXEL HASHING REQUIRED. Pillow decodes and hashes raw
     decoded pixels (the engine's PNG writer emits nondeterministic –
     but valid – deflate streams: two same-frame exports byte-differ
     and pixel-match). No Pillow = LOUD FAILURE, never a silent
     raw-byte-hash fallback (that "hash" would flake against the
     threaded writer and defeat the whole gate).
  3. GPU+DRIVER KEYED BASELINES. Baselines live under keys that name
     the pak, the bench/capture point, AND the GPU (name + driver
     version slug via Win32_VideoController) — hashes are
     GPU/driver-sensitive, so the lookup never invents a baseline for
     a different adapter. Legacy keys (no gpu slug) are detected and
     refused with an explicit re-record hint.

On MISMATCH the failing capture is COPIED to
<config>/exports/golden_fail_<key>.png (the original stays untouched)
so the run diff can be diffed by hand before anyone touches the
baseline.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HASH_FILE = Path(__file__).with_name("golden_hashes.txt")

DEFAULT_ENV = {
    "TOURMALINE_SSR": "1",
    "TOURMALINE_AA": "off",
    "TOURMALINE_AUTO_EXPOSURE": "0",
    "TOURMALINE_CAM_POS": "24.26,1.82,-9.52",
    "TOURMALINE_CAM_FWD": "-0.947,-0.022,-0.321",
}

EXIT_NO_NEW_CAPTURE = 4
EXIT_PILLOW_MISSING = 3
EXIT_LEGACY_BASELINE = 5


def load_hashes() -> dict:
    out = {}
    if HASH_FILE.exists():
        for line in HASH_FILE.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def gpu_slug() -> str:
    """A slug naming THIS machine's 3D adapter + driver (the baseline
    key's GPU half). Windows-only detection via Win32_VideoController;
    anything else falls back to an explicit unknown (with a LOUD note —
    cross-machine baseline sharing for a pixel gate is a mistake)."""
    if sys.platform == "win32":
        try:
            ps = ("Get-CimInstance Win32_VideoController | "
                  "Sort-Object -Property adaptertype -Descending | "
                  "Select-Object -First 1 Name,DriverVersion | "
                  "ConvertTo-Json -Compress")
            r = subprocess.run(
                ["powershell", "-NoProfile", "-Command", ps],
                capture_output=True, text=True, timeout=30)
            if r.returncode == 0 and r.stdout.strip():
                import json
                info = json.loads(r.stdout)
                name = str(info.get("Name", "")).strip()
                drv = str(info.get("DriverVersion", "")).strip()
                if name:
                    raw = f"{name}_drv{drv}" if drv else name
                    return re.sub(r"[^A-Za-z0-9]+", "_", raw).strip("_")[:80]
        except Exception as e:  # detection is best-effort, never fatal
            print(f"NOTE: GPU detection failed ({e}) - using gpu_unknown")
    else:
        print("NOTE: GPU identity is Windows-only tooling - using"
              " gpu_unknown (do NOT share baselines across machines)")
    return "gpu_unknown"


def pixel_digest(path: Path) -> str:
    """sha256 over DECODED pixels. Pillow is REQUIRED (no raw-byte
    fallback: the engine's PNG writer is threaded + nondeterministic
    at the file level, so a byte hash is a coin flip between runs and
    a useless gate)."""
    try:
        from PIL import Image
    except ImportError:
        print("ERROR: Pillow is required for the pixel gate (decoding is"
              " the only deterministic hash - the PNG file bytes flake"
              " against the threaded writer). Install it:\n"
              "    pip install pillow")
        raise SystemExit(EXIT_PILLOW_MISSING)
    with Image.open(path) as im:
        return hashlib.sha256(im.tobytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="out/build/x64-Release")
    ap.add_argument("--pak", default="content/paks/sponza.pak")
    ap.add_argument("--bench", type=int, default=64)
    ap.add_argument("--capture", type=int, default=48)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--gpu-slug", default=None,
                    help="override the auto-detected GPU/driver key slug")
    args = ap.parse_args()

    if not args.capture < args.bench:
        print(f"ERROR: capture ({args.capture}) must be < bench ({args.bench})")
        return 2

    bindir = ROOT / args.config
    exe = bindir / "App.exe"
    if not exe.exists():
        print(f"ERROR: {exe} missing - build the App target first")
        return 2
    exports = bindir / "exports"
    before = {p.name for p in exports.glob("screenshot_*.png")} \
        if exports.exists() else set()

    env = dict(os.environ)
    env.update(DEFAULT_ENV)
    env["TOURMALINE_CAPTURE"] = str(args.capture)
    cmd = [str(exe), "--pak", args.pak, "--disable_control", "--bench",
           str(args.bench)]
    print(f"$ {' '.join(cmd)} (cwd={bindir})")
    r = subprocess.run(cmd, cwd=bindir, env=env, timeout=900)
    if r.returncode != 0:
        print(f"ERROR: App exited {r.returncode}")
        return 1

    after = {p.name for p in exports.glob("screenshot_*.png")} \
        if exports.exists() else set()
    new = sorted(after - before)
    new_files = [exports / n for n in new if (exports / n).is_file()]
    if not new_files:
        print(f"ERROR: this run produced NO new exports/screenshot_*.png "
              f"({len(before)} stale captures exist) - refusing to hash "
              f"a screenshot the run did not create (fresh-capture rule)")
        return EXIT_NO_NEW_CAPTURE
    newest = new_files[-1]  # npm-sorted + mtime-sorted agree on the stamp;
    # newest wins; warn when the run somehow exported more than one
    if len(new_files) > 1:
        print(f"NOTE: {len(new_files)} new captures this run - using the "
              f"newest: {newest.name}")

    digest = pixel_digest(newest)
    slug = args.gpu_slug or gpu_slug()
    key = (f"{Path(args.pak).stem}.bench{args.bench}"
           f".cap{args.capture}.{slug}")
    legacy_key = f"{Path(args.pak).stem}.bench{args.bench}.cap{args.capture}"
    print(f"{key}: {newest.name} pixel-sha256={digest}")

    hashes = load_hashes()
    if args.update:
        hashes[key] = digest
        lines = [
            "# golden screenshot hashes:",
            "#   <pak-stem>.bench<N>.cap<M>.<gpu-slug>=sha256",
            "# sha256 over DECODED PIXELS (the PNG writer's deflate",
            "# stream is nondeterministic; file bytes flake - Pillow",
            "# decodes, the tool REQUIRES it and never falls back).",
            "# GPU/driver-sensitive: one baseline per (scene, GPU, driver)",
            "# - the gpu-slug is Win32_VideoController Name+DriverVersion;",
            "# pre-GPU-key baselines (no '.gpu_' suffix) are rejected at",
            "# lookup; re-record those with --update.",
            *[f"{k}={v}" for k, v in sorted(hashes.items())]]
        HASH_FILE.write_text("\n".join(lines) + "\n")
        print(f"updated {HASH_FILE}")
        return 0

    if key not in hashes:
        if legacy_key in hashes:
            print(f"LEGACY BASELINE for {legacy_key} has no GPU identity "
                  f"(pre-gpu-key format) - refusing to use it (a hash is "
                  f"only valid for the adapter that recorded it). "
                  f"Re-record: run with --update on THIS machine.")
            return EXIT_LEGACY_BASELINE
        print(f"NO BASELINE for {key} - run with --update to record one")
        return 1

    if hashes[key] != digest:
        fail_copy = exports / f"golden_fail_{key}.png"
        try:
            shutil.copy2(newest, fail_copy)
        except OSError as e:
            fail_copy = newest
            print(f"NOTE: could not copy the failing capture ({e})")
        print(f"MISMATCH: baseline {hashes[key]}\n"
              f"          actual   {digest}\n"
              f"Capture retained: {fail_copy}\n"
              f"Inspect it (compare_screenshots.ps1 <baseline-image>"
              f" {fail_copy}) before touching the baseline.")
        return 1
    print("MATCH: golden screenshot identical.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
