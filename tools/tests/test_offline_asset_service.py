"""CPU-only checks: SMB-style overwrites, revision recovery and local DCC cache."""
from __future__ import annotations

import json
from http.server import ThreadingHTTPServer
import threading
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import urlopen
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.offline_asset_service import Archive, relative_asset_path, handler_for
from tools.install_offline_assets import configure, install_maya_script


class OfflineAssetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.source = base / "mapped-share"
        self.source.mkdir()
        self.archive = base / "protected"
        self.cache = base / "local"
        self.store = Archive(str(self.source), str(self.archive), str(self.cache),
                             settle=0, max_cache_bytes=1024)

    def put(self, name, content):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_overwrite_and_restore_prior_revision_without_touching_share(self):
        obj = self.put("my own structure/robot.obj", b"original obj")
        self.assertEqual(self.store.scan()["captured"], 1)
        obj.write_bytes(b"agent replaced it")
        self.assertEqual(self.store.scan()["captured"], 1)
        self.assertEqual([x["revision"] for x in self.store.versions(
            "my own structure/robot.obj")], [2, 1])
        old = self.store.resolve("my own structure/robot.obj", 1, pin=True)
        self.assertEqual(Path(old["local_path"]).read_bytes(), b"original obj")
        self.assertTrue(old["local_path"].endswith(".obj"))
        self.assertEqual(obj.read_bytes(), b"agent replaced it")
        obj.unlink()
        self.assertEqual(self.store.scan()["missing"], 1)
        self.assertEqual(len(self.store.versions("my own structure/robot.obj")), 2)
        self.assertEqual(Path(self.store.resolve("my own structure/robot.obj", 2)[
            "local_path"]).read_bytes(), b"agent replaced it")

    def test_hash_deduplication(self):
        self.put("models/thing.obj", b"same bytes")
        self.put("textures/thing.png", b"same bytes")
        self.assertEqual(self.store.scan()["captured"], 2)
        self.assertEqual(len([x for x in (self.archive / "blobs").rglob("*")
                              if x.is_file()]), 1)

    def test_same_bytes_do_not_reversion(self):
        path = self.put("rock.obj", b"v1")
        self.store.scan()
        path.write_bytes(b"v1")
        self.assertEqual(self.store.scan()["captured"], 0)
        self.assertEqual(len(self.store.versions("rock.obj")), 1)

    def test_settle_window_defers_capture(self):
        self.store.settle = 999
        self.put("in-progress.fbx", b"partial")
        self.assertEqual(self.store.scan()["pending"], 1)
        self.assertEqual(self.store.assets(), [])

    def test_disconnected_share_keeps_history(self):
        self.put("a.obj", b"v1")
        self.store.scan()
        self.source.rename(self.source.with_name("offline"))
        with self.assertRaises(OSError):
            self.store.scan()
        self.assertEqual(len(self.store.versions("a.obj")), 1)

    def test_restart_with_disconnected_share_can_resolve_history(self):
        self.put("characters/hero.obj", b"version one")
        self.store.scan()
        self.source.rename(self.source.with_name("unmounted"))
        # A new process must still open protected history without an SMB mount.
        reopened = Archive(str(self.source), str(self.archive), str(self.cache),
                           settle=0)
        self.assertEqual(reopened.status()["revisions"], 1)
        resolved = reopened.resolve("characters/hero.obj", 1)
        self.assertEqual(Path(resolved["local_path"]).read_bytes(), b"version one")
        with self.assertRaises(OSError):
            reopened.scan()

    def test_cache_repairs_corruption_and_pins(self):
        self.put("a.obj", b"A" * 30)
        self.put("b.obj", b"B" * 30)
        self.store.scan()
        pinned = Path(self.store.resolve("a.obj", pin=True)["local_path"])
        pinned.write_bytes(b"CORRUPT")
        self.assertEqual(Path(self.store.resolve("a.obj")["local_path"]).read_bytes(),
                         b"A" * 30)
        self.store.max_cache_bytes = 31
        self.store.resolve("b.obj")
        self.assertTrue(pinned.is_file())

    def test_rejects_archive_inside_share_and_traversal(self):
        with self.assertRaises(ValueError):
            Archive(str(self.source), str(self.source / "hidden"), str(self.cache))
        with self.assertRaises(ValueError):
            Archive(str(self.source), str(self.archive), str(self.source / "cache"))
        for name in ("../secret", "/etc/passwd", "C:/x", "sub/../secret", "./a", "a//b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                relative_asset_path(name)

    def test_installer_copies_maya_picker_without_touching_share(self):
        destination = Path(self.tmp.name) / "maya-user" / "scripts"
        installed = install_maya_script(destination)
        self.assertEqual(installed.parent, destination)
        self.assertTrue(installed.is_file())
        self.assertIn("def show_picker(", installed.read_text(encoding="utf-8"))
        self.assertEqual(list(self.source.rglob("*")), [])

    def test_localhost_api_reads_versions_and_resolves_cache(self):
        self.put("artists/hero.obj", b"version-1")
        self.assertEqual(self.store.scan()["captured"], 1)
        httpd = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(self.store))
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        root = "http://127.0.0.1:%d" % httpd.server_port

        def read(endpoint, **params):
            url = root + endpoint
            if params:
                url += "?" + urlencode(params)
            with urlopen(url, timeout=3) as response:
                return json.load(response)

        try:
            self.assertEqual(read("/health")["status"], "ok")
            self.assertEqual(read("/v1/status")["revisions"], 1)
            self.assertEqual(read("/v1/assets")["assets"][0]["path"], "artists/hero.obj")
            self.assertEqual(read("/v1/versions", path="artists/hero.obj")[
                "versions"][0]["revision"], 1)
            r = read("/v1/resolve", path="artists/hero.obj", revision=1, pin=1)
            self.assertEqual(Path(r["local_path"]).read_bytes(), b"version-1")
            with self.assertRaises(HTTPError) as result:
                read("/v1/versions", path="../outside.obj")
            self.assertEqual(result.exception.code, 400)
        finally:
            httpd.shutdown()
            httpd.server_close()
            thread.join(timeout=5)

    def test_setup_roundtrip(self):
        config = configure(str(self.source), str(self.archive), str(self.cache),
                           config=self.source.parent / "config.json", settle=1)
        settings = json.loads(config.read_text(encoding="utf-8"))
        self.assertEqual(settings["schema_version"], 1)
        self.assertEqual(settings["source"], str(self.source.resolve()))
        self.assertEqual(self.store.status()["revisions"], 0)


if __name__ == "__main__":
    unittest.main()
