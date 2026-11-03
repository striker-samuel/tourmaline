"""Advisory reader tracking tests: no locks, expires, filters, actual HTTP auth."""
from __future__ import annotations

import json
from http.server import ThreadingHTTPServer
import io
from pathlib import Path
import sys
import tempfile
import threading
import unittest
import uuid
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools" / "maya"))

from tools.asset_read_presence import ConsumerPresence
from tools.shared_asset_repository import SharedArchive, handler_for_shared
from tourmaline_asset_cache import AssetClient


TOKEN = "integration-test-repository-token-long-enough"


class PresenceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.source = root / "source"
        self.source.mkdir()
        self.store = SharedArchive(
            self.source, root / "archive", root / "server-cache", settle=0)
        self.now = [1000.0]
        self.presence = ConsumerPresence(self.store, ttl_seconds=90,
                                         clock=lambda: self.now[0])
        self.a = str(uuid.uuid4())
        self.b = str(uuid.uuid4())

    def source_asset(self, name, data):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        self.store.scan()
        entry = next(x for x in self.store.sources() if x["current_path"] == name)
        return path, entry["asset_id"]

    def test_two_consumers_two_assets_and_filter(self):
        _, x = self.source_asset("models/x.obj", b"x")
        _, y = self.source_asset("textures/y.exr", b"y")
        self.presence.heartbeat(self.a, "Maya station A", "maya", [
            {"kind": "source", "id": x, "version": 1},
            {"kind": "source", "id": y, "version": 1}])
        self.presence.heartbeat(self.b, "agent B", "agent", [
            {"kind": "source", "id": y, "version": 1}])
        active = self.presence.active()
        self.assertTrue(active["advisory_only"])
        self.assertEqual(len(active["consumers"]), 2)
        readers_y = self.presence.active("source", y)["consumers"]
        self.assertEqual(len(readers_y), 2)
        readers_x = self.presence.active("source", x)["consumers"]
        self.assertEqual([r["label"] for r in readers_x], ["Maya station A"])
        self.assertEqual({r["path"] for r in readers_y[0]["reads"]}, {"textures/y.exr"})

        # A consumer reports the FULL replacement set, not accumulated locks.
        self.presence.heartbeat(self.a, "Maya station A", "maya", [])
        self.assertEqual(len(self.presence.active("source", x)["consumers"]), 0)
        self.assertEqual(len(self.presence.active("source", y)["consumers"]), 1)

    def test_expire_disconnect_release(self):
        _, x = self.source_asset("x.ma", b"scene")
        payload = [{"kind": "source", "id": x, "version": 1}]
        self.presence.heartbeat(self.a, "Maya A", "maya", payload)
        self.now[0] += 80
        self.assertEqual(len(self.presence.active()["consumers"]), 1)
        self.presence.heartbeat(self.a, "Maya A", "maya", payload)
        self.now[0] += 89
        self.assertEqual(len(self.presence.active()["consumers"]), 1)
        self.now[0] += 2
        self.assertEqual(self.presence.active()["consumers"], [])
        self.presence.heartbeat(self.b, "Tool B", "tool", payload)
        self.presence.release(self.b)
        self.assertEqual(self.presence.active()["consumers"], [])

    def test_reject_invalid_sessions_resources_and_versions(self):
        _, x = self.source_asset("source.fbx", b"fbx")
        for bad in (
            [{"kind": "source", "id": "not-uuid", "version": 1}],
            [{"kind": "product", "id": x, "version": 1}],
            [{"kind": "source", "id": x, "version": 100}],
            [{"kind": "source", "id": x, "version": 0}],
        ):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.presence.heartbeat(self.a, "consumer", "maya", bad)
        with self.assertRaises(ValueError):
            self.presence.heartbeat("invalid", "Maya", "maya", [])
        with self.assertRaises(ValueError):
            self.presence.heartbeat(self.a, "Maya", "writer-lock", [])
        with self.assertRaises(ValueError):
            self.presence.active(resource_id=x)

    def test_products_are_separate_and_writes_never_blocked(self):
        path, x = self.source_asset("models/hero.obj", b"one")
        product = self.store.publish(x, 1, "mesh", "game",
                                     "hero.glb", io.BytesIO(b"published"), 9)
        prodid = product["product_id"]
        self.presence.heartbeat(self.a, "Maya A", "maya", [
            {"kind": "source", "id": x, "version": 1},
            {"kind": "product", "id": prodid, "version": 1}])
        self.assertEqual(len(self.presence.active("product", prodid)["consumers"]), 1)
        self.assertEqual(len(self.presence.active("source", x)["consumers"]), 1)
        # Presence cannot block edits or publish; scanner keeps capturing
        # independent source versions even during a 'read'.
        path.write_bytes(b"two")
        self.assertEqual(self.store.scan()["captured"], 1)
        self.assertEqual(self.store.source_version(x, 2)["sha256"] !=
                         self.store.source_version(x, 1)["sha256"], True)

    def test_authenticated_http_dashboard_and_heartbeat(self):
        _, x = self.source_asset("x.obj", b"data")
        server = ThreadingHTTPServer(("127.0.0.1", 0),
                                     handler_for_shared(self.store, TOKEN, 90))
        background = threading.Thread(target=server.serve_forever, daemon=True)
        background.start()
        url = "http://127.0.0.1:%s" % server.server_port
        try:
            with urlopen(url + "/consumers", timeout=4) as fd:
                html = fd.read().decode("utf-8")
                self.assertIn("Asset Consumers", html)
                self.assertNotIn(TOKEN, html)
            with self.assertRaises(HTTPError) as unauth:
                urlopen(url + "/v2/consumers", timeout=4)
            self.assertEqual(unauth.exception.code, 401)
            payload = {
                "session_id": self.a, "label": "Maya A", "kind": "maya",
                "reads": [{"kind": "source", "id": x, "version": 1}]
            }
            req = Request(
                url + "/v2/consumers/heartbeat",
                data=json.dumps(payload).encode("utf-8"),
                headers={"Authorization": "Bearer " + TOKEN,
                         "Content-Type": "application/json"},
                method="POST")
            with urlopen(req, timeout=4) as fd:
                self.assertEqual(json.load(fd)["reads"], 1)
            req = Request(url + "/v2/consumers?resource_kind=source&resource_id=" + x,
                          headers={"Authorization": "Bearer " + TOKEN})
            with urlopen(req, timeout=4) as fd:
                self.assertEqual(json.load(fd)["consumers"][0]["label"], "Maya A")
        finally:
            server.shutdown()
            server.server_close()
            background.join(timeout=5)

    def test_asset_client_auto_reports_and_releases(self):
        _, x = self.source_asset("models/a.obj", b"geometry")
        server = ThreadingHTTPServer(("127.0.0.1", 0),
                                     handler_for_shared(self.store, TOKEN, 90))
        background = threading.Thread(target=server.serve_forever, daemon=True)
        background.start()
        url = "http://127.0.0.1:%d" % server.server_port
        client = AssetClient(url, TOKEN, cache=Path(self.tmp.name) / "artist",
                             consumer_kind="maya", consumer_label="Maya A")
        try:
            item = client.resolve_source(x, 1)
            self.assertEqual(Path(item["local_path"]).read_bytes(), b"geometry")
            data = client.consumers(resource_kind="source", resource_id=x)
            self.assertEqual(data["consumers"][0]["label"], "Maya A")
            client.clear_reads()
            self.assertEqual(client.consumers(resource_kind="source", resource_id=x)[
                "consumers"], [])
            client.resolve_source(x, 1)
            client.close()
            self.assertEqual(client.consumers(resource_kind="source", resource_id=x)[
                "consumers"], [])
        finally:
            server.shutdown()
            server.server_close()
            background.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
