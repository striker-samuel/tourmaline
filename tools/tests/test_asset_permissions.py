"""User/group permission registry and HTTP authorization tests."""
from __future__ import annotations

import hashlib
import json
import os
import stat
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.asset_permissions import PermissionRegistry, match_rule, permitted
from tools.shared_asset_repository import SharedArchive, handler_for_shared
from tools.install_shared_assets import configure as configure_host

LEGACY_TOKEN = "old-deployment-token-does-not-bypass-the-registry"


class PermissionRegistryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.path = self.root / "protected" / "users.json"
        self.admin_token = PermissionRegistry.initialize(self.path)
        self.registry = PermissionRegistry(self.path)

    def test_wildcards_and_exact_matches(self):
        self.assertTrue(match_rule("source.*", "source.revisions.read"))
        self.assertTrue(match_rule("source.revisions.*", "source.revisions.list"))
        self.assertFalse(match_rule("source.revisions.*", "product.versions.list"))
        self.assertFalse(match_rule("source.*", "source_archive.read"))
        self.assertTrue(match_rule("*", "source.read"))
        self.assertTrue(permitted(["source.*"], "source.revisions.read")[1])

    def test_users_groups_custom_rules_and_denials(self):
        self.registry.group_set("publishers", ["source.*", "product.*"])
        self.registry.group_set("readers", ["source.read", "artifact.*", "-product.publish"])
        token = self.registry.user_set(
            "artist_one", ["publishers", "readers"], ["source.revisions.*"])
        self.assertTrue(token and len(token) >= 24)
        self.assertEqual(self.registry.identity(token), "artist_one")
        self.assertTrue(self.registry.allows("artist_one", "source.revisions.read"))
        self.assertTrue(self.registry.allows("artist_one", "product.versions.read"))
        self.assertTrue(self.registry.allows("artist_one", "artifact.read"))
        self.assertFalse(self.registry.allows("artist_one", "product.publish"))
        self.assertFalse(self.registry.allows("artist_one", "source.write"))
        self.registry.user_set("artist_one", ["publishers", "readers"],
                               ["source.revisions.*", "product.publish"])
        self.assertTrue(self.registry.allows("artist_one", "product.publish"))
        self.registry.user_set("artist_one", ["readers"], ["-source.read"])
        self.assertFalse(self.registry.allows("artist_one", "source.read"))

    def test_token_rotation_and_registry_does_not_store_plaintext(self):
        token = self.registry.user_set("worker", ["readers"], [])
        raw = self.path.read_text(encoding="utf-8")
        self.assertNotIn(token, raw)
        self.assertIn(hashlib.sha256(token.encode()).hexdigest(), raw)
        new_token = self.registry.rotate_token("worker")
        self.assertIsNone(self.registry.identity(token))
        self.assertEqual(self.registry.identity(new_token), "worker")

    def test_invalid_group_and_registry_reinit(self):
        with self.assertRaises(ValueError):
            self.registry.user_set("bad", ["unknown"], [])
        with self.assertRaises(ValueError):
            self.registry.group_set("readers", ["../bad"])
        with self.assertRaises(FileExistsError):
            PermissionRegistry.initialize(self.path)

    def test_reloads_edits_without_server_restart(self):
        self.registry.user_set("worker", ["readers"], [])
        self.assertTrue(self.registry.allows("worker", "source.read"))
        self.registry.group_set("readers", ["-source.read"])
        self.assertFalse(self.registry.allows("worker", "source.read"))

    def test_per_asset_user_and_group_permissions(self):
        source_id = "11111111-1111-4111-8111-111111111111"
        other_id = "22222222-2222-4222-8222-222222222222"
        self.registry.group_set("artists", ["source.read", "product.publish",
                                            "artifact.read"])
        self.registry.user_set("artist", ["artists"], [])
        self.registry.user_set("guest", ["readers"], [])
        self.registry.asset_set(source_id, "groups", "artists",
                                ["source.read", "product.publish", "artifact.read"])
        self.registry.asset_set(source_id, "users", "guest",
                                ["source.read"])
        self.assertTrue(self.registry.allows("artist", "source.read", source_id))
        self.assertTrue(self.registry.allows("artist", "product.publish", source_id))
        self.assertTrue(self.registry.allows("artist", "source.read", other_id))
        self.assertTrue(self.registry.allows("guest", "source.read", source_id))
        self.assertFalse(self.registry.allows("guest", "artifact.read", source_id))
        self.assertFalse(self.registry.allows("guest", "product.publish", source_id))
        self.assertTrue(self.registry.allows("admin", "artifact.read", source_id))
        self.registry.asset_set(source_id, "users", "guest",
                                ["-source.read"])
        self.assertFalse(self.registry.allows("guest", "source.read", source_id))
        self.registry.asset_clear(source_id)
        self.assertTrue(self.registry.allows("guest", "artifact.read", source_id))

    def test_api_filters_asset_lists_and_blob_downloads(self):
        share = self.root / "share"
        share.mkdir()
        (share / "allowed.obj").write_bytes(b"allowed")
        (share / "restricted.obj").write_bytes(b"restricted")
        archive = SharedArchive(share, self.root / "archive", self.root / "cache", settle=0)
        archive.scan()
        ids = {row["current_path"]: row["asset_id"] for row in archive.sources()}
        allowed_id = ids["allowed.obj"]
        restricted_id = ids["restricted.obj"]
        self.registry.asset_set(restricted_id, "users", "admin", ["*"])
        httpd = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            handler_for_shared(archive, LEGACY_TOKEN, registry=self.registry))
        bg = threading.Thread(target=httpd.serve_forever, daemon=True)
        bg.start()
        base = "http://127.0.0.1:%d" % httpd.server_port
        token = self.registry.user_set("viewer", ["readers"], [])
        def get(path, auth=token):
            return urlopen(Request(base + path,
                headers={"Authorization": "Bearer " + auth}), timeout=3)
        try:
            with get("/v2/sources") as response:
                rows = json.load(response)["sources"]
            self.assertEqual({r["asset_id"] for r in rows}, {allowed_id})
            with self.assertRaises(HTTPError) as exc:
                get("/v2/resolve?asset_id=" + restricted_id)
            self.assertEqual(exc.exception.code, 404)
            secret_sha = archive.source_version(restricted_id, 1)["sha256"]
            with self.assertRaises(HTTPError) as exc:
                get("/v2/blobs/" + secret_sha)
            self.assertEqual(exc.exception.code, 404)
            with get("/v2/sources", self.admin_token) as response:
                self.assertEqual(len(json.load(response)["sources"]), 2)
        finally:
            httpd.shutdown()
            httpd.server_close()
            bg.join(timeout=5)

    @unittest.skipIf(os.name == "nt", "POSIX mode/ownership test")
    def test_registry_updates_preserve_read_permissions(self):
        self.path.chmod(0o640)
        original = self.path.stat()
        self.registry.group_set("artists", ["source.read"])
        updated = self.path.stat()
        self.assertEqual(stat.S_IMODE(updated.st_mode), 0o640)
        self.assertEqual((updated.st_uid, updated.st_gid),
                         (original.st_uid, original.st_gid))

    def test_reinstall_preserves_registry_configuration(self):
        share = self.root / "share"
        share.mkdir()
        config = self.root / "host.json"
        args = (str(share), str(self.root / "archive"),
                str(self.root / "cache"))
        config_path, old_token = configure_host(*args, config=config)
        settings = json.loads(config_path.read_text(encoding="utf-8"))
        settings["permissions_registry"] = str(self.path)
        config_path.write_text(json.dumps(settings), encoding="utf-8")
        config_path, token = configure_host(*args, config=config)
        updated = json.loads(config_path.read_text(encoding="utf-8"))
        self.assertEqual(updated["permissions_registry"], str(self.path))
        self.assertEqual(token, old_token)

    def test_live_open_asset_database_tracks_authenticated_user_and_station(self):
        import uuid
        share = self.root / "share"
        share.mkdir()
        (share / "scene.tour").write_bytes(b"scene")
        archive = SharedArchive(share, self.root / "archive", self.root / "cache", settle=0)
        archive.scan()
        asset_id = archive.sources()[0]["asset_id"]
        token = self.registry.user_set("designer", ["readers"], [])
        other = self.registry.user_set("other", ["readers"], [])
        session = str(uuid.uuid4())
        httpd = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            handler_for_shared(archive, LEGACY_TOKEN, registry=self.registry))
        bg = threading.Thread(target=httpd.serve_forever, daemon=True)
        bg.start()
        base = "http://127.0.0.1:%d" % httpd.server_port

        def post(path, access_token, body):
            return urlopen(Request(
                base + path, method="POST", data=json.dumps(body).encode(),
                headers={"Authorization": "Bearer " + access_token,
                         "Content-Type": "application/json"}), timeout=3)

        def get(access_token):
            return urlopen(Request(
                base + "/v2/open-assets",
                headers={"Authorization": "Bearer " + access_token}), timeout=3)

        body = {"session_id": session, "kind": "editor", "label": "Level Editor",
                "workstation": "ART-STATION-07",
                "reads": [{"kind": "source", "id": asset_id, "version": 1,
                           "mode": "edit"}]}
        try:
            with post("/v2/consumers/heartbeat", token, body) as response:
                self.assertEqual(json.load(response)["reads"], 1)
            with get(token) as response:
                entries = json.load(response)["open_assets"]
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["username"], "designer")
            self.assertEqual(entries[0]["workstation"], "ART-STATION-07")
            self.assertEqual(entries[0]["mode"], "edit")
            self.assertEqual(entries[0]["id"], asset_id)
            with self.assertRaises(HTTPError) as exc:
                post("/v2/consumers/release", other, {"session_id": session})
            self.assertEqual(exc.exception.code, 403)
            with post("/v2/consumers/release", token, {"session_id": session}):
                pass
            with get(token) as response:
                self.assertEqual(json.load(response)["open_assets"], [])
        finally:
            httpd.shutdown()
            httpd.server_close()
            bg.join(timeout=5)

    def test_api_enforces_read_publish_and_rejects_old_shared_token(self):
        share = self.root / "share"
        share.mkdir()
        (share / "mesh.obj").write_bytes(b"mesh")
        archive = SharedArchive(share, self.root / "archive", self.root / "cache", settle=0)
        archive.scan()
        admin = self.admin_token
        viewer = self.registry.user_set("viewer", ["readers"], [])
        publisher = self.registry.user_set("publisher", ["readers"],
                                           ["product.publish"])
        httpd = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            handler_for_shared(archive, LEGACY_TOKEN, registry=self.registry))
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        base = "http://127.0.0.1:%d" % httpd.server_port

        def request(path, token=None, method="GET"):
            headers = {"Authorization": "Bearer " + token} if token else {}
            return urlopen(Request(base + path, headers=headers, method=method),
                           timeout=3)

        def expect_error(code, path, token=None, method="GET"):
            with self.assertRaises(HTTPError) as ctx:
                request(path, token, method)
            self.assertEqual(ctx.exception.code, code)

        try:
            with request("/health") as response:
                self.assertEqual(json.load(response)["status"], "ok")
            with request("/v2/sources", viewer) as response:
                self.assertEqual(len(json.load(response)["sources"]), 1)
            expect_error(401, "/v2/sources")
            expect_error(401, "/v2/sources", LEGACY_TOKEN)
            expect_error(403, "/v2/publish", viewer, "PUT")
            # Publish permission is authorized before metadata processing.
            expect_error(400, "/v2/publish", publisher, "PUT")
            expect_error(401, "/v2/consumers", "wrong-token")
            with request("/v2/products", admin) as response:
                self.assertIn("products", json.load(response))
            self.registry.group_set("readers", ["-source.read"])
            expect_error(403, "/v2/sources", viewer)
        finally:
            httpd.shutdown()
            httpd.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
