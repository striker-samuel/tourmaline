#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Portable keyframe/delta and corrupt-PNG regression tests (no GPU)."""
import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import snapshot_sequence as snapshots


def png_chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))


def write_png(path, record):
    raw = bytes([0x11, 0x22, 0x33, 0x44])
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    payload = snapshots.MARKER + snapshots.canonical(record)
    png = (snapshots.PNG_SIGNATURE + png_chunk(b"IHDR", ihdr) +
           png_chunk(b"IDAT", zlib.compress(b"\x00" + raw)) +
           png_chunk(snapshots.CHUNK, payload) + png_chunk(b"IEND", b""))
    path.write_bytes(png)
    return payload


class SnapshotSequenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = {
            "scope": "engine-scene-graph-v1",
            "nodes": {"0:0": {"name": "Root", "position_f32": [0, 0, 0]}},
            "rng": {"seed": 42, "streams": {"loot": {"state": 8, "increment": 3}}},
        }
        self.make_frames()

    def make_frames(self):
        previous = None
        before = None
        self.paths = []
        for index in range(1, 4):
            state = json.loads(json.dumps(self.state))
            state["nodes"]["0:0"]["position_f32"][0] = index
            key = index in (1, 3)
            rec = {
                "format": "tourmaline.snapshot", "version": 1,
                "sequence_id": "unit-test", "frame": index, "tick": index - 1,
                "dt_ns": 16666667, "kind": "key" if key else "delta",
                "previous_payload_fnv64": previous,
                "state_fnv64": snapshots.fnv64(snapshots.canonical(state)),
                "pixels_fnv64": snapshots.fnv64(bytes([0x11,0x22,0x33,0x44])),
                "events": ([{"type": 3, "key": 26}] if index == 1 else []),
            }
            if key:
                rec["provenance"] = {"rng_seed": 42, "code_identity": "test"}
                rec["state"] = state
            else:
                rec["previous_frame"] = index - 1
                rec["patch"] = [
                    {"op": "replace", "path": "/nodes/0:0/position_f32/0",
                     "value": index}
                ]
            filename = self.root / f"Frame_{index:04d}.png"
            payload = write_png(filename, rec)
            previous = snapshots.fnv64(payload)
            before = state
            self.paths.append(filename)

    def test_reconstruct_and_verify(self):
        frames = snapshots.reconstruct(self.root)
        self.assertEqual(len(frames), 3)
        self.assertEqual(frames[1][1]["nodes"]["0:0"]["position_f32"][0], 2)
        self.assertEqual(frames[2][1]["nodes"]["0:0"]["position_f32"][0], 3)
        with contextlib.redirect_stdout(io.StringIO()) as output:
            rc = snapshots.main(["verify", str(self.root), "--json"])
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(output.getvalue())["keyframes"], [1, 3])

    def test_extract_and_compare(self):
        destination = self.root / "world.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(snapshots.main([
                "extract", str(self.root), "--frame", "2", "--output", str(destination)
            ]), 0)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(snapshots.main([
                "compare", str(self.root), "--frame", "2",
                "--candidate", str(destination)
            ]), 0)
        wrong = json.loads(destination.read_text())
        wrong["rng"]["seed"] = 10
        destination.write_text(json.dumps(wrong))
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(snapshots.main([
                "compare", str(self.root), "--frame", "2",
                "--candidate", str(destination), "--json"
            ]), 2)
        self.assertEqual(json.loads(output.getvalue())["first_difference"], "/rng/seed")

    def test_convert_input_tape(self):
        path = self.root / "replay.tinp"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(snapshots.main([
                "input-tape", str(self.root), "--output", str(path)
            ]), 0)
        tape = path.read_bytes()
        self.assertEqual(tape[:8], b"TOURINP1")
        self.assertEqual(struct.unpack_from("<II", tape, 8), (1, 3))
        self.assertEqual(struct.unpack_from("<QQI", tape, 16), (0, 16666667, 1))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(snapshots.main([
                "input-tape", str(self.root), "--output", str(path)
            ]), 1)

    def test_fail_on_corrupted_chunk(self):
        png = bytearray(self.paths[1].read_bytes())
        position = png.find(b"tsNp")
        self.assertNotEqual(position, -1)
        png[position + 10] ^= 1
        self.paths[1].write_bytes(png)
        with self.assertRaisesRegex(snapshots.SnapshotError, "CRC mismatch"):
            snapshots.reconstruct(self.root)

    def test_fail_on_missing_frame(self):
        self.paths[1].unlink()
        with self.assertRaisesRegex(snapshots.SnapshotError, "missing or duplicate"):
            snapshots.reconstruct(self.root)

    def test_fail_on_broken_chain(self):
        rec, _ = snapshots.read_snapshot(self.paths[1])
        rec["previous_payload_fnv64"] = "0" * 16
        write_png(self.paths[1], rec)
        with self.assertRaisesRegex(snapshots.SnapshotError, "hash chain"):
            snapshots.reconstruct(self.root)

    def test_fail_on_bad_world_hash(self):
        rec, _ = snapshots.read_snapshot(self.paths[0])
        rec["state_fnv64"] = "bad"
        write_png(self.paths[0], rec)
        with self.assertRaisesRegex(snapshots.SnapshotError, "checksum"):
            snapshots.reconstruct(self.root)

    def test_bad_json_patch_is_rejected(self):
        with self.assertRaises(snapshots.SnapshotError):
            snapshots.apply_patch({"a": 1}, [
                {"op": "remove", "path": "/missing"}
            ])
        with self.assertRaises(snapshots.SnapshotError):
            snapshots.apply_patch({"a": 1}, [
                {"op": "replace", "path": "/a", "value": 2},
                {"op": "execute", "path": "/a"}
            ])

    def test_pixels_when_pillow_available(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not available")
        self.assertEqual(len(snapshots.reconstruct(self.root, pixels=True)), 3)


if __name__ == "__main__":
    unittest.main()
