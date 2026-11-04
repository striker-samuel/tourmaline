"""User/group permission registry and HTTP authorization tests."""
from __future__ import annotations

import hashlib
import json
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
        self.registry.group_set("VIP", ["source.*", "product.publish"])
        self.registry.group_set("normal", ["source.read", "artifact.*", "-product.publish"])
        token = self.registry.user_set(
            "User1", ["VIP", "normal"], ["source.revisions.*"])
        self.assertTrue(token and len(token) >= 24)
        self.assertEqual(self.registry.identity(token), "User1")
        self.assertTrue(self.registry.allows("User1", "source.revisions.read"))
        self.assertTrue(self.registry.allows("User1", "product.versions.read"))
        self.assertTrue(self.registry.allows("User1", "artifact.read"))
        self.assertFalse(self.registry.allows("User1", "product.publish"))
        self.assertFalse(self.registry.allows("User1", "source.write"))
        self.registry.user_set("User1", ["VIP", "normal"],
                               ["source.revisions.*", "product.publish"])
        self.assertTrue(self.registry.allows("User1", "product.publish"))
        self.registry.user_set("User1", ["normal"], ["-source.read"])
        self.assertFalse(self.registry.allows("User1", "source.read"))

    def test_token_rotation_and_registry_does_not_store_plaintext(self):
        token = self.registry.user_set("worker", ["normal"], [])
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
            self.registry.group_set("normal", ["../bad"])
        with self.assertRaises(FileExistsError):
            PermissionRegistry.initialize(self.path)

    def test_reloads_edits_without_server_restart(self):
        self.registry.user_set("worker", ["normal"], [])
        self.assertTrue(self.registry.allows("worker", "source.read"))
        self.registry.group_set("normal", ["-source.read"])
        self.assertFalse(self.registry.allows("worker", "source.read"))

    def test_api_enforces_read_publish_and_rejects_old_shared_token(self):
        share = self.root / "share"
        share.mkdir()
        (share / "mesh.obj").write_bytes(b"mesh")
        archive = SharedArchive(share, self.root / "archive", self.root / "cache", settle=0)
        archive.scan()
        admin = self.admin_token
        viewer = self.registry.user_set("viewer", ["normal"], [])
        publisher = self.registry.user_set("publisher", ["normal"],
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
            self.registry.group_set("normal", ["-source.read"])
            expect_error(403, "/v2/sources", viewer)
        finally:
            httpd.shutdown()
            httpd.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
