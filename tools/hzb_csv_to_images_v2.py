#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

import argparse
import csv
from pathlib import Path

import numpy as np
from PIL import Image


def mip_layout(width: int, height: int, mip_count: int):
    w, h = width, height
    offset = 0
    for level in range(mip_count):
        count = w * h
        yield level, w, h, offset, count
        offset += count
        w = max(w >> 1, 1)
        h = max(h >> 1, 1)


def as_float(s):
    try:
        return float(s.strip())
    except Exception:
        return None


def load_renderdoc_csv(path: Path, column=None):
    with path.open("r", newline="", encoding="utf-8-sig") as f:
        rows = list(csv.reader(f))

    if len(rows) < 2:
        raise RuntimeError("CSV is empty or has no data rows.")

    print("CSV first rows:")
    for r in rows[:5]:
        print(r)

    raw_header = rows[0]
    header = [h.strip() for h in raw_header]
    header_lower = [h.lower() for h in header]

    # Explicit --column can be an integer index or a header name.
    if column is not None:
        try:
            idx = int(column)
        except ValueError:
            wanted = column.strip().lower()
            if wanted not in header_lower:
                raise RuntimeError(
                    f'Column "{column}" not found. Headers: {header}'
                )
            idx = header_lower.index(wanted)
    else:
        # RenderDoc buffer CSVs commonly look like:
        # Element, texels
        # Prefer the actual data member, never the Element/index column.
        preferred = ("texels", "value", "depth", "data")
        idx = None
        for name in preferred:
            if name in header_lower:
                idx = header_lower.index(name)
                break

        if idx is None:
            # If first column is clearly an index, choose the next numeric column.
            first_is_index = (
                len(header_lower) >= 2
                and header_lower[0] in ("element", "index", "idx")
            )
            start = 1 if first_is_index else 0

            best = None
            for c in range(start, max(len(r) for r in rows)):
                vals = 0
                for r in rows[1:]:
                    if c < len(r) and as_float(r[c]) is not None:
                        vals += 1
                if best is None or vals > best[0]:
                    best = (vals, c)

            if best is None or best[0] == 0:
                raise RuntimeError("Could not find a numeric data column.")
            idx = best[1]

    vals = []
    for r in rows[1:]:
        if idx < len(r):
            v = as_float(r[idx])
            if v is not None:
                vals.append(v)

    if not vals:
        raise RuntimeError(f"No float values found in column {idx} ({header[idx]}).")

    arr = np.asarray(vals, dtype=np.float32)
    return arr, idx, header[idx]


def save_raw16(mip: np.ndarray, out: Path):
    # Preserve 0..1 depth with much more precision than 8-bit grayscale.
    x = np.nan_to_num(mip, nan=0.0, posinf=1.0, neginf=0.0)
    x = np.clip(x, 0.0, 1.0)
    u16 = np.round(x * 65535.0).astype(np.uint16)
    Image.fromarray(u16, mode="I;16").save(out)


def save_debug_view(mip: np.ndarray, out: Path):
    """
    Visibility-only image:
      * clear depth 0 stays black
      * nonzero values are stretched using p1..p99.9
    This is NOT the original numerical depth scale.
    """
    x = np.nan_to_num(mip.astype(np.float32), nan=0.0, posinf=0.0, neginf=0.0)
    nz = x[x > 0.0]

    if nz.size == 0:
        y = np.zeros_like(x)
    else:
        lo = float(np.percentile(nz, 1.0))
        hi = float(np.percentile(nz, 99.9))
        if hi <= lo:
            lo = float(nz.min())
            hi = float(nz.max())

        y = np.zeros_like(x)
        mask = x > 0.0
        if hi > lo:
            y[mask] = np.clip((x[mask] - lo) / (hi - lo), 0.0, 1.0)
        else:
            y[mask] = 1.0

    u8 = np.round(y * 255.0).astype(np.uint8)
    Image.fromarray(u8, mode="L").save(out)


def reduce_min_floor(prev: np.ndarray, dst_h: int, dst_w: int):
    """
    Match the posted HLSL exactly:
      dst dimensions are floor-halved (>> 1)
      each dst pixel reads src (2x,2y)..(+1,+1)
    For odd source dimensions, the final unmatched row/column is discarded.
    """
    out = np.empty((dst_h, dst_w), dtype=np.float32)
    src_h, src_w = prev.shape
    for y in range(dst_h):
        by = y * 2
        y1 = min(by + 1, src_h - 1)
        for x in range(dst_w):
            bx = x * 2
            x1 = min(bx + 1, src_w - 1)
            out[y, x] = min(
                prev[by, bx],
                prev[by, x1],
                prev[y1, bx],
                prev[y1, x1],
            )
    return out


def main():
    ap = argparse.ArgumentParser(
        description="Convert RenderDoc packed HZB CSV to images and validate MIN mips."
    )
    ap.add_argument("csv", type=Path)
    ap.add_argument("-o", "--output", type=Path, default=Path("hzb_mips"))
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--mips", type=int, default=11)
    ap.add_argument(
        "--column",
        default=None,
        help='Value column index/name. Usually unnecessary; "texels" is auto-detected.',
    )
    ap.add_argument(
        "--verify-min",
        action="store_true",
        help="Verify each stored mip equals MIN of the previous mip's 2x2 blocks.",
    )
    args = ap.parse_args()

    layout = list(mip_layout(args.width, args.height, args.mips))
    expected = sum(count for _, _, _, _, count in layout)

    values, col_idx, col_name = load_renderdoc_csv(args.csv, args.column)

    print()
    print(f'Using CSV column {col_idx}: "{col_name}"')
    print(f"Loaded:   {values.size:,} floats")
    print(f"Expected: {expected:,} floats")
    print(f"Bytes:    {expected * 4:,}")

    if values.size < expected:
        raise RuntimeError(
            f"Not enough HZB values: got {values.size:,}, need {expected:,}."
        )
    if values.size > expected:
        print(f"WARNING: ignoring {values.size - expected:,} extra values.")
        values = values[:expected]

    args.output.mkdir(parents=True, exist_ok=True)

    mips = []
    print("\nMip stats:")
    for level, w, h, off, count in layout:
        mip = values[off:off + count].reshape(h, w)
        mips.append(mip)

        nz = mip[mip > 0.0]
        zero_pct = 100.0 * (1.0 - (nz.size / mip.size))

        if nz.size:
            stats = (
                f"min={mip.min():.10g} max={mip.max():.10g} "
                f"zero={zero_pct:6.2f}% "
                f"nz_min={nz.min():.10g} "
                f"nz_p50={np.percentile(nz, 50):.10g} "
                f"nz_max={nz.max():.10g}"
            )
        else:
            stats = f"min=0 max=0 zero=100.00%"

        print(
            f"mip {level:2d} {w:4d}x{h:<4d} "
            f"float_off={off:9d} byte_off=0x{off*4:08X}  {stats}"
        )

        stem = f"mip_{level:02d}_{w}x{h}"
        save_raw16(mip, args.output / f"{stem}_raw16.png")
        save_debug_view(mip, args.output / f"{stem}_view.png")

    if args.verify_min:
        print("\nMIN reduction verification:")
        for level in range(1, len(mips)):
            actual = mips[level]
            expected_mip = reduce_min_floor(
                mips[level - 1], actual.shape[0], actual.shape[1]
            )
            diff = np.abs(actual - expected_mip)
            bad = diff > 1e-7
            bad_count = int(np.count_nonzero(bad))

            if bad_count == 0:
                print(f"mip {level:2d}: OK")
            else:
                max_diff = float(diff.max())
                y, x = np.unravel_index(np.argmax(diff), diff.shape)
                print(
                    f"mip {level:2d}: BAD: {bad_count:,}/{actual.size:,} differ, "
                    f"max_diff={max_diff:.10g} at ({x},{y}), "
                    f"stored={actual[y,x]:.10g}, expected={expected_mip[y,x]:.10g}"
                )

    print(f"\nWrote images to: {args.output.resolve()}")
    print("  *_raw16.png = numerical depth mapped 0..1 -> 0..65535")
    print("  *_view.png  = contrast-stretched nonzero values for visual inspection")


if __name__ == "__main__":
    main()
