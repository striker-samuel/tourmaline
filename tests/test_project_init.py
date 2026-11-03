#!/usr/bin/env python3
"""GPU-free project-init tests: simulated native Project, no project GUID reimplementation."""
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import project_init


class InitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.exe = self.root / "Project"
        self.exe.write_text("stub", encoding="utf-8")
        self.dest = self.root / "MyGame"
        self.args = ["--name", "My Game", "--path", str(self.dest),
                     "--project-exe", str(self.exe)]

    def invoke(self, *args, mock=None):
        out = io.StringIO()
        with redirect_stdout(out):
            if mock is None:
                rc = project_init.main([*self.args, *args, "--json"])
            else:
                with patch.object(project_init.subprocess, "run", side_effect=mock):
                    rc = project_init.main([*self.args, *args, "--json"])
        return rc, json.loads(out.getvalue()) if out.getvalue() else None

    def fake_native(self, argv, **kwargs):
        dest = Path(argv[argv.index("--project") + 1])
        dest.mkdir(parents=True, exist_ok=True)
        (dest / "project.tourproj").write_text(
            "# Native created GUID\nname=My Game\n"
            "guid=0123456789abcdef0123456789abcdef\nversion=2\n", encoding="utf-8")
        return SimpleNamespace(returncode=0, stdout="created", stderr="")

    def test_dry_run_writes_nothing(self):
        rc, info = self.invoke("--dry-run", "--readme", "--gitignore")
        self.assertEqual(rc, 0)
        self.assertEqual(info["status"], "planned")
        self.assertFalse(self.dest.exists())
        self.assertEqual(len(info["files_to_create"]), 3)

    def test_scaffold_recipe_and_manifest_without_overwrite(self):
        rc, info = self.invoke("--readme", "--gitignore", "--bake-gi", "on",
                               mock=self.fake_native)
        self.assertEqual(rc, 0)
        self.assertEqual(info["status"], "created")
        recipe = (self.dest / "game.tourrecipe").read_text()
        self.assertIn("recipe 1\nproject My Game\n", recipe)
        self.assertIn("entry content/scenes/main.tour", recipe)
        self.assertIn("bake gi on", recipe)
        self.assertIn("[pak world]\noutput world.pak", recipe)
        self.assertIn("scene content/scenes/main.tour", recipe)
        self.assertFalse(info["scene_ready"])
        self.assertTrue((self.dest / ".gitignore").is_file())
        self.assertTrue((self.dest / "README.md").is_file())
        self.assertIn("guid=", (self.dest / "project.tourproj").read_text())
        rc, _ = self.invoke("--adopt", mock=self.fake_native)
        self.assertEqual(rc, 2)  # never overwrite existing recipe

    def test_from_scene_tracks_native_seed(self):
        model = self.root / "stone.glb"
        scene = self.root / "intro.tour"
        model.write_text("mock")
        scene.write_text("mock")
        def importer(argv, **kwargs):
            result = self.fake_native(argv, **kwargs)
            path = self.dest / "content/scenes/intro.tour"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("native seeded")
            return result
        rc, info = self.invoke("--from-gltf", str(model), "--from-scene", str(scene),
                               "--pak", "main", "--recipe-name", "level", mock=importer)
        self.assertEqual(rc, 0)
        self.assertTrue(info["scene_ready"])
        self.assertEqual(info["entry_scene"], "content/scenes/intro.tour")
        self.assertTrue((self.dest / "level.tourrecipe").is_file())
        self.assertIn("--from-gltf", info["native_command"])
        self.assertIn("--from-scene", info["native_command"])

    def test_existing_folder_refusal_and_adopt(self):
        self.dest.mkdir()
        (self.dest / "unrelated.txt").write_text("untouched")
        rc, _ = self.invoke("--dry-run")
        self.assertEqual(rc, 2)
        self.assertEqual((self.dest / "unrelated.txt").read_text(), "untouched")
        (self.dest / "project.tourproj").write_text(
            "name=My Game\nguid=0123456789abcdef0123456789abcdef\nversion=2\n")
        rc, info = self.invoke("--dry-run", "--adopt")
        self.assertEqual(rc, 0)
        self.assertEqual(info["existing_guid"], "0123456789abcdef0123456789abcdef")
        rc, _ = self.invoke("--dry-run", "--name", "Wrong", "--adopt")
        self.assertEqual(rc, 2)

    def test_unsafe_paths_and_source_check(self):
        for values in (("--recipe-name", "../escape"), ("--pak", "a/b"),
                       ("--entry-scene", "../bad.tour"), ("--from-gltf", "missing.glb"),
                       ("--from-scene", "unknown.tour")):
            with self.subTest(values=values):
                rc, _ = self.invoke("--dry-run", *values)
                self.assertEqual(rc, 2)
        rc, info = self.invoke("--dry-run", "--no-recipe")
        self.assertEqual(rc, 0)
        self.assertIsNone(info["recipe"])


if __name__ == "__main__":
    unittest.main()
