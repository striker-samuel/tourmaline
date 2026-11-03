"""Cross-platform tests for stable source identity, explicit products, NAS API and caching."""
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from http.server import ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools" / "maya"))

from tools.offline_asset_service import Archive
from tools.shared_asset_repository import SharedArchive, handler_for_shared
from tools.install_shared_assets import configure as configure_host
from tools.configure_asset_client import configure as configure_client
from tourmaline_asset_cache import AssetClient, load_client_settings


TOKEN = "test-only-long-random-access-token-123456789"


class SharedRepositoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.source = root / "share"
        self.source.mkdir()
        self.archive = root / "history"
        self.cache = root / "server-cache"
        self.workstation_cache = root / "artist-cache"
        self.store = SharedArchive(self.source, self.archive, self.cache, settle=0)

    def put(self, path, data):
        file = self.source / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
        return file

    def test_source_identity_tracks_revisions_without_cooked_ids(self):
        source = self.put("characters/hero.ma", b"working maya revision 1")
        self.assertEqual(self.store.scan()["captured"], 1)
        item = self.store.sources()[0]
        self.assertEqual(item["current_path"], "characters/hero.ma")
        source_id = item["asset_id"]
        self.assertEqual(len(source_id), 36)
        source.write_bytes(b"working maya revision 2")
        self.store.scan()
        self.assertEqual(self.store.sources()[0]["asset_id"], source_id)
        self.assertEqual([r["revision"] for r in self.store.source_revisions(source_id)],
                         [2, 1])
        # Source-version tracking doesn't create/modify cooked project.h,
        # .tourmaline/ assets.db or stable content-ID tables.
        with self.store.connect() as db:
            tables = {r[0] for r in db.execute(
                "SELECT name FROM sqlite_master WHERE type='table'")}
        self.assertNotIn("content_ids", tables)
        self.assertFalse((self.source / "characters" / "hero.ma.meta").exists())

    def test_unambiguous_move_preserves_identity_but_copy_does_not(self):
        source = self.put("old/rock.obj", b"same rock")
        self.store.scan()
        rock_id = self.store.sources()[0]["asset_id"]
        new = self.source / "new" / "rock.obj"
        new.parent.mkdir()
        source.rename(new)
        self.store.scan()
        self.assertEqual(self.store.sources()[0]["asset_id"], rock_id)
        self.assertEqual(self.store.sources()[0]["current_path"], "new/rock.obj")
        self.assertEqual(len(self.store.source_revisions(rock_id)), 1)
        self.put("other/rock.obj", b"same rock")
        self.store.scan()
        self.assertEqual(len(self.store.sources()), 2)
        self.assertEqual(len({s["asset_id"] for s in self.store.sources()}), 2)

    def test_deleted_source_preserves_versions(self):
        source = self.put("scene/shot.ma", b"original maya")
        self.store.scan()
        source_id = self.store.sources()[0]["asset_id"]
        source.unlink()
        self.store.scan()
        self.assertEqual(self.store.sources()[0]["missing"], 1)
        self.assertEqual(self.store.source_version(source_id, 1)["size"], 13)

    def test_legacy_archive_is_migrated_without_modifying_share(self):
        source = self.put("scenes/old.ma", b"legacy")
        offline = Archive(self.source, self.archive, self.cache, settle=0)
        offline.scan()
        source.write_bytes(b"legacy-v2")
        offline.scan()
        upgraded = SharedArchive(self.source, self.archive, self.cache, settle=0)
        source_id = upgraded.sources()[0]["asset_id"]
        self.assertEqual(len(upgraded.source_revisions(source_id)), 2)
        self.assertEqual(source.read_bytes(), b"legacy-v2")

    def test_published_product_separate_from_source_and_catalog(self):
        self.put("textures/albedo.exr", b"working source")
        self.store.scan()
        source = self.store.sources()[0]
        one = self.store.publish(source["asset_id"], 1, "texture", "bc7",
                                 "albedo.ktx2", io.BytesIO(b"published v1"), 12)
        two = self.store.publish(source["asset_id"], 1, "texture", "bc7",
                                 "albedo.ktx2", io.BytesIO(b"published v2"), 12)
        self.assertEqual(one["product_id"], two["product_id"])
        self.assertEqual((one["version"], two["version"]), (1, 2))
        self.assertNotEqual(one["artifact_sha256"], two["artifact_sha256"])
        self.assertEqual(len(self.store.products(source["asset_id"])), 2)
        self.assertEqual((self.source / "textures" / "albedo.exr").read_bytes(),
                         b"working source")
        self.assertFalse((self.source / "textures" / "albedo.ktx2").exists())

    def test_publish_rejects_missing_or_bad_metadata(self):
        self.put("mesh.obj", b"data")
        self.store.scan()
        source_id = self.store.sources()[0]["asset_id"]
        with self.assertRaises(ValueError):
            self.store.publish(source_id, 1, "../bad", "", "mesh.glb",
                               io.BytesIO(b"x"), 1)
        with self.assertRaises(ValueError):
            self.store.publish(source_id, 1, "mesh", "", "../outside.glb",
                               io.BytesIO(b"x"), 1)
        with self.assertRaises(FileNotFoundError):
            self.store.publish(source_id, 999, "mesh", "", "mesh.glb",
                               io.BytesIO(b"x"), 1)
        with self.assertRaises(ValueError):
            self.store.publish(source_id, 1, "mesh", "", "mesh.glb",
                               io.BytesIO(b"x"), 4)

    def test_setup_creates_protected_separate_host_config(self):
        config, token = configure_host(
            str(self.source), str(self.archive), str(self.cache),
            config=Path(self.temp.name) / "server.json", listen_lan=True)
        settings = json.loads(config.read_text(encoding="utf-8"))
        self.assertEqual(settings["mode"], "shared")
        self.assertEqual(settings["schema_version"], 2)
        self.assertEqual(settings["bind"], "0.0.0.0")
        self.assertEqual(settings["access_token"], token)
        self.assertFalse((self.source / "server.json").exists())

    def test_http_auth_revisions_products_upload_and_client_cache(self):
        src = self.put("models/hero.obj", b"the-OBJ-source")
        self.store.scan()
        source_id = self.store.sources()[0]["asset_id"]
        httpd = ThreadingHTTPServer(
            ("127.0.0.1", 0), handler_for_shared(self.store, TOKEN))
        bg = threading.Thread(target=httpd.serve_forever, daemon=True)
        bg.start()
        url = "http://127.0.0.1:%d" % httpd.server_port
        try:
            # Auth is mandatory for all source/product lists and bytes.
            with self.assertRaises(HTTPError) as denied:
                urlopen(url + "/v2/sources")
            self.assertEqual(denied.exception.code, 401)
            with urlopen(url + "/health") as response:
                self.assertEqual(json.load(response)["status"], "ok")

            client = AssetClient(url, TOKEN, cache=self.workstation_cache,
                                 max_bytes=10 * 1024**2)
            self.assertEqual(len(client.sources()), 1)
            self.assertEqual(client.revisions(source_id)[0]["revision"], 1)

            # The NAS gives immutable bytes; the artist uses a LOCAL file.
            first = client.resolve_source(source_id, 1, pin=True)
            local_path = Path(first["local_path"])
            self.assertTrue(local_path.is_file())
            self.assertEqual(local_path.read_bytes(), b"the-OBJ-source")
            self.assertTrue(local_path.is_relative_to(self.workstation_cache.resolve()))
            self.assertNotEqual(local_path, src)
            second = client.resolve_source(source_id, 1, pin=True)
            self.assertEqual(first["local_path"], second["local_path"])

            product_file = Path(self.temp.name) / "cooked.glb"
            product_file.write_bytes(b"immutable published representation")
            published = client.publish_product(
                source_id, 1, "mesh", product_file, variant="game")
            self.assertEqual(published["source_asset_id"], source_id)
            self.assertEqual(len(client.products(source_id)), 1)
            selected = client.resolve_product(published["product_id"], 1)
            self.assertEqual(Path(selected["local_path"]).read_bytes(),
                             product_file.read_bytes())
            self.assertFalse((self.source / "models" / "cooked.glb").exists())

            # A byte endpoint cannot expose arbitrary archive filenames.
            req = Request(url + "/v2/blobs/" + "0" * 64,
                          headers={"Authorization": "Bearer " + TOKEN})
            with self.assertRaises(HTTPError) as missing:
                urlopen(req)
            self.assertEqual(missing.exception.code, 404)
            req = Request(url + "/v2/products",
                          headers={"Authorization": "Bearer wrong"})
            with self.assertRaises(HTTPError) as denied:
                urlopen(req)
            self.assertEqual(denied.exception.code, 401)
        finally:
            httpd.shutdown()
            httpd.server_close()
            bg.join(timeout=5)

    def test_client_config_stores_url_and_local_cache(self):
        root = Path(self.temp.name)
        config = configure_client("http://nas.example:8765", TOKEN,
                                  cache=root / "artist-cache",
                                  config=root / "client.json")
        raw = json.loads(config.read_text(encoding="utf-8"))
        self.assertEqual(raw["url"], "http://nas.example:8765")
        self.assertEqual(raw["token"], TOKEN)
        self.assertTrue(Path(raw["cache"]).is_dir())


if __name__ == "__main__":
    unittest.main()
