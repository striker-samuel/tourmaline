#!/usr/bin/env python3
"""CPU-only Tourmaline package comparison regression tests."""
import io
import json
import struct
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tourdiff
import pak_browser


def ids_blob(rows):
    data = bytearray(struct.pack("<III", 0x52544449, 1, len(rows)))
    for aid, kind, path, scene in rows:
        p, s = path.encode(), scene.encode()
        data += struct.pack("<QBI", aid, kind, len(p)) + p
        data += struct.pack("<I", len(s)) + s
    return bytes(data)


def pak_bytes(entries):
    body, toc = bytearray(32), bytearray()
    for slot, (kind, path, payload) in enumerate(entries):
        offset = len(body)
        body.extend(payload)
        p = path.encode()
        toc += struct.pack("<QIIQQIII", pak_browser.fnv1a64(path), kind, 0,
                           offset, len(payload), slot, zlib.crc32(payload), len(p)) + p
    offset = len(body)
    struct.pack_into("<IIIQQI", body, 0, pak_browser.KPAK_MAGIC, 0, 0,
                     offset, len(entries), 0)
    return body + toc + struct.pack("<QQII", offset, len(entries),
                                    zlib.crc32(toc), pak_browser.KPAK_FOOTER_MAGIC)


class DiffTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.left = self.root / "old"
        self.right = self.root / "new"
        self.left.mkdir()
        self.right.mkdir()

    def manifest(self, folder, pak_name, payload=b"same", scene="main"):
        (folder / pak_name).write_bytes(pak_bytes([
            (11, "audio/signal.wav", payload),
            (15, "#ids", ids_blob([(0x1001, 1, "world.tour", scene),
                                   (0x1002, 2, "textures/base.ktx", "")]))]))
        release = folder / "game.tapp"
        release.write_text(f"appmanifest 2\nentry {pak_name} {scene}\n[paks]\n{pak_name}\n")
        return release

    def cli(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = tourdiff.main(list(args))
        return rc, out.getvalue()

    def test_content_address_rename_is_not_change(self):
        a = self.manifest(self.left, "world-aaaa.pak")
        b = self.manifest(self.right, "world-bbbb.pak")
        status, output = self.cli(str(a), str(b), "--json")
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(output)["changes"], 0)

    def test_page_change_does_not_invent_identity_change(self):
        a = self.manifest(self.left, "world-a.pak", b"old")
        b = self.manifest(self.right, "world-b.pak", b"new")
        status, output = self.cli(str(a), str(b), "--json")
        self.assertEqual(status, 1)
        items = json.loads(output)["items"]
        self.assertEqual(len([x for x in items if x["record_type"] == "asset"]), 0)
        self.assertEqual(len([x for x in items if x["record_type"] == "page"]), 1)
        status, output = self.cli(str(a), str(b), "--scope", "assets", "--json")
        self.assertEqual(status, 0)

    def test_glob_and_jsonl_and_limit(self):
        a = self.manifest(self.left, "world-a.pak", b"old")
        b = self.manifest(self.right, "world-b.pak", b"new")
        status, output = self.cli(str(a), str(b), "-g", "audio/**", "--jsonl")
        self.assertEqual(status, 1)
        item = json.loads(output)
        self.assertEqual(item["path"], "audio/signal.wav")
        status, output = self.cli(str(a), str(b), "-g", "textures/**", "--json")
        self.assertEqual(status, 0)
        status, output = self.cli(str(a), str(b), "--limit", "0", "--json")
        self.assertEqual(status, 1)  # truncating presentation doesn't mask differences
        self.assertEqual(json.loads(output)["items"], [])

    def test_identity_and_entry_change(self):
        a = self.manifest(self.left, "world-a.pak", scene="main")
        b = self.manifest(self.right, "world-b.pak", scene="level2")
        status, output = self.cli(str(a), str(b), "--scope", "assets", "--json")
        self.assertEqual(status, 1)
        items = json.loads(output)["items"]
        self.assertEqual({x["record_type"] for x in items}, {"asset", "entry"})

    def test_missing_manifest_pak_is_an_error(self):
        a = self.manifest(self.left, "world-a.pak")
        b = self.manifest(self.right, "world-b.pak")
        (self.right / "world-b.pak").unlink()
        status, _ = self.cli(str(a), str(b), "--json")
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
