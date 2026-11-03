#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""tourinfo: headless, pipe-friendly Tourmaline SMP8 and .tapp inspection.

Uses pak_browser's existing format readers and decoders. No Vulkan, Qt, or
third-party dependencies for metadata queries. LZ4 is optional for decoding
compressed asset payloads (it is NOT needed to validate stored page CRCs).

Examples:
  python tools/tourinfo.py game.tapp
  python tools/tourinfo.py assets game.tapp -g '**/*normal*' --jsonl
  python tools/tourinfo.py entries game.tapp --regex 'terrain|foliage' -i --paths
  python tools/tourinfo.py inspect game.tapp --path '#ids' --json
  python tools/tourinfo.py entries game.tapp --jsonl | python tools/tourinfo.py inspect --stdin --jsonl
  python tools/tourinfo.py validate game.tapp --deep --json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter
from pathlib import Path

import pak_browser as pak

SCHEMA_VERSION = 1
COMMANDS = ("summary", "paks", "assets", "entries", "inspect", "validate")
FIELDS = {
    "assets": {"record_type", "id", "kind", "path", "scene", "pak", "pak_name", "mount_index"},
    "entries": {"record_type", "pak", "pak_name", "mount_index", "path", "kind", "kind_id",
                "slot", "size_stored", "offset", "codec", "content_crc", "vfs_visible"},
    "paks": {"record_type", "pak", "pak_name", "mount_index", "size_bytes", "entry_count",
             "toc_crc_ok", "toc_offset", "identity_count"},
}


class InspectionError(Exception):
    """Invalid package, invalid query, or unusable input."""


def glob_regex(pattern: str) -> re.Pattern[str]:
    """Path-aware glob: '*' does not cross '/', while '**/' matches zero or more dirs."""
    result = ["^"]
    i = 0
    while i < len(pattern):
        ch = pattern[i]
        if ch == "*":
            if i + 1 < len(pattern) and pattern[i + 1] == "*":
                i += 2
                if i < len(pattern) and pattern[i] == "/":
                    result.append("(?:.*/)?")
                    i += 1
                else:
                    result.append(".*")
                continue
            result.append("[^/]*")
        elif ch == "?":
            result.append("[^/]")
        elif ch == "[":
            end = pattern.find("]", i + 1)
            if end < 0 or end == i + 1:
                result.append(r"\[")
            else:
                chars = pattern[i + 1:end]
                negative = chars.startswith("!")
                if negative:
                    chars = chars[1:]
                if not chars:
                    raise InspectionError("empty glob character class")
                result.append("[" + ("^" if negative else "") +
                              re.escape(chars).replace(r"\-", "-") + "]")
                i = end
        else:
            result.append(re.escape(ch))
        i += 1
    result.append("$")
    try:
        return re.compile("".join(result))
    except re.error as exc:
        raise InspectionError(f"invalid glob {pattern!r}: {exc}") from exc


def filters(args: argparse.Namespace):
    flags = re.IGNORECASE if args.ignore_case else 0
    try:
        globs = [re.compile(glob_regex(g).pattern, flags) for g in args.glob]
        regexes = [re.compile(r, flags) for r in args.regex]
    except re.error as exc:
        raise InspectionError(f"invalid regex: {exc}") from exc
    kinds = {v.casefold() for s in args.kind for v in s.split(",")}
    def accepted(row):
        path = str(row.get("path", "")).replace("\\", "/")
        kind = str(row.get("kind", "")).casefold()
        return (
            (not kinds or kind in kinds)
            and (not globs or any(g.fullmatch(path) for g in globs))
            and (not regexes or any(r.search(path) for r in regexes))
        )
    return accepted


def read_source(path: str):
    p = Path(path)
    if p.suffix.lower() == ".tapp":
        try:
            manifest = pak.load_tapp(str(p))
        except (OSError, UnicodeError, ValueError) as exc:
            raise InspectionError(f"{p}: cannot open manifest: {exc}") from exc
        if manifest.error:
            raise InspectionError(f"{p}: {manifest.error}")
        names = manifest.paks
        paths = [pak.tapp_pak_path(manifest, n) for n in names]
    elif p.suffix.lower() == ".pak":
        manifest = None
        names, paths = [p.name], [p]
    else:
        raise InspectionError("expected an SMP8 .pak or appmanifest v2 .tapp")
    mounts = []
    for name, physical in zip(names, paths):
        image = pak._read_pak(str(physical))
        if image.error:
            raise InspectionError(f"{physical}: {image.error}")
        mounts.append((name, image))
    return manifest, mounts


def read_ids(image, required=False):
    if not any(e.path == "#ids" and e.kind == 15 for e in image.entries):
        if required:
            raise InspectionError(f"{image.source}: missing #ids identity table")
        return []
    result = pak.package_id_info(image)
    if "error" in result:
        raise InspectionError(f"{image.source}: #ids: {result['error']}")
    return result["rows"]


def list_rows(command, mounts, required_ids=False):
    rows = []
    for idx, (name, image) in enumerate(mounts):
        base = {"pak": image.source, "pak_name": name, "mount_index": idx}
        if command == "paks":
            ids = read_ids(image, required=required_ids)
            rows.append({"record_type": "pak", **base, "size_bytes": image.size,
                         "entry_count": len(image.entries), "toc_offset": image.toc_offset,
                         "toc_crc_ok": image.crc_ok, "identity_count": len(ids)})
        elif command == "assets":
            for ident in read_ids(image, required=required_ids):
                rows.append({"record_type": "asset", **base, **ident})
        elif command == "entries":
            for e in image.entries:
                rows.append({"record_type": "entry", **base, "path": e.path,
                             "kind": e.kind_name.lower(), "kind_id": e.kind,
                             "slot": e.slot, "size_stored": e.size, "offset": e.offset,
                             "codec": "lz4" if e.compressed else "raw",
                             "content_crc": f"0x{e.content_crc:08X}",
                             "vfs_visible": e.vfs_visible})
    return rows


def summary(source, manifest, mounts, sections=None):
    counts = Counter()
    size = 0
    identities = 0
    for _, image in mounts:
        counts.update(e.kind_name.lower() for e in image.entries)
        size += image.size
        identities += len(read_ids(image, required=manifest is not None))
    result = {
        "schema_version": SCHEMA_VERSION,
        "source": source,
        "format": "appmanifest2" if manifest else "smp8",
        "entry": {"pak": manifest.entry_pak, "scene": manifest.entry_scene} if manifest else None,
        "pak_count": len(mounts),
        "entry_count": sum(len(image.entries) for _, image in mounts),
        "asset_count": identities,
        "size_bytes": size,
        "kinds": dict(sorted(counts.items())),
        "paks": list_rows("paks", mounts, required_ids=manifest is not None),
    }
    if sections:
        allowed = {"entry", "paks", "assets", "kinds"}
        unknown = set(sections) - allowed
        if unknown:
            raise InspectionError(f"unknown summary section(s): {', '.join(sorted(unknown))}")
        keep = {"schema_version", "source", "format"}
        mapping = {"entry": {"entry"}, "paks": {"paks", "pak_count", "size_bytes"},
                   "assets": {"asset_count", "entry_count"}, "kinds": {"kinds"}}
        for section in sections:
            keep.update(mapping[section])
        result = {k: v for k, v in result.items() if k in keep}
    return result


def decoded_entry(image, entry):
    data, note = image.payload(entry)
    if note and not note.startswith("lz4:"):
        raise InspectionError(f"{image.source}:{entry.path}: {note}")
    decoders = {1: pak.decode_scene_summary, 2: pak.decode_mesh,
                4: pak.decode_texture, 6: pak.decode_gi, 15: pak.decode_ids}
    decoded = decoders[entry.kind](data) if entry.kind in decoders else None
    return {"record_type": "inspection", "pak": image.source, "path": entry.path,
            "slot": entry.slot, "kind": entry.kind_name.lower(),
            "size_stored": entry.size, "size_decoded": len(data),
            "codec": "lz4" if entry.compressed else "raw",
            "crc_ok": True, "decoded": decoded}


def inspect_records(args, mounts):
    records = []
    cache = {img.source: img for _, img in mounts}
    if args.stdin:
        for number, line in enumerate(sys.stdin, 1):
            if not line.strip():
                continue
            if number > 100000:
                raise InspectionError("--stdin limited to 100000 input lines")
            try:
                src = json.loads(line)
            except ValueError as exc:
                raise InspectionError(f"stdin line {number}: invalid JSON: {exc}") from exc
            if not isinstance(src, dict) or not src.get("pak"):
                raise InspectionError(f"stdin line {number}: expected JSON object with 'pak'")
            physical = str(src["pak"])
            if physical not in cache:
                _, opened = read_source(physical)
                if len(opened) != 1:
                    raise InspectionError(f"stdin line {number}: 'pak' must reference one .pak")
                cache[physical] = opened[0][1]
            image = cache[physical]
            if "slot" in src:
                matches = [e for e in image.entries if e.slot == src["slot"]]
            elif "path" in src:
                matches = [e for e in image.entries if e.path == src["path"]]
            elif "id" in src:
                ids = [i for i in read_ids(image) if i["id"].casefold() == str(src["id"]).casefold()]
                matches = [e for e in image.entries if any(e.path == i["path"] for i in ids)]
            else:
                raise InspectionError(f"stdin line {number}: requires slot, path, or id")
            if not matches:
                raise InspectionError(f"stdin line {number}: entry not found in {physical}")
            records.extend(decoded_entry(image, e) for e in matches)
        return records
    if args.slot is None and not args.path and not args.id:
        raise InspectionError("inspect requires --path, --slot, --id, or --stdin")
    for _, image in mounts:
        ids = read_ids(image) if args.id else []
        paths = {i["path"] for i in ids if i["id"].casefold() == args.id.casefold()}
        for e in image.entries:
            if ((args.slot is not None and e.slot == args.slot)
                    or (args.path is not None and e.path == args.path)
                    or (args.id and e.path in paths)):
                records.append(decoded_entry(image, e))
    if not records:
        raise InspectionError("no matching stored entry (identity paths need not equal TOC paths)")
    return records


def validate(source, manifest, mounts, deep=False):
    problems = []
    for name, image in mounts:
        if manifest:
            problems.extend(f"{name}: {problem}" for problem in
                            pak._manifest_pak_problems(manifest, name, image, deep))
        elif deep:
            problems.extend(f"{name}: {problem}" for problem in image.verify_all())
    return {"schema_version": SCHEMA_VERSION, "source": source,
            "valid": not problems, "deep": bool(deep),
            "verified_pages": sum(len(img.entries) for _, img in mounts) if deep else 0,
            "problems": problems}


def format_rows(command, rows, args):
    fmt = ("json" if args.json else "jsonl" if args.jsonl else
           "paths" if args.paths else args.format)
    if args.fields:
        names = [field.strip() for field in args.fields.split(",") if field.strip()]
        known = FIELDS.get(command, set())
        if not known and rows:
            known = set(rows[0])
        unknown = set(names) - known
        if unknown:
            raise InspectionError(f"unknown field(s): {', '.join(sorted(unknown))}")
        rows = [{k: v for k, v in row.items() if k in names} for row in rows]
    if fmt == "json":
        print(json.dumps({"schema_version": SCHEMA_VERSION, "command": command,
                          "count": len(rows), "items": rows}, sort_keys=True))
    elif fmt == "jsonl":
        for row in rows:
            print(json.dumps(row, sort_keys=True))
    elif fmt == "paths":
        for row in rows:
            if "path" not in row:
                raise InspectionError("--paths requires records with a path")
            print(row["path"])
    else:
        if not rows:
            print("(no matches)")
            return
        keys = list(rows[0]) if not args.fields else names
        for row in rows:
            print("  ".join(f"{k}={row[k]}" for k in keys if k in row))


def output_object(data, args):
    if args.paths or args.fields:
        raise InspectionError("--paths/--fields only apply to record commands")
    fmt = "json" if args.json else "jsonl" if args.jsonl else args.format
    if fmt in ("json", "jsonl"):
        print(json.dumps(data, sort_keys=True))
    elif fmt == "paths":
        raise InspectionError("--paths is not applicable here")
    else:
        for key, value in data.items():
            if key == "paks":
                print("paks:")
                for item in value:
                    print(f"  [{item['mount_index']}] {item['pak_name']}: "
                          f"{item['entry_count']} entries, {item['identity_count']} assets, "
                          f"{item['size_bytes']} bytes")
            elif key == "kinds":
                print("kinds: " + ", ".join(f"{k}={v}" for k, v in value.items()))
            elif key == "problems":
                for issue in value:
                    print(f"ERROR: {issue}")
            else:
                print(f"{key}: {value}")


def parser():
    p = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=COMMANDS)
    p.add_argument("source", nargs="?", help=".pak or .tapp source (optional with inspect --stdin)")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--json", action="store_true", help="single JSON document")
    mode.add_argument("--jsonl", action="store_true", help="one JSON record per line")
    mode.add_argument("--paths", action="store_true", help="path only, one per line")
    mode.add_argument("--format", choices=("text", "json", "jsonl", "paths"),
                      default="text", help="output format (default: text)")
    p.add_argument("-g", "--glob", action="append", default=[], help="path-aware glob; repeated values OR")
    p.add_argument("--regex", action="append", default=[], help="regex on logical path; repeated values OR")
    p.add_argument("-i", "--ignore-case", action="store_true")
    p.add_argument("--kind", action="append", default=[], help="asset/entry kind; comma-separated or repeated")
    p.add_argument("--fields", help="comma-separated field projection (record commands)")
    p.add_argument("--sort", help="sort records by field name")
    p.add_argument("--desc", action="store_true", help="descending sort")
    p.add_argument("--limit", type=int, help="maximum number of matching records (0 allows none)")
    p.add_argument("--section", help="summary sections: entry,paks,assets,kinds")
    p.add_argument("--path", help="exact stored TOC path to inspect")
    p.add_argument("--slot", type=int, help="exact SMP8 slot to inspect")
    p.add_argument("--id", help="catalog asset ID to locate by its logical path")
    p.add_argument("--stdin", action="store_true", help="inspect JSONL records from standard input")
    p.add_argument("--deep", action="store_true", help="validate every stored page CRC (potentially expensive)")
    return p


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] not in COMMANDS and argv[0] not in ("-h", "--help"):
        argv.insert(0, "summary")
    args = parser().parse_args(argv)
    try:
        if args.limit is not None and args.limit < 0:
            raise InspectionError("--limit must be >= 0")
        if args.stdin and args.command != "inspect":
            raise InspectionError("--stdin is only supported by inspect")
        if args.command != "inspect" and (args.path or args.slot is not None or args.id):
            raise InspectionError("--path, --slot, --id only apply to inspect")
        if not args.source and not (args.command == "inspect" and args.stdin):
            raise InspectionError("a .pak or .tapp source is required")
        manifest, mounts = read_source(args.source) if args.source else (None, [])
        if args.command == "summary":
            sections = args.section.split(",") if args.section else None
            output_object(summary(args.source, manifest, mounts, sections), args)
        elif args.command == "validate":
            report = validate(args.source, manifest, mounts, args.deep)
            output_object(report, args)
            return 0 if report["valid"] else 1
        else:
            if args.command == "inspect":
                rows = inspect_records(args, mounts)
            else:
                rows = list_rows(args.command, mounts, required_ids=manifest is not None)
            accepted = filters(args)
            rows = [row for row in rows if accepted(row)]
            if args.sort:
                if args.sort not in FIELDS.get(args.command, set(rows[0]) if rows else set()):
                    raise InspectionError(f"unknown sort field: {args.sort}")
                rows.sort(key=lambda r: (r.get(args.sort) is None, r.get(args.sort)),
                          reverse=args.desc)
            if args.limit is not None:
                rows = rows[:args.limit]
            format_rows(args.command, rows, args)
    except (InspectionError, OSError, UnicodeError) as exc:
        print(f"tourinfo: {exc}", file=sys.stderr)
        return 2
    except BrokenPipeError:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
