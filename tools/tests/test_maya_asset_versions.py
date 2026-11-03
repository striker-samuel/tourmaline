"""Maya asset-version client contract tests; no Maya installation required."""
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "maya"))
import tourmaline_asset_versions as maya_versions


class FakeResponse(io.BytesIO):
    pass


class FakeCmds:
    def __init__(self):
        self.files = []
        self.attrs = {}
        self.exists = {"existingReferenceRN"}

    def loadPlugin(self, *args, **kwargs):
        pass

    def file(self, path, **kwargs):
        self.files.append((path, kwargs))

    def createNode(self, kind, name):
        if kind != "network":
            raise AssertionError("unexpected metadata node")
        return "tourmalineAssetVersion1"

    def addAttr(self, node, longName, dataType):
        self.attrs[node + "." + longName] = ""

    def setAttr(self, attr, value, type):
        self.attrs[attr] = value

    def getAttr(self, attr):
        return self.attrs[attr]

    def attributeQuery(self, attr, node, exists):
        return node + "." + attr in self.attrs

    def objExists(self, name):
        return name in self.exists

    def referenceQuery(self, local, referenceNode):
        return "existingReferenceRN"


class MayaContractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache_file = Path(self.temp.name) / "mesh.obj"
        self.cache_file.write_bytes(b"obj")
        self.cmds = FakeCmds()
        self.item = {
            "revision": 2, "sha256": "a" * 64, "local_path": str(self.cache_file),
        }

    def test_resolve_uses_local_cache_and_pins(self):
        with patch.object(maya_versions, "_api", return_value=self.item) as api:
            item = maya_versions.resolve("characters/hero.obj", 2)
            self.assertEqual(Path(item["local_path"]).read_bytes(), b"obj")
            api.assert_called_once_with("/v1/resolve", path="characters/hero.obj",
                                        revision=2, pin=1)

    def test_missing_cache_is_error(self):
        bad = {**self.item, "local_path": str(Path(self.temp.name) / "missing.obj")}
        with patch.object(maya_versions, "_api", return_value=bad):
            with self.assertRaises(FileNotFoundError):
                maya_versions.resolve("hero.obj", 1)

    def test_asset_filter_and_revisions(self):
        with patch.object(maya_versions, "_api", return_value={"assets": [
                {"path": "characters/Hero.obj"}, {"path": "textures/ground.exr"}]}):
            self.assertEqual([a["path"] for a in maya_versions.available_assets("hero")],
                             ["characters/Hero.obj"])
        with patch.object(maya_versions, "_api", return_value={"versions": [{"revision": 3}]}):
            self.assertEqual(maya_versions.versions("hero.obj")[0]["revision"], 3)

    def test_import_pins_cached_bytes_and_records_working_revision(self):
        with patch.object(maya_versions, "_maya", return_value=self.cmds), \
             patch.object(maya_versions, "resolve", return_value=self.item):
            record = maya_versions.import_version("characters/hero.obj", 2)
        self.assertEqual(record, "tourmalineAssetVersion1")
        self.assertEqual(self.cmds.files[0][0], str(self.cache_file))
        self.assertTrue(self.cmds.files[0][1]["i"])
        self.assertEqual(
            self.cmds.attrs[record + ".tourmalineRevision"], "2")
        self.assertEqual(
            self.cmds.attrs[record + ".tourmalineAssetPath"], "characters/hero.obj")

    def test_reference_switches_version_without_modifying_share(self):
        self.cache_file = Path(self.temp.name) / "scene.ma"
        self.cache_file.write_bytes(b"mayaAscii")
        item = {**self.item, "local_path": str(self.cache_file)}
        record = "tourmalineAssetVersion1"
        for attr, value in (
                ("tourmalineAssetPath", "scenes/level.ma"),
                ("tourmalineImportMode", "reference"),
                ("tourmalineReferenceNode", "existingReferenceRN"),
                ("tourmalineRevision", "1"),
                ("tourmalineSha256", "b" * 64),
                ("tourmalineCachePath", "/old/scene.ma")):
            self.cmds.attrs[record + "." + attr] = value
        with patch.object(maya_versions, "_maya", return_value=self.cmds), \
             patch.object(maya_versions, "resolve", return_value=item):
            result = maya_versions.switch_reference_version(record, 2)
        self.assertEqual(result["revision"], 2)
        self.assertEqual(self.cmds.files[-1][1]["loadReference"], "existingReferenceRN")
        self.assertEqual(self.cmds.attrs[record + ".tourmalineRevision"], "2")

    def test_reference_rejects_obj(self):
        with patch.object(maya_versions, "_maya", return_value=self.cmds), \
             patch.object(maya_versions, "resolve", return_value=self.item):
            with self.assertRaisesRegex(ValueError, "Use Import"):
                maya_versions.import_version("mesh.obj", 2, reference=True)
        self.assertEqual(self.cmds.files, [])

    def test_request_queries_are_encoded(self):
        payload = json.dumps({"assets": []}).encode("utf-8")
        with patch.object(maya_versions, "urlopen", return_value=FakeResponse(payload)) as call:
            maya_versions._api("/v1/assets", path="my folder/a&b.obj")
        self.assertIn("my+folder%2Fa%26b.obj", call.call_args[0][0])

    def test_remote_service_not_accepted(self):
        with patch.dict(os.environ, {"TOURMALINE_ASSET_URL": "http://example.com:8765"}):
            with self.assertRaises(ValueError):
                maya_versions._server()


if __name__ == "__main__":
    unittest.main()
