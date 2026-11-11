#!/usr/bin/env python3
"""Explicitly vendor a reviewed, pinned smvk snapshot into external/smvk.

Never run on its own as part of a regular configure/build. This experimental
script deliberately refuses to vendor a smvk revision without CMakeLists.txt.
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
VENDOR = ROOT / "external" / "smvk"
DEFAULT_REPOSITORY = "https://github.com/striker-samuel/smvk.git"

def run(*args: str, cwd: pathlib.Path | None = None) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rev", required=True, help="reviewed smvk commit SHA")
    parser.add_argument("--repo", default=DEFAULT_REPOSITORY)
    parser.add_argument("--apply", action="store_true",
                        help="replace external/smvk (otherwise dry-run)")
    args = parser.parse_args()
    if len(args.rev) != 40 or any(c not in "0123456789abcdefABCDEF" for c in args.rev):
        parser.error("--rev must be a full 40-character commit SHA")
    with tempfile.TemporaryDirectory(prefix="smvk-vendor-") as tmp:
        checkout = pathlib.Path(tmp) / "smvk"
        run("git", "clone", "--quiet", "--no-checkout", args.repo, str(checkout))
        run("git", "checkout", "--quiet", "--detach", args.rev, cwd=checkout)
        resolved = run("git", "rev-parse", "HEAD", cwd=checkout)
        if not (checkout / "CMakeLists.txt").exists():
            raise SystemExit("Refusing to vendor: smvk has no standalone CMakeLists.txt yet")
        print(f"Source: {args.repo} @ {resolved}")
        print(f"Destination: {VENDOR}")
        if not args.apply:
            print("Dry run; pass --apply to replace the vendor tree.")
            return
        if VENDOR.exists():
            shutil.rmtree(VENDOR)
        shutil.copytree(checkout, VENDOR, ignore=shutil.ignore_patterns(".git"))
        (VENDOR / "SMVK_UPSTREAM_COMMIT").write_text(resolved + "\n", encoding="utf-8")
        print("Vendored snapshot updated. Review git diff and validate builds.")

if __name__ == "__main__":
    main()
