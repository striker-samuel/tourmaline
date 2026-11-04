#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract embedded Tourmaline tsNp metadata from PNGs without a GPU or Pillow.

Examples:
  python tools/png_metadata.py Frame_0001.png
  python tools/png_metadata.py out/frames --jsonl
  python tools/png_metadata.py out/ --recursive --output metadata.json
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import snapshot_sequence as codec


def parser_factory() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", type=Path, nargs="+",
                        help="PNG files or directories containing PNGs")
    parser.add_argument("-r", "--recursive", action="store_true",
                        help="search directories recursively (default: top-level only)")
    parser.add_argument("--jsonl", action="store_true",
                        help="emit one JSON object per file instead of a JSON array")
    parser.add_argument("-o", "--output", type=Path,
                        help="write JSON/JSONL to a new file instead of stdout")
    return parser


def discover(paths: list[Path], recursive: bool) -> list[Path]:
    """Return sorted, unique PNG paths; reject empty inputs and symlink files."""
    found = []
    for path in paths:
        if path.is_symlink():
            raise codec.SnapshotError(f"refusing symlink: {path}")
        if path.is_file():
            if path.suffix.lower() != ".png":
                raise codec.SnapshotError(f"expected PNG file: {path}")
            found.append(path)
        elif path.is_dir():
            matches = sorted(path.rglob("*") if recursive else path.iterdir())
            found.extend(p for p in matches if p.suffix.lower() == ".png" and
                         (p.is_file() or p.is_symlink()))
        else:
            raise codec.SnapshotError(f"file or directory not found: {path}")
    unique = sorted(set(p.absolute() for p in found))
    if not unique:
        raise codec.SnapshotError("no PNG files found")
    return unique


def extract(paths: list[Path], recursive: bool = False) -> list[dict]:
    records = []
    for path in discover(paths, recursive):
        # Reuse the snapshot codec: validates signature, PNG chunk CRCs,
        # unique tsNp marker, JSON format/version and IEND. Does not
        # reconstruct state deltas or decode pixel data.
        metadata, _ = codec.read_snapshot(path)
        records.append({"path": str(path), "metadata": metadata})
    return records


def main(argv=None) -> int:
    args = parser_factory().parse_args(argv)
    try:
        # Validate all inputs before emitting anything; malformed PNGs fail
        # the whole invocation instead of silently producing partial results.
        records = extract(args.paths, args.recursive)
        if args.jsonl:
            data = "".join(json.dumps(row, ensure_ascii=False, sort_keys=True,
                                      allow_nan=False) + "\n" for row in records)
        else:
            value = records[0] if len(records) == 1 else records
            data = json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True,
                              allow_nan=False) + "\n"
        if args.output is None:
            sys.stdout.write(data)
        else:
            # Do not overwrite existing files; leave no partial output on
            # invalid input. Parent directory must already exist.
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(data)
        return 0
    except (codec.SnapshotError, OSError, ValueError, MemoryError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
