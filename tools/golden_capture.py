#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

"""Headless golden capture: deterministic screenshot + content hash.

Runs App.exe headless (fixed camera, fixed bench budget) and hashes the
exported screenshot so CI catches pixel regressions in the 14k-line
WorldRenderer core that has no GPU unit tests:

  python tools/golden_capture.py --config out/build/x64-Release \
      --pak content/paks/sponza.pak [--bench 64] [--capture 48] [--update]

Environment pinned per AGENTS.md (SSR on, AA off, manual exposure, frozen
camera). --update writes the new hash to tools/golden_hashes.txt; without
it the run FAILS on mismatch (CI mode). The capture frame must land INSIDE
the bench budget (capture < bench) or auto-exit happens first.

Note: hashes are GPU/driver-sensitive; keep one baseline per (scene, GPU).
A mismatch means "look at the PNG", not "revert blindly".
"""
import argparse
import hashlib
import os
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


def load_hashes() -> dict:
    out = {}
    if HASH_FILE.exists():
        for line in HASH_FILE.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="out/build/x64-Release")
    ap.add_argument("--pak", default="content/paks/sponza.pak")
    ap.add_argument("--bench", type=int, default=64)
    ap.add_argument("--capture", type=int, default=48)
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()

    if not args.capture < args.bench:
        print(f"ERROR: capture ({args.capture}) must be < bench ({args.bench})")
        return 2

    bindir = ROOT / args.config
    exe = bindir / "App.exe"
    if not exe.exists():
        print(f"ERROR: {exe} missing — build the App target first")
        return 2

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

    shots = sorted((bindir / "exports").glob("screenshot_*.png"))
    if not shots:
        print("ERROR: no exports/screenshot_*.png produced")
        return 1
    shot = shots[-1]
    digest = hashlib.sha256(shot.read_bytes()).hexdigest()
    key = f"{Path(args.pak).stem}.bench{args.bench}.cap{args.capture}"
    print(f"{key}: {shot.name} sha256={digest}")

    hashes = load_hashes()
    if args.update:
        hashes[key] = digest
        lines = ["# golden screenshot hashes: <pak-stem>.bench<N>.cap<M>=sha256",
                 "# GPU/driver-sensitive: one baseline per (scene, GPU).",
                 *[f"{k}={v}" for k, v in sorted(hashes.items())]]
        HASH_FILE.write_text("\n".join(lines) + "\n")
        print(f"updated {HASH_FILE}")
        return 0
    if key not in hashes:
        print(f"NO BASELINE for {key} — run with --update to record one")
        return 1
    if hashes[key] != digest:
        print(f"MISMATCH: baseline {hashes[key]}\n"
              f"          actual   {digest}\nInspect {shot} before updating.")
        return 1
    print("MATCH: golden screenshot identical.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
