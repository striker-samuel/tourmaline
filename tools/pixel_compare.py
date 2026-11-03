#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Compare decoded screenshot pixels, optionally producing a PNG difference map.

Requires Pillow (python -m pip install pillow). No Vulkan or Qt required.

  python tools/pixel_compare.py a.png b.png --json
  python tools/pixel_compare.py a.png b.png --threshold 12 --stride 1 --diff-out diff.png
  printf 'a.png\tb.png\n' | python tools/pixel_compare.py --stdin --jsonl

Exit 0 = no threshold-exceeding pixel differences, 1 = pixel differences
found, 2 = input/usage/IO failure, 3 = missing Pillow.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


def compare(reference, candidate, threshold=6, stride=2, diff_out=None, white_min=None):
    from PIL import Image

    with Image.open(reference) as a:
        aa = a.convert("RGB")
    with Image.open(candidate) as b:
        bb = b.convert("RGB")
    if aa.size != bb.size:
        raise ValueError(f"dimension mismatch: {aa.size} vs {bb.size}")
    width, height = aa.size
    ap, bp = aa.load(), bb.load()
    n = differing = total = white = 0
    diff_img = Image.new("RGB", aa.size) if diff_out else None
    dp = diff_img.load() if diff_img else None
    for y in range(0, height, stride):
        for x in range(0, width, stride):
            left, right = ap[x, y], bp[x, y]
            delta = sum(abs(c - d) for c, d in zip(left, right))
            hit = delta > threshold
            differing += hit
            total += delta
            n += 1
            if white_min is not None and all(v >= white_min for v in left):
                white += 1
            if dp is not None:
                dp[x, y] = (255, 0, 0) if hit else tuple(v // 4 for v in left)
    if diff_img is not None:
        diff_img.save(diff_out)
        diff_img.close()
    return {
        "record_type": "pixel_comparison", "reference": str(reference),
        "candidate": str(candidate), "width": width, "height": height,
        "stride": stride, "threshold": threshold, "sampled_pixels": n,
        "differing_pixels": differing, "difference_fraction": differing / n,
        "average_channel_delta": total / n,
        "reference_pixel_sha256": hashlib.sha256(aa.tobytes()).hexdigest(),
        "candidate_pixel_sha256": hashlib.sha256(bb.tobytes()).hexdigest(),
        "equal": differing == 0, "diff_image": str(diff_out) if diff_out else None,
        **({"near_white_pixels": white, "white_min": white_min} if white_min is not None else {}),
    }


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("reference", nargs="?")
    p.add_argument("candidate", nargs="?")
    p.add_argument("--stdin", action="store_true", help="read tab-separated reference/candidate pairs from stdin")
    output = p.add_mutually_exclusive_group()
    output.add_argument("--json", action="store_true")
    output.add_argument("--jsonl", action="store_true")
    p.add_argument("--threshold", type=int, default=6, help="sum of absolute RGB channel deltas (0..765)")
    p.add_argument("--stride", type=int, default=2, help="sample every Nth pixel on both axes")
    p.add_argument("--diff-out", type=Path, help="write change map PNG (one pair only)")
    p.add_argument("--white-min", type=int, help="also count pixels with all channels >= value")
    args = p.parse_args(argv)
    if args.threshold < 0 or args.threshold > 765 or args.stride < 1 or (
        args.white_min is not None and not 0 <= args.white_min <= 255
    ):
        p.error("threshold must be 0..765, stride >= 1, white-min 0..255")
    if args.stdin and (args.reference or args.candidate or args.diff_out):
        p.error("--stdin cannot be combined with positional paths or --diff-out")
    if not args.stdin and not (args.reference and args.candidate):
        p.error("provide both screenshot paths, or use --stdin")
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("pixel_compare: Pillow required; install with pip install pillow", file=sys.stderr)
        return 3
    pairs = []
    if args.stdin:
        for n, line in enumerate(sys.stdin, 1):
            if not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 2 or not all(parts):
                print(f"pixel_compare: stdin line {n}: expected two tab-separated paths", file=sys.stderr)
                return 2
            pairs.append(tuple(parts))
            if len(pairs) > 10000:
                print("pixel_compare: stdin pair limit exceeded", file=sys.stderr)
                return 2
    else:
        pairs.append((args.reference, args.candidate))
    any_diff = False
    for reference, candidate in pairs:
        try:
            result = compare(reference, candidate, args.threshold, args.stride,
                             args.diff_out, args.white_min)
        except (OSError, ValueError) as exc:
            print(f"pixel_compare: {reference} vs {candidate}: {exc}", file=sys.stderr)
            return 2
        any_diff |= not result["equal"]
        if args.json or args.jsonl:
            print(json.dumps(result, sort_keys=True))
        else:
            print(f"{reference} vs {candidate}: {result['differing_pixels']}/"
                  f"{result['sampled_pixels']} differing "
                  f"({100 * result['difference_fraction']:.2f}%), "
                  f"avg delta {result['average_channel_delta']:.2f}")
    return 1 if any_diff else 0


if __name__ == "__main__":
    sys.exit(main())
