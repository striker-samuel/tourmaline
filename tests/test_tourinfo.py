#!/usr/bin/env python3
"""CPU-only contract tests for tourinfo; reuse test_pak_browser's SMP8 fixtures."""
import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import tourinfo as cli
import pak_browser as pak


def sample_pak():
    payload = bytearray(32)
    toc = bytearray()
    for slot, (kind, path, data) in enumerate([
        (4, "textures/normal.ktx", b"texture"),
        (11, "audio/click.wav", b"RIFF"),
        (15, "#ids", struct.pack("<IIIQBI", 0x52544449, 1, 1, 0x1001, 3, 19)
         + b"textures/normal.ktx" + struct.pack("<I", 0))]):
        import zlib
        off = len(payload)
        payload.extend(data)
        b = path.encode("utf-8")
        toc += struct.pack("<QIIQQIII", pak.fnv1a64(path), kind, 0, off,
                           len(data), slot, zlib.crc32(data), len(b)) + b
    import zlib
    pos = len(payload)
    struct.pack_into("<IIIQQI", payload, 0, pak.KPAK_MAGIC, 0, 0, pos, 3, 0)
    return payload + toc + struct.pack("<QQII", pos, 3, zlib.crc32(toc),
                                       pak.KPAK_FOOTER_MAGIC)


class TourInfoTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.pak = self.root / "world.pak"
        self.pak.write_bytes(sample_pak())
        self.manifest = self.root / "game.tapp"
        self.manifest.write_text("appmanifest 2\nentry world.pak world\n[paks]\nworld.pak\n")

    def run_cli(self, *args, stdin=None):
        out = io.StringIO()
        with redirect_stdout(out):
            if stdin is not None:
                with patch.object(sys, "stdin", io.StringIO(stdin)):
                    status = cli.main(list(args))
            else:
                status = cli.main(list(args))
        return status, out.getvalue()

    def test_summary_and_deep_validate(self):
        code, output = self.run_cli(str(self.pak), "--json")
        self.assertEqual(code, 0)
        report = json.loads(output)
        self.assertEqual(report["pak_count"], 1)
        self.assertEqual(report["entry_count"], 3)
        code, output = self.run_cli("validate", str(self.pak), "--deep", "--json")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output)["verified_pages"], 3)

    def test_glob_regex_and_field_projection(self):
        code, output = self.run_cli("entries", str(self.pak), "-g",
                                    "**/*normal*", "--jsonl", "--fields", "path,kind")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output), {"path": "textures/normal.ktx",
                                              "kind": "texture"})
        code, output = self.run_cli("entries", str(self.pak), "--regex",
                                    "NORMAL", "-i", "--paths")
        self.assertEqual(code, 0)
        self.assertEqual(output.strip(), "textures/normal.ktx")
        self.assertTrue(cli.glob_regex("**/normal*").fullmatch("normal.ktx"))
        self.assertFalse(cli.glob_regex("textures/*").fullmatch("textures/sub/file"))

    def test_pipe_entries_to_inspect(self):
        code, rows = self.run_cli("entries", str(self.pak), "--jsonl",
                                  "-g", "**/*normal*")
        self.assertEqual(code, 0)
        code, detail = self.run_cli("inspect", "--stdin", "--jsonl", stdin=rows)
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(detail)["path"], "textures/normal.ktx")

    def test_identities_and_error_codes(self):
        code, output = self.run_cli("assets", str(self.pak), "--json")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output)["items"][0]["id"], "0x1001")
        code, _ = self.run_cli("entries", str(self.pak), "--regex", "[")
        self.assertEqual(code, 2)

    def test_corrupted_page_detected(self):
        data = bytearray(self.pak.read_bytes())
        data[32] ^= 1
        self.pak.write_bytes(data)
        code, output = self.run_cli("validate", str(self.pak), "--deep", "--json")
        self.assertEqual(code, 1)
        self.assertFalse(json.loads(output)["valid"])


if __name__ == "__main__":
    unittest.main()
