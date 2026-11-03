#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Compare Tourmaline SMP8 .pak or appmanifest v2 .tapp releases.

Logical identities are keyed by persistent #ids asset ID, not filenames.
Physical pages are keyed by mount index, kind and TOC path; CRC, codec,
size and slot changes are reported. Content-addressed pak filename churn
by itself is NOT a change. Mount order IS significant to the runtime.

Examples:
  python tools/tourdiff.py old.tapp new.tapp --json
  python tools/tourdiff.py old.tapp new.tapp --scope assets --jsonl | jq .
  python tools/tourdiff.py old.pak new.pak --scope pages -g 'textures/**'

Exit: 0 no relevant changes, 1 changes found, 2 invalid input.
No Vulkan, Qt or GPU required. Stored page CRCs are compared, not verified.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter

import tourinfo


class DiffError(Exception):
    pass


def _map_identities(mounts, require):
    rows = {}
    for index, (name, image) in enumerate(mounts):
        for item in tourinfo.read_ids(image, required=require):
            key = item["id"].lower()
            if key in rows:
                raise DiffError(f"duplicate identity {key} across paks")
            # The physical pak name may contain a content hash and is NOT
            # part of the identity. Mount index is relevant for resolution.
            rows[key] = {"id": item["id"], "kind": item["kind"],
                         "path": item["path"], "scene": item["scene"],
                         "mount_index": index}
    return rows


def _map_pages(mounts):
    rows = {}
    for index, (_name, image) in enumerate(mounts):
        for item in image.entries:
            key = (index, item.kind, item.path)
            if key in rows:
                raise DiffError(f"duplicate page kind/path in mount {index}: {item.path}")
            rows[key] = {"path": item.path, "kind": item.kind_name.lower(),
                         "kind_id": item.kind, "mount_index": index,
                         "slot": item.slot, "size_stored": item.size,
                         "codec": "lz4" if item.compressed else "raw",
                         "content_crc": f"0x{item.content_crc:08X}"}
    return rows


def load(path, scope):
    manifest, mounts = tourinfo.read_source(path)
    if manifest is not None:
        for name, image in mounts:
            issues = tourinfo.pak._manifest_pak_problems(manifest, name, image, False)
            if issues:
                raise DiffError(f"{path}: {name}: {'; '.join(issues)}")
    identities = _map_identities(mounts, require=manifest is not None) if scope in ("all", "assets") else {}
    pages = _map_pages(mounts) if scope in ("all", "pages") else {}
    entry = ((manifest.entry_pak, manifest.entry_scene) if manifest else None)
    # The entry pak name may be content-addressed. Resolve to ordinal.
    if manifest:
        entry = {"mount_index": manifest.paks.index(manifest.entry_pak),
                 "scene": manifest.entry_scene}
    return {"assets": identities, "pages": pages, "entry": entry}


def _changes(before, after, record_type, show_unchanged):
    result = []
    for key in sorted(set(before) | set(after)):
        old, new = before.get(key), after.get(key)
        status = "added" if old is None else "removed" if new is None else (
            "unchanged" if old == new else "modified")
        if status == "unchanged" and not show_unchanged:
            continue
        item = {"record_type": record_type, "status": status,
                "before": old, "after": new,
                "path": (new or old).get("path", ""),
                "kind": (new or old).get("kind", "")}
        if record_type == "asset":
            item["id"] = (new or old)["id"]
        result.append(item)
    return result


def compare(old, new, scope="all", show_unchanged=False):
    result = []
    if scope in ("all", "assets"):
        result += _changes(old["assets"], new["assets"], "asset", show_unchanged)
        if old["entry"] != new["entry"]:
            result.append({"record_type": "entry", "status": "modified", "path": "",
                           "kind": "scene", "before": old["entry"], "after": new["entry"]})
    if scope in ("all", "pages"):
        result += _changes(old["pages"], new["pages"], "page", show_unchanged)
    return result


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("before", help="older .pak or .tapp")
    p.add_argument("after", help="newer .pak or .tapp")
    p.add_argument("--scope", choices=("all", "assets", "pages"), default="all")
    p.add_argument("-g", "--glob", action="append", default=[], help="path-aware glob (quote it)")
    p.add_argument("--regex", action="append", default=[], help="regex on logical path")
    p.add_argument("-i", "--ignore-case", action="store_true")
    p.add_argument("--kind", action="append", default=[], help="kind name, repeated or comma-separated")
    p.add_argument("--status", action="append", choices=("added", "removed", "modified", "unchanged"),
                   help="filter change status")
    p.add_argument("--include-unchanged", action="store_true")
    p.add_argument("--limit", type=int, help="limit output rows after filtering")
    fmt = p.add_mutually_exclusive_group()
    fmt.add_argument("--json", action="store_true", help="one JSON document")
    fmt.add_argument("--jsonl", action="store_true", help="one diff record per line")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.limit is not None and args.limit < 0:
            raise DiffError("--limit must be >= 0")
        src = load(args.before, args.scope)
        dst = load(args.after, args.scope)
        changed = compare(src, dst, args.scope, show_unchanged=args.include_unchanged)
        accepted = tourinfo.filters(args)
        rows = [r for r in changed if accepted(r) and (not args.status or r["status"] in args.status)]
        # Determine exit from ALL matching changes, never hide a regression
        # just because --limit truncates output.
        has_diff = any(r["status"] != "unchanged" for r in rows)
        counts = dict(sorted(Counter(r["status"] for r in rows).items()))
        shown = rows[:args.limit] if args.limit is not None else rows
        if args.json:
            print(json.dumps({"schema_version": 1, "before": args.before,
                              "after": args.after, "scope": args.scope,
                              "changes": sum(v for k, v in counts.items() if k != "unchanged"),
                              "counts": counts, "items": shown}, sort_keys=True))
        elif args.jsonl:
            for row in shown:
                print(json.dumps(row, sort_keys=True))
        else:
            for row in shown:
                label = row.get("id", row.get("path", ""))
                print(f"{row['status']:9} {row['record_type']:5} {label}"
                      + (f"  [{row['path']}]" if row.get("id") else ""))
            print(f"{sum(v for k, v in counts.items() if k != 'unchanged')} change(s)", file=sys.stderr)
        return 1 if has_diff else 0
    except (tourinfo.InspectionError, DiffError, OSError, ValueError) as exc:
        print(f"tourdiff: {exc}", file=sys.stderr)
        return 2
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
