#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CPU-only tests for Tourmaline's standalone PNG metadata extraction CLI."""
import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import png_metadata as cli
import snapshot_sequence as codec


def chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))


def write_png(path, *, corrupt=False, include_metadata=True):
    record = {"format": "tourmaline.snapshot", "version": 1,
              "frame": 1, "tick": 0, "kind": "key",
              "events": [{"type": 3, "key": 26}],
              "provenance": {"code_identity": "demo"},
              "state": {"rng": {"seed": 7}}}
    payload = (codec.MARKER + codec.canonical(record))
    image = (codec.PNG_SIGNATURE +
             chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
             chunk(b"IDAT", zlib.compress(b"\x00\x11\x22\x33\x44")) +
             (chunk(codec.CHUNK, payload) if include_metadata else b"") +
             chunk(b"IEND", b""))
    if corrupt:
        image = image.replace(b"TSNP1", b"TSNP2")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(image)
    return record


def invoke(*args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = cli.main(list(map(str, args)))
    return status, out.getvalue(), err.getvalue()


class PngMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.png = self.root / "Frame_0001.png"
        self.record = write_png(self.png)

    def test_one_png_returns_original_record(self):
        rc, out, err = invoke(self.png)
        self.assertEqual((rc, err), (0, ""))
        result = json.loads(out)
        self.assertEqual(result["metadata"], self.record)
        self.assertEqual(result["path"], str(self.png))

    def test_directory_and_jsonl(self):
        second = self.root / "Frame_0002.png"
        write_png(second)
        rc, out, _ = invoke(self.root, "--jsonl")
        self.assertEqual(rc, 0)
        rows = [json.loads(line) for line in out.splitlines()]
        self.assertEqual([r["path"] for r in rows], [str(self.png), str(second)])

    def test_recursion_and_deduplication(self):
        nested = self.root / "nested" / "Frame_0003.png"
        write_png(nested)
        self.assertEqual(len(cli.extract([self.root])), 1)
        self.assertEqual(len(cli.extract([self.root, self.png], recursive=True)), 2)

    def test_output_file_and_refuse_overwrite(self):
        output = self.root / "metadata.json"
        rc, out, err = invoke(self.png, "-o", output)
        self.assertEqual((rc, out, err), (0, "", ""))
        self.assertEqual(json.loads(output.read_text())["metadata"], self.record)
        rc, _, err = invoke(self.png, "-o", output)
        self.assertEqual(rc, 1)
        self.assertIn("ERROR:", err)

    def test_missing_metadata_and_corruption_fail_without_stdout(self):
        for name, kwargs in (("missing.png", {"include_metadata": False}),
                             ("corrupt.png", {"corrupt": True})):
            path = self.root / name
            write_png(path, **kwargs)
            rc, out, err = invoke(path, "--jsonl")
            self.assertEqual(rc, 1)
            self.assertEqual(out, "")
            self.assertIn("ERROR:", err)

    def test_batch_failure_never_writes_partial_jsonl(self):
        bad = self.root / "bad.png"
        bad.write_bytes(b"not a PNG")
        rc, out, err = invoke(self.root, "--jsonl")
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn("ERROR:", err)

    def test_symlink_rejected_when_supported(self):
        link = self.root / "link.png"
        try:
            link.symlink_to(self.png)
        except (OSError, NotImplementedError):
            self.skipTest("symlinks not supported")
        rc, out, err = invoke(link)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn("symlink", err)


if __name__ == "__main__":
    unittest.main()
