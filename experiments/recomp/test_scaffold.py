# SPDX-License-Identifier: MIT
"""Offline, asset-free checks for the external Tourmaline project wrapper."""
import base64
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import scaffold_external as pipe


class AssetPipelineTests(unittest.TestCase):
    def test_repository_exclusion(self):
        with self.assertRaisesRegex(ValueError, "OUTSIDE"):
            pipe.external(pipe.REPO, "root")
        with self.assertRaisesRegex(ValueError, "OUTSIDE"):
            pipe.external(pipe.REPO / "content" / "rom.z64", "ROM")
        with tempfile.TemporaryDirectory() as t:
            sibling = Path(t) / "tourmaline-neighbor"
            self.assertEqual(pipe.external(sibling, "assets"), sibling.resolve())

    def test_bootstrap_and_build_plan(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t) / "project"
            publish = Path(t) / "published"
            calls = []
            with patch.object(pipe, "invoke", side_effect=lambda *args: calls.append(args)):
                pipe.make_project(root, Path("Project"))
                self.assertEqual(len(calls), 2)
                self.assertEqual(calls[0][1], "init")
                self.assertEqual(calls[1][1], "import")
                scene = root / pipe.SCENE_REL
                self.assertTrue(scene.is_file())
                model = json.loads((root / ".tourmaline/scaffold/recomp_bootstrap.gltf")
                                   .read_text(encoding="utf-8"))
                blob = base64.b64decode(model["buffers"][0]["uri"].split(",", 1)[1])
                self.assertEqual(len(blob), model["buffers"][0]["byteLength"])
                self.assertTrue((root / pipe.RECIPE).is_file())
                self.assertFalse((root / "content/extracted").is_file())
                # User assets are never invented or downloaded by bootstrap.
                image = root / "content/extracted/hud.png"
                sound = root / "content/extracted/sfx/coin.wav"
                sound.parent.mkdir(parents=True, exist_ok=True)
                image.write_bytes(b"TEST.PNG")
                sound.write_bytes(b"TEST.WAV")
                (root / "project.tourproj").write_text("dummy test marker\n")
                pipe.build(root, publish, Path("Project"))
                self.assertEqual([c[1] for c in calls[2:]], ["validate", "ids", "build"])
                recipe = (root / pipe.RECIPE).read_text(encoding="utf-8")
                self.assertIn("include \"content/extracted/hud.png\"", recipe)
                self.assertIn("include \"content/extracted/sfx/coin.wav\"", recipe)
                self.assertIn("depends extracted", recipe)
                self.assertIn("[pak extracted]", recipe)
                self.assertIn("--publish", calls[-1])
                self.assertEqual(Path(calls[-1][calls[-1].index("--publish") + 1]),
                                 publish)
                self.assertIn("--header", calls[-1])
                self.assertTrue((root / "generated").is_dir())

    def test_unknown_format_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            payload = root / "content/extracted/unknown.n64"
            payload.parent.mkdir(parents=True)
            payload.write_bytes(b"test")
            with self.assertRaisesRegex(ValueError, "Unsupported extracted asset"):
                pipe.write_recipe(root)
            self.assertFalse((root / pipe.RECIPE).exists())

    def test_symlink_asset_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            path = root / "content/extracted/external.png"
            path.parent.mkdir(parents=True)
            source = root / "test-image.png"
            source.write_bytes(b"TEST")
            try:
                path.symlink_to(source)
            except (NotImplementedError, OSError):
                self.skipTest("Symlinks unavailable on runner")
            with self.assertRaisesRegex(ValueError, "symlinked asset"):
                pipe.write_recipe(root)


if __name__ == "__main__":
    unittest.main()
