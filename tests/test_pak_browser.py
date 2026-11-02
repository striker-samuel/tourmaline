#!/usr/bin/env python3
"""Focused browser/manifest regression checks (no Qt or engine build)."""
import importlib.util
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "pak_browser", Path(__file__).resolve().parents[1] / "tools" / "pak_browser.py"
)
browser = importlib.util.module_from_spec(SPEC)
import sys
sys.modules[SPEC.name] = browser
SPEC.loader.exec_module(browser)


def pak_bytes(entries):
    payload = bytearray(32)
    toc = bytearray()
    for slot, (kind, path, data) in enumerate(entries):
        offset = len(payload)
        payload += data
        encoded = path.encode("utf-8")
        toc += struct.pack(
            "<QIIQQIII", browser.fnv1a64(path), kind, 0,
            offset, len(data), slot, zlib.crc32(data), len(encoded)
        ) + encoded
    toc_offset = len(payload)
    struct.pack_into("<IIIQQI", payload, 0, browser.KPAK_MAGIC,
                     0, 0, toc_offset, len(entries), 0)
    return bytes(payload + toc +
                 struct.pack("<QQII", toc_offset, len(entries),
                             zlib.crc32(toc), browser.KPAK_FOOTER_MAGIC))


def ids_blob(scene="main"):
    path = b"world.tour"
    name = scene.encode()
    return (struct.pack("<IIIQBI", 0x52544449, 1, 1, 0x1001, 0, len(path))
            + path + struct.pack("<I", len(name)) + name)


class BrowserTests(unittest.TestCase):
    def test_smp8_and_lazy_crc(self):
        data = pak_bytes([(11, "audio/example.wav", b"RIFFtest"),
                          (15, "#ids", ids_blob())])
        pak = browser.parse_pak("demo.pak", data)
        self.assertTrue(pak.valid, pak.error)
        self.assertTrue(pak.crc_ok)
        self.assertEqual([e.slot for e in pak.entries], [0, 1])
        self.assertTrue(pak.entries[0].vfs_visible)
        self.assertIsNone(pak.entries[0].page_crc_ok)
        self.assertEqual(pak.payload(pak.entries[0])[0], b"RIFFtest")
        self.assertEqual(pak.entries[0].page_crc_ok, True)
        self.assertEqual(browser.package_id_info(pak)["rows"][0]["scene"], "main")

    def test_bad_page_and_toc_are_independent(self):
        original = pak_bytes([(11, "audio/example.wav", b"RIFFtest")])
        corrupt = bytearray(original)
        corrupt[32] ^= 0x01
        pak = browser.parse_pak("bad.pak", bytes(corrupt))
        self.assertTrue(pak.valid, pak.error)
        self.assertFalse(pak.verify_page(pak.entries[0]))
        self.assertTrue(pak.verify_all())
        corrupt[-6] ^= 1
        self.assertFalse(browser.parse_pak("bad.pak", bytes(corrupt)).valid)

    def test_tapp_release_pointer(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            (root / "world.pak").write_bytes(
                pak_bytes([(15, "#ids", ids_blob())]))
            tapp = root / "release.tapp"
            tapp.write_text('appmanifest 2\nentry world.pak main\n'
                            '[paks]\nworld.pak\n')
            self.assertEqual(browser.cli_dump(str(tapp), validate=True), 0)
            m = browser.load_tapp(str(tapp))
            self.assertEqual(m.entry_scene, "main")
            self.assertEqual(m.paks, ["world.pak"])
            tapp.write_text('appmanifest 2\nentry world.pak main\n'
                            '[paks]\n../escape.pak\n')
            self.assertFalse(browser.load_tapp(str(tapp)).valid)

    def test_old_epoch_rejected(self):
        raw = bytearray(pak_bytes([]))
        raw[:4] = b"SMP4"
        self.assertFalse(browser.parse_pak("old.pak", bytes(raw)).valid)


if __name__ == "__main__":
    unittest.main()
