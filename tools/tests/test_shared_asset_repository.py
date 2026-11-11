"""Cross-platform tests for stable source identity, explicit products, NAS API and caching."""
from __future__ import annotations

import io
import hashlib
from concurrent.futures import ThreadPoolExecutor
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
from tourmaline_asset_cache import AssetClient, load_client_settings, _NoAuthenticatedRedirect


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

    def test_verification_scan_detects_same_size_same_timestamp_edits(self):
        source = self.put("textures/normal.png", b"first")
        self.store.scan()
        source_id = self.store.sources()[0]["asset_id"]
        original = source.stat()
        source.write_bytes(b"other")
        os.utime(source, ns=(original.st_atime_ns, original.st_mtime_ns))
        self.assertEqual(self.store.scan()["captured"], 0)
        self.assertEqual(self.store.scan(verify=True)["captured"], 1)
        self.assertEqual(len(self.store.source_revisions(source_id)), 2)

    def test_corrupt_archive_blob_cannot_be_reused_for_publication(self):
        self.put("mesh.obj", b"source")
        self.store.scan()
        source_id = self.store.sources()[0]["asset_id"]
        content = b"published"
        sha = hashlib.sha256(content).hexdigest()
        blob = self.store.blob(self.archive, sha)
        blob.parent.mkdir(parents=True, exist_ok=True)
        blob.write_bytes(b"corrupted")
        with self.assertRaisesRegex(OSError, "SHA-256 mismatch"):
            self.store.publish(source_id, 1, "mesh", "", "mesh.glb",
                               io.BytesIO(content), len(content))
        self.assertEqual(blob.read_bytes(), b"corrupted")
        self.assertEqual(self.store.products(source_id), [])

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

    def test_reusing_a_moved_source_path_creates_distinct_identity(self):
        old = self.put("models/original.obj", b"original bytes")
        self.store.scan()
        original_id = self.store.sources()[0]["asset_id"]
        moved = self.source / "models" / "renamed.obj"
        old.rename(moved)
        self.store.scan()
        self.assertEqual(self.store.sources()[0]["asset_id"], original_id)

        self.put("models/original.obj", b"entirely different source")
        self.store.scan()
        by_path = {entry["current_path"]: entry for entry in self.store.sources()}
        self.assertEqual(by_path["models/renamed.obj"]["asset_id"], original_id)
        self.assertNotEqual(by_path["models/original.obj"]["asset_id"], original_id)
        self.assertEqual(len(self.store.source_revisions(original_id)), 1)
        new_id = by_path["models/original.obj"]["asset_id"]
        self.assertEqual(self.store.source_version(new_id, 1)["size"],
                         len(b"entirely different source"))

        self.put("models/original.obj", b"changed replacement")
        self.store.scan()
        self.assertEqual(len(self.store.source_revisions(new_id)), 2)
        self.assertEqual(len(self.store.source_revisions(original_id)), 1)

    def test_concurrent_publishes_get_distinct_versions(self):
        self.put("mesh.obj", b"source")
        self.store.scan()
        source_id = self.store.sources()[0]["asset_id"]

        def publish(index):
            content = ("product-%03d" % index).encode("ascii")
            return self.store.publish(source_id, 1, "mesh", "game", "mesh.glb",
                                      io.BytesIO(content), len(content))

        with ThreadPoolExecutor(max_workers=8) as pool:
            published = list(pool.map(publish, range(16)))
        self.assertEqual(sorted(row["version"] for row in published),
                         list(range(1, 17)))
        self.assertEqual(len({row["publish_id"] for row in published}), 16)

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

    def test_server_config_must_not_be_inside_source_share(self):
        bad_config = self.source / "private" / "server.json"
        with self.assertRaisesRegex(ValueError, "outside the source share"):
            configure_host(str(self.source), str(self.archive), str(self.cache),
                           config=bad_config)
        self.assertFalse(bad_config.exists())

    def test_client_failed_connectivity_check_does_not_save_config(self):
        from tools.configure_asset_client import main as client_main

        target = Path(self.temp.name) / "client.json"
        existing = b"previous working configuration"
        target.write_bytes(existing)
        with self.assertRaises(Exception):
            client_main(["--url", "http://127.0.0.1:1",
                         "--token", TOKEN, "--config", str(target), "--check"])
        self.assertEqual(target.read_bytes(), existing)

    def test_detached_installer_rejects_occupied_port(self):
        from tools.install_shared_assets import run_detached
        httpd = ThreadingHTTPServer(
            ("127.0.0.1", 0), handler_for_shared(self.store, TOKEN))
        try:
            config, _ = configure_host(
                str(self.source), str(self.archive), str(self.cache),
                config=Path(self.temp.name) / "server.json",
                port=httpd.server_port)
            with self.assertRaisesRegex(RuntimeError, "already in use"):
                run_detached(config)
        finally:
            httpd.server_close()

    def test_health_check_accepts_running_server(self):
        from tools.install_shared_assets import wait_for_health
        httpd = ThreadingHTTPServer(
            ("127.0.0.1", 0), handler_for_shared(self.store, TOKEN))
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            config, _ = configure_host(
                str(self.source), str(self.archive), str(self.cache),
                config=Path(self.temp.name) / "server.json",
                port=httpd.server_port)
            wait_for_health(config, timeout=2.0)
        finally:
            httpd.shutdown()
            httpd.server_close()
            thread.join(timeout=5)

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
            # A reverse proxy may not share an authenticated blob between
            # users, even though immutable content is otherwise cacheable.
            digest = hashlib.sha256(src.read_bytes()).hexdigest()
            with urlopen(Request(url + "/v2/blobs/" + digest,
                         headers={"Authorization": "Bearer " + TOKEN})) as fd:
                self.assertTrue(fd.headers["Cache-Control"].startswith("private"))
                self.assertEqual(fd.headers["Vary"], "Authorization")
                self.assertEqual(fd.read(), src.read_bytes())

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

    def test_remote_http_requires_explicit_opt_in(self):
        with self.assertRaisesRegex(ValueError, "require HTTPS"):
            AssetClient("http://nas.example:8765", TOKEN,
                        cache=self.workstation_cache)
        with self.assertRaisesRegex(ValueError, "require HTTPS"):
            configure_client("http://nas.example:8765", TOKEN,
                             cache=self.workstation_cache)
        secure = AssetClient("https://nas.example", TOKEN,
                             cache=self.workstation_cache)
        secure.close()
        local = AssetClient("http://127.0.0.1:8765", TOKEN,
                            cache=self.workstation_cache)
        local.close()

    def test_authenticated_redirects_fail_closed(self):
        req = Request("https://nas.example/v2/sources",
                      headers={"Authorization": "Bearer " + TOKEN})
        with self.assertRaises(HTTPError):
            _NoAuthenticatedRedirect().redirect_request(
                req, None, 302, "redirect", {},
                "https://attacker.example/collect")

    def test_client_config_stores_url_and_local_cache(self):
        root = Path(self.temp.name)
        config = configure_client("http://nas.example:8765", TOKEN,
                                  cache=root / "artist-cache",
                                  config=root / "client.json",
                                   allow_insecure_remote=True)
        raw = json.loads(config.read_text(encoding="utf-8"))
        self.assertEqual(raw["url"], "http://nas.example:8765")
        self.assertEqual(raw["token"], TOKEN)
        self.assertTrue(Path(raw["cache"]).is_dir())


if __name__ == "__main__":
    unittest.main()
