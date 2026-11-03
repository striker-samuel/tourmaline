#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Inspect, reconstruct and compare self-describing Tourmaline PNG sequences.

The PNGs contain a versioned private ancillary chunk (tsNp) with a JSON
keyframe or RFC6902 state delta. No game code is executed by this tool.
The PNG's image remains viewable by ordinary PNG decoders.

  python tools/snapshot_sequence.py verify out/snapshots --pixels
  python tools/snapshot_sequence.py inspect out/snapshots
  python tools/snapshot_sequence.py extract out/snapshots --frame 61 --output state.json
  python tools/snapshot_sequence.py compare out/snapshots --frame 61 --candidate agent_state.json
  python tools/snapshot_sequence.py input-tape out/snapshots --output replay.tinp
"""
from __future__ import annotations

import argparse
import copy
import json
import math
import re
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHUNK = b"tsNp"  # ancillary, private, reserved-bit valid, safe-to-copy
MARKER = b"TSNP1\x00"
MAX_PNG = 512 * 1024 * 1024
MAX_CHUNK = 64 * 1024 * 1024
MAX_FRAMES = 1_000_000
MAX_STATE = 64 * 1024 * 1024
NAME = re.compile(r"Frame_([0-9]{4,9})\.png\Z")


class SnapshotError(ValueError):
    pass


def _fail(message):
    raise SnapshotError(message)


def fnv64(data: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def canonical(obj) -> bytes:
    return json.dumps(obj, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def _json(data: bytes):
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                _fail(f"duplicate key: {key}")
            result[key] = value
        return result

    def constant(value):
        _fail(f"invalid JSON constant: {value}")

    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=pairs,
                          parse_constant=constant)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise SnapshotError(f"invalid JSON payload: {exc}") from exc


def read_snapshot(path: Path):
    if path.is_symlink() or not path.is_file():
        _fail(f"not a regular PNG: {path}")
    size = path.stat().st_size
    if not 8 <= size <= MAX_PNG:
        _fail(f"PNG exceeds size limits: {path}")
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        _fail(f"invalid PNG signature: {path}")
    offset = 8
    snapshot = None
    ihdr = False
    idat = False
    ended = False
    while offset < len(data):
        if offset + 12 > len(data):
            _fail(f"truncated PNG chunk: {path}")
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        if length > MAX_PNG or offset + 12 + length > len(data):
            _fail(f"invalid PNG chunk length: {path}")
        payload = data[offset + 8:offset + 8 + length]
        given_crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        actual_crc = zlib.crc32(kind + payload) & 0xFFFFFFFF
        if given_crc != actual_crc:
            _fail(f"PNG CRC mismatch: {path} {kind!r}")
        if not ihdr and kind != b"IHDR":
            _fail(f"PNG missing first IHDR: {path}")
        if kind == b"IHDR":
            if ihdr or length != 13:
                _fail(f"invalid IHDR: {path}")
            ihdr = True
        if kind == b"IDAT":
            idat = True
        if kind == CHUNK:
            if snapshot is not None or length > MAX_CHUNK or not payload.startswith(MARKER):
                _fail(f"invalid or duplicated snapshot chunk: {path}")
            snapshot = _json(payload[len(MARKER):])
        if kind == b"IEND":
            if length or not idat:
                _fail(f"invalid IEND: {path}")
            ended = True
            if offset + 12 != len(data):
                _fail(f"trailing bytes after PNG: {path}")
        offset += 12 + length
        if ended:
            break
    if not ended or snapshot is None:
        _fail(f"PNG missing snapshot chunk or terminator: {path}")
    if not isinstance(snapshot, dict) or snapshot.get("format") != "tourmaline.snapshot" or (
            type(snapshot.get("version")) is not int or snapshot["version"] != 1):
        _fail(f"unsupported snapshot format: {path}")
    return snapshot, payload_of_chunk(data)


def payload_of_chunk(png: bytes) -> bytes:
    """Return the exact encoded snapshot chunk, including marker, for chain hashing."""
    offset = 8
    while offset + 12 <= len(png):
        length = struct.unpack_from(">I", png, offset)[0]
        kind = png[offset + 4:offset + 8]
        if kind == CHUNK:
            return png[offset + 8:offset + 8 + length]
        offset += length + 12
    _fail("missing snapshot payload")


def _pointer(container, pointer: str):
    if not isinstance(pointer, str) or not pointer.startswith("/"):
        _fail("invalid JSON pointer")
    raw = pointer[1:].split("/")
    if any(re.search(r"~(?![01])", part) for part in raw):
        _fail("invalid JSON pointer escaping")
    segments = [part.replace("~1", "/").replace("~0", "~") for part in raw]
    current = container
    for part in segments[:-1]:
        if isinstance(current, dict):
            if part not in current:
                _fail(f"missing JSON pointer component: {part}")
            current = current[part]
        elif isinstance(current, list):
            if not part.isascii() or not part.isdecimal() or (
                    len(part) > 1 and part.startswith("0")):
                _fail("invalid JSON pointer array index")
            idx = int(part)
            if idx >= len(current):
                _fail("JSON pointer array index out of bounds")
            current = current[idx]
        else:
            _fail("JSON pointer descends through a scalar")
    return current, segments[-1]


def apply_patch(state, patch):
    if not isinstance(patch, list) or len(patch) > 1_000_000:
        _fail("snapshot patch must be a bounded array")
    state = copy.deepcopy(state)
    for item in patch:
        if not isinstance(item, dict) or item.get("op") not in ("add", "replace", "remove"):
            _fail("unsupported JSON patch operation")
        parent, key = _pointer(state, item.get("path"))
        op = item["op"]
        if op != "remove" and "value" not in item:
            _fail("patch operation missing value")
        if isinstance(parent, dict):
            if op in ("remove", "replace") and key not in parent:
                _fail("patch target not found")
            if op == "remove":
                del parent[key]
            else:
                parent[key] = copy.deepcopy(item["value"])
        elif isinstance(parent, list):
            if key == "-" and op == "add":
                idx = len(parent)
            elif (isinstance(key, str) and key.isascii() and key.isdecimal() and
                  (len(key) == 1 or key[0] != "0")):
                idx = int(key)
            else:
                _fail("invalid array index")
            if idx > len(parent) or (op != "add" and idx >= len(parent)):
                _fail("array patch index out of bounds")
            if op == "add":
                parent.insert(idx, copy.deepcopy(item["value"]))
            elif op == "replace":
                parent[idx] = copy.deepcopy(item["value"])
            else:
                del parent[idx]
        else:
            _fail("JSON patch target is not a container")
    if len(canonical(state)) > MAX_STATE:
        _fail("reconstructed state exceeds limit")
    return state


def sequence_files(folder: Path):
    if not folder.is_dir():
        _fail(f"snapshot directory not found: {folder}")
    files = []
    for p in folder.iterdir():
        match = NAME.fullmatch(p.name)
        if match:
            files.append((int(match.group(1)), p))
    files.sort(key=lambda item: item[0])
    if not files or len(files) > MAX_FRAMES:
        _fail("no PNG sequence or too many frames")
    for index, (number, _) in enumerate(files, 1):
        if number != index:
            _fail(f"sequence has missing or duplicate frame: expected {index}, got {number}")
    return [item[1] for item in files]


def reconstruct(folder: Path, *, pixels: bool = False):
    state = None
    sequence_id = None
    previous_payload_hash = None
    previous_tick = -1
    full = []
    if pixels:
        try:
            from PIL import Image
        except ImportError as exc:
            raise SnapshotError("--pixels requires Pillow") from exc
    for index, path in enumerate(sequence_files(folder), 1):
        record, payload = read_snapshot(path)
        if record.get("frame") != index or type(record.get("frame")) is not int:
            _fail(f"wrong frame index in {path}")
        if index == 1:
            sequence_id = record.get("sequence_id")
            if not isinstance(sequence_id, str) or not sequence_id:
                _fail("missing sequence identity")
        elif record.get("sequence_id") != sequence_id:
            _fail(f"sequence identity drift at {path}")
        if record.get("previous_payload_fnv64") != previous_payload_hash:
            _fail(f"broken metadata hash chain at {path}")
        tick = record.get("tick")
        dt = record.get("dt_ns")
        if type(tick) is not int or tick <= previous_tick or (
                index != 1 and tick != previous_tick + 1):
            _fail(f"non-contiguous simulation tick at {path}")
        if type(dt) is not int or not 0 < dt <= 10_000_000_000:
            _fail(f"invalid timestep at {path}")
        previous_tick = tick
        keyframe = record.get("kind") == "key"
        if keyframe:
            if not isinstance(record.get("state"), dict) or (
                    not isinstance(record.get("provenance"), dict)):
                _fail(f"missing keyframe state/provenance at {path}")
            state = copy.deepcopy(record["state"])
        elif record.get("kind") == "delta" and state is not None:
            if record.get("previous_frame") != index - 1:
                _fail(f"invalid delta predecessor at {path}")
            state = apply_patch(state, record.get("patch"))
        else:
            _fail(f"missing initial keyframe or unknown record kind at {path}")
        if fnv64(canonical(state)) != record.get("state_fnv64"):
            _fail(f"world state checksum mismatch at {path}")
        events = record.get("events")
        if not isinstance(events, list) or len(events) > 4096:
            _fail(f"invalid input events at {path}")
        for event in events:
            if not isinstance(event, dict) or type(event.get("type")) is not int or (
                    not 3 <= event["type"] <= 14):
                _fail(f"invalid input event at {path}")
        if pixels:
            from PIL import Image
            try:
                with Image.open(path) as im:
                    rgba = im.convert("RGBA").tobytes()
            except (OSError, ValueError) as exc:
                raise SnapshotError(f"cannot decode PNG pixels: {path}: {exc}") from exc
            if fnv64(rgba) != record.get("pixels_fnv64"):
                _fail(f"image mismatch at {path}")
        previous_payload_hash = fnv64(payload)
        full.append((record, copy.deepcopy(state)))
    return full


def _first_difference(expected, actual, pointer=""):
    if type(expected) is not type(actual):
        return pointer or "/"
    if isinstance(expected, dict):
        for key in sorted(expected.keys() | actual.keys()):
            next_pointer = pointer + "/" + key.replace("~", "~0").replace("/", "~1")
            if key not in expected or key not in actual:
                return next_pointer
            diff = _first_difference(expected[key], actual[key], next_pointer)
            if diff is not None:
                return diff
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            return pointer or "/"
        for index, (left, right) in enumerate(zip(expected, actual)):
            diff = _first_difference(left, right, pointer + "/" + str(index))
            if diff is not None:
                return diff
    elif expected != actual:
        return pointer or "/"
    return None


def to_input_tape(frames, destination: Path):
    if destination.exists():
        _fail(f"refusing to overwrite {destination}")
    if frames[0][0]["tick"] != 0:
        _fail("input tape requires first snapshot tick 0; restore a keyframe for partial captures")
    if len(frames) > MAX_FRAMES:
        _fail("input tape too long")
    out = bytearray(b"TOURINP1")
    out += struct.pack("<II", 1, len(frames))
    for index, (record, _) in enumerate(frames):
        events = record["events"]
        out += struct.pack("<QQI", index, record["dt_ns"], len(events))
        for e in events:
            text = e.get("text", "")
            if not isinstance(text, str):
                _fail("event text must be a string")
            blob = text.encode("utf-8")
            if len(blob) > 4096:
                _fail("event text too long")
            if any(not math.isfinite(float(e.get(k, 0.0))) for k in
                   ("axis", "x", "y", "xrel", "yrel")):
                _fail("input event has non-finite float")
            try:
                out += struct.pack("<B6I5fBI", e["type"],
                    *(int(e.get(k, 1 if k == "mouseButton" else 0)) for k in ("key", "mouseButton", "gamepadButton",
                                                "gamepadAxis", "gamepadId", "mods")),
                    *(float(e.get(k, 0.0)) for k in ("axis", "x", "y", "xrel", "yrel")),
                    1 if e.get("repeat", False) else 0, len(blob))
            except (KeyError, ValueError, OverflowError, struct.error) as exc:
                raise SnapshotError(f"invalid tape event: {exc}") from exc
            out.extend(blob)
    if len(out) > 128 * 1024 * 1024:
        _fail("input tape exceeds 128 MiB limit")
    temp = destination.with_name(destination.name + ".tmp")
    if temp.exists():
        _fail(f"refusing to overwrite {temp}")
    temp.write_bytes(out)
    temp.rename(destination)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inspect", "verify", "extract", "compare",
                                           "compare-sequence", "input-tape"))
    parser.add_argument("folder", type=Path)
    parser.add_argument("--pixels", action="store_true", help="verify decoded RGBA pixels (needs Pillow)")
    parser.add_argument("--frame", type=int, help="one-based frame index")
    parser.add_argument("--candidate", type=Path,
                        help="compare: agent-produced JSON state; compare-sequence: agent PNG folder")
    parser.add_argument("--output", type=Path, help="destination JSON or input tape")
    parser.add_argument("--json", action="store_true", help="machine-readable JSON result")
    args = parser.parse_args(argv)
    try:
        frames = reconstruct(args.folder, pixels=args.pixels)
        if args.action == "compare-sequence":
            if not args.candidate:
                _fail("compare-sequence requires --candidate DIRECTORY")
            candidate = reconstruct(args.candidate, pixels=args.pixels)
            if len(candidate) != len(frames):
                print(json.dumps({"status": "different", "type": "frame_count",
                                  "baseline": len(frames), "candidate": len(candidate)}))
                return 2
            for index, ((reference_record, reference), (observed_record, observed)) in enumerate(
                    zip(frames, candidate), 1):
                if reference_record["tick"] != observed_record["tick"]:
                    print(json.dumps({"status": "different", "type": "tick", "frame": index}))
                    return 2
                diff = _first_difference(reference, observed)
                if diff is not None:
                    print(json.dumps({"status": "different", "type": "state",
                                      "frame": index, "first_difference": diff}))
                    return 2
                if reference_record["pixels_fnv64"] != observed_record["pixels_fnv64"]:
                    print(json.dumps({"status": "different", "type": "pixels", "frame": index}))
                    return 2
        if args.action == "extract":
            if not args.frame or not args.output or not 1 <= args.frame <= len(frames):
                _fail("extract requires --frame N and --output FILE")
            if args.output.exists():
                _fail("refusing to overwrite existing output")
            args.output.write_bytes(canonical(frames[args.frame - 1][1]) + b"\n")
        if args.action == "input-tape":
            if not args.output:
                _fail("input-tape requires --output FILE")
            to_input_tape(frames, args.output)
        if args.action == "compare":
            if not args.frame or not args.candidate or not 1 <= args.frame <= len(frames):
                _fail("compare requires --frame N and --candidate JSON")
            actual = _json(args.candidate.read_bytes())
            expected = frames[args.frame - 1][1]
            diff = _first_difference(expected, actual)
            if diff is not None:
                report = {"status": "different", "frame": args.frame, "first_difference": diff}
                print(json.dumps(report) if args.json else f"DIFFERENT: {report}")
                return 2
        report = {"status": "ok", "frames": len(frames),
                  "sequence_id": frames[0][0]["sequence_id"],
                  "first_tick": frames[0][0]["tick"],
                  "last_tick": frames[-1][0]["tick"],
                  "keyframes": [i + 1 for i, (r, _) in enumerate(frames) if r["kind"] == "key"],
                  "pixels_checked": args.pixels}
        if args.action == "inspect":
            report["provenance"] = frames[0][0]["provenance"]
            report["state_scope"] = frames[0][1].get("scope", "unknown")
            report["rng_seed"] = frames[0][1].get("rng", {}).get("seed")
            report["first_state_fnv64"] = frames[0][0]["state_fnv64"]
            report["last_state_fnv64"] = frames[-1][0]["state_fnv64"]
        print(json.dumps(report) if args.json else f"OK: {report}")
        return 0
    except (SnapshotError, OSError, OverflowError, MemoryError) as exc:
        report = {"status": "error", "message": str(exc)}
        print(json.dumps(report) if args.json else f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
