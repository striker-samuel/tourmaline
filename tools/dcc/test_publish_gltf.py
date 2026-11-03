"""CPU-only tests for the DCC publish transaction; no Maya/Blender required."""
from __future__ import annotations

import json
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

from publish_gltf import publish, validate_gltf


class TestPublishGltf(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "work" / "asset.ma"
        self.source.parent.mkdir()
        self.source.write_text("maya scene", encoding="utf-8")
        self.published = self.root / "content" / "asset.gltf"
        self.published.parent.mkdir()
        self.published.write_text("old good output", encoding="utf-8")

    def _run(self, valid=True, returncode=0):
        def fake_run(args, **kwargs):
            staging = Path(args[-1])
            if returncode == 0:
                staging.write_text(
                    json.dumps({"asset": {"version": "2.0"}, "scenes": [],
                                "buffers": [{"uri": "data:application/octet-stream;base64,AA=="}]})
                    if valid else '{"asset":{"version":"1.0"}}',
                    encoding="utf-8")
            return type("Result", (), {"returncode": returncode, "stdout": ""})()
        with patch("dcc_runner.subprocess.run", side_effect=fake_run):
            publish(self.source, self.published, "maya", maya_exe="fake-mayapy")

    def test_success_replaces_existing_publication(self):
        self._run()
        self.assertIn('"version": "2.0"', self.published.read_text(encoding="utf-8"))

    def test_failure_keeps_last_good(self):
        with self.assertRaises(RuntimeError):
            self._run(returncode=1)
        self.assertEqual(self.published.read_text(), "old good output")

    def test_invalid_export_keeps_last_good(self):
        with self.assertRaises(ValueError):
            self._run(valid=False)
        self.assertEqual(self.published.read_text(), "old good output")

    def test_malformed_gltf_never_replaces_last_good(self):
        cases = [
            {"asset": [], "scenes": []},
            {"asset": {"version": "2.invalid"}, "scenes": []},
            {"asset": {"version": "2.0"}, "scenes": [], "buffers": [{"byteLength": 4}]},
            {"asset": {"version": "2.0"}, "scenes": [], "buffers": [None]},
            {"asset": {"version": "2.0"}, "scenes": [], "images": [{"uri": 42}]},
            {"asset": {"version": "2.0"}, "scenes": [], "images": [{}]},
        ]
        for document in cases:
            with self.subTest(document=document):
                def fake_run(args, **kwargs):
                    Path(args[-1]).write_text(json.dumps(document), encoding="utf-8")
                    return type("Result", (), {"returncode": 0, "stdout": ""})()
                with patch("dcc_runner.subprocess.run", side_effect=fake_run):
                    with self.assertRaises(ValueError):
                        publish(self.source, self.published, "maya", maya_exe="fake")
                self.assertEqual(self.published.read_text(), "old good output")

    def test_embedded_bufferview_image_is_allowed(self):
        path = self.root / "embedded.gltf"
        path.write_text(json.dumps({
            "asset": {"version": "2.0"}, "scenes": [],
            "buffers": [{"byteLength": 1, "uri": "data:application/octet-stream;base64,AA=="}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 1}],
            "images": [{"bufferView": 0, "mimeType": "image/png"}],
        }), encoding="utf-8")
        validate_gltf(path)

    def test_rejects_non_gltf_publication(self):
        with self.assertRaises(ValueError):
            publish(self.source, self.root / "model.obj", "maya")

    def test_rejects_non_embedded_dependencies(self):
        def fake_run(args, **kwargs):
            Path(args[-1]).write_text(
                json.dumps({"asset": {"version": "2.0"}, "scenes": [],
                            "buffers": [{"uri": "../outside.bin"}]}))
            return type("Result", (), {"returncode": 0, "stdout": ""})()
        with patch("dcc_runner.subprocess.run", side_effect=fake_run):
            with self.assertRaises(ValueError):
                publish(self.source, self.published, "maya", maya_exe="fake")
        self.assertEqual(self.published.read_text(), "old good output")


if __name__ == "__main__":
    unittest.main()
