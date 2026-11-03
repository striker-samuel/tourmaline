# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""CPU-only search scoring and image-fab request tests (no live API calls)."""
import base64
from contextlib import redirect_stdout
import copy
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import asset_reverse_search as search


@unittest.skipIf(search.Image is None, "Pillow not installed")
class ReverseSearchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.root = root
        names = ("red", "red_copy", "blue", "red_huge")
        colors = ((250, 20, 20, 255), (250, 20, 20, 255),
                  (10, 30, 255, 255), (250, 20, 20, 255))
        self.assets = []
        # A single synthetic mip with rects at arbitrary locations. This
        # forces the indexer to use the manifest, never implicit shifts.
        pages = {ch: search.Image.new("RGBA", (128, 24))
                 for ch in search.CAPTURE_CHANNELS}
        for i, name in enumerate(names):
            boxes = {}
            for j, view in enumerate(search.VIEWS):
                x, y = i * 24 + (j % 3) * 7 + 1, (j // 3) * 8 + 1
                boxes[view] = dict(x=x, y=y, width=6, height=6)
                for ch, canvas in pages.items():
                    if ch == "coverage":
                        rgba = (255, 255, 255, 255)
                    elif ch == "albedo":
                        rgba = colors[i]
                    elif ch == "normal":
                        rgba = (127, 127, 255, 255)
                    elif ch == "orm":
                        rgba = (255, 100, 25, 255)
                    else:
                        rgba = (0, 0, 0, 255)
                    canvas.paste(search.Image.new("RGBA", (6, 6), rgba), (x, y))
            self.assets.append({"source": f"assets/{name}.geometry",
                                "sha256": "a" * 63 + str(i),
                                "geometry_guid": f"guid-{i}", "materials": [],
                                "bounds": {"min": [0, 0, 0], "max": [10, 10, 10] if i == 3 else [1, 1, 1],
                                           "size": [10, 10, 10] if i == 3 else [1, 1, 1]},
                                "mips": [{"level": 2, "views": boxes}],
                                "triangle_count": 12})
        files = {}
        for ch, page in pages.items():
            filename = f"{ch}_mip2.png"
            page.save(root / filename)
            files[ch] = filename
        (root / "index.json").write_text(json.dumps({
            "version": 1, "mips": [{"level": 2, "face_size": 6,
                                     "width": 128, "height": 24, "files": files}],
            "assets": self.assets}), encoding="utf-8")
        self.index_path = root / "reverse.json"
        self.doc = search.atlas_to_search_index(root / "index.json", self.index_path, mip=2, grid=6)

    def test_exact_similarity_and_size_filter(self):
        a = search.match_asset(self.doc, "red.geometry")
        rows = search.search(self.doc, a, limit=10, exclude_source=a["source"])
        self.assertEqual(rows[0]["source"], "assets/red_copy.geometry")
        self.assertEqual(rows[0]["score"], 100)
        self.assertEqual(rows[0]["dimensions"]["shape"], 100)
        self.assertEqual(rows[0]["dimensions"]["size"], 100)
        self.assertEqual(len(rows), 2)  # Huge asset filtered by 2x per-axis
        self.assertLess(rows[1]["dimensions"]["color"], 60)
        self.assertEqual(rows[1]["dimensions"]["shape"], 100)
        all_rows = search.search(self.doc, a, exclude_source=a["source"], size_ratio=0)
        self.assertEqual(len(all_rows), 3)
        self.assertEqual(set(r["source"] for r in all_rows), {
            "assets/red_copy.geometry", "assets/red_huge.geometry", "assets/blue.geometry"})

    def test_geometry_scale_and_dimension_weights(self):
        a = self.doc["assets"][0]
        huge = self.doc["assets"][3]
        value, ratio = search.size_score(a, huge)
        self.assertAlmostEqual(ratio, 10)
        self.assertLess(value, .2)
        with self.assertRaises(search.SearchError):
            search.weights_from_string("shape=-1")
        with self.assertRaises(search.SearchError):
            search.weights_from_string("shape=nan")
        with self.assertRaises(search.SearchError):
            search.weights_from_string("unknown=1")
        ranking = search.search(self.doc, a, size_ratio=0, weights={"size": 1})
        self.assertEqual(ranking[0]["score"], 100)
        self.assertTrue(ranking[-1]["score"] < 100)

    def test_image_query_has_only_available_scores(self):
        png = self.root / "query.png"
        search.Image.new("RGBA", (32, 32), (250, 20, 20, 255)).save(png)
        q = search.descriptor_for_image(png, self.doc, view="+Z")
        rows = search.search(self.doc, q, size_ratio=2)
        self.assertEqual(rows[0]["score"], 100)
        self.assertIsNone(rows[0]["dimensions"]["size"])
        self.assertIsNone(rows[0]["dimensions"]["normal"])
        self.assertIsNone(rows[0]["dimensions"]["roughness"])
        blue = next(row for row in rows if row["source"] == "assets/blue.geometry")
        self.assertLess(blue["dimensions"]["color"], rows[0]["dimensions"]["color"])
        with self.assertRaises(search.SearchError):
            search.descriptor_for_image(png, self.doc, view="not a view")

    def test_description_and_offline_fab_plan(self):
        asset = self.doc["assets"][0]
        description = search.asset_description(self.doc, asset)
        self.assertIn("dimensions 1 × 1 × 1", description["description"])
        self.assertEqual(description["measured"]["size"], [1, 1, 1])
        with patch.object(search, "generate_image_request", side_effect=AssertionError("network")):
            plan = search.fab(self.doc, asset, self.root / "fab", "stylized prop",
                              "gpt-image-2.5-flare", "1024x1024", "medium", 1)
        self.assertEqual(plan["status"], "planned")
        self.assertEqual(plan["images"], [])
        self.assertTrue((self.root / "fab/fab.json").is_file())
        self.assertFalse((self.root / "fab/fab_001.png").exists())

    def test_fab_api_mock_persists_image_and_matches(self):
        buf = io.BytesIO()
        search.Image.new("RGBA", (12, 12), (250, 20, 20, 255)).save(buf, format="PNG")
        encoded = base64.b64encode(buf.getvalue()).decode()
        class Response:
            def __enter__(self): return self
            def __exit__(self, exc_type, exc_val, exc_tb): return False
            def read(self): return json.dumps({"data": [{"b64_json": encoded}]}).encode()
        def fake_open(req, timeout):
            self.assertEqual(req.get_method(), "POST")
            self.assertIn("/v1/images/generations", req.full_url)
            self.assertEqual(timeout, 180)
            self.assertIn("Bearer fake-key", req.get_header("Authorization"))
            self.assertEqual(json.loads(req.data)["background"], "transparent")
            return Response()
        with patch.dict(os.environ, {"OPENAI_API_KEY": "fake-key"}):
            outcome = search.fab(self.doc, self.doc["assets"][0], self.root / "gen",
                                 "game-ready prop", "gpt-image-2.5-flare", "1024x1024",
                                 "medium", 1, execute=True, search_count=2, opener=fake_open)
        self.assertEqual(outcome["status"], "generated")
        self.assertEqual(outcome["images"][0]["similar_assets"][0]["score"], 100)
        self.assertTrue((self.root / "gen/fab_001.png").is_file())
        self.assertNotIn("fake-key", (self.root / "gen/fab.json").read_text())

    def test_corrupt_rect_and_descriptor_rejected(self):
        doc = json.loads((self.root / "index.json").read_text())
        doc["assets"][0]["mips"][0]["views"]["+X"]["x"] = 9999
        (self.root / "index.json").write_text(json.dumps(doc))
        with self.assertRaisesRegex(search.SearchError, "outside atlas"):
            search.atlas_to_search_index(self.root / "index.json", self.root / "broken.json", mip=2)
        self.doc["assets"][0]["features"]["+Z"]["albedo"] = "invalid"
        (self.root / "reverse.json").write_text(json.dumps(self.doc))
        with self.assertRaises(search.SearchError):
            search.load_search_index(self.root / "reverse.json")

    def test_mesh_reference_resolution(self):
        path = self.root / "legacy.mesh"
        target = self.root / "object.geometry"
        target.write_bytes(b"HSEM\x03\x00\x00\x00")
        path.write_text("geometry=object.geometry\n", encoding="utf-8")
        self.assertEqual(search.resolve_geometry(path), target)
        self.assertEqual(search.resolve_geometry(target), target)
        path.write_text("mesh=object.geometry\n", encoding="utf-8")
        with self.assertRaises(search.SearchError):
            search.resolve_geometry(path)


    def test_material_channels_are_scored_independently(self):
        original = self.doc["assets"][0]
        changed = copy.deepcopy(original)
        for view in changed["features"].values():
            count = self.doc["grid"] ** 2
            view["orm"] = base64.b64encode(bytes([0, 220, 255] * count)).decode()
            view["normal"] = base64.b64encode(bytes([255, 127, 127] * count)).decode()
            view["emissive"] = base64.b64encode(bytes([255, 255, 255] * count)).decode()
        scores = search.visual_scores(original, changed, self.doc["grid"])
        self.assertEqual(scores["shape"], 1)
        self.assertEqual(scores["color"], 1)
        for name in ("normal", "roughness", "metallic", "occlusion", "emissive"):
            self.assertLess(scores[name], 0.75, name)

    def test_cli_json_search_and_mock_geometry_query(self):
        target = self.root / "new.geometry"
        target.write_bytes(b"HSEM\x03\x00\x00\x00")
        tiles = {}
        for view in search.VIEWS:
            tiles[view] = {}
            for name, rgba in (("coverage", (255, 255, 255, 255)),
                               ("albedo", (250, 20, 20, 255)),
                               ("normal", (127, 127, 255, 255)),
                               ("orm", (255, 100, 25, 255)),
                               ("emissive", (0, 0, 0, 255))):
                tiles[view][name] = search.Image.new("RGBA", (6, 6), rgba)
        fake = {"guid": "generated", "aabb_min": [0, 0, 0], "aabb_max": [1, 1, 1],
                "captures": tiles}
        with patch.object(search.capture, "prepare", return_value=fake) as prepare:
            d = search.descriptor_for_geometry(target, self.doc)
            prepare.assert_called_once_with(target, self.doc["face_size"])
            self.assertEqual(d["bounds"]["size"], [1, 1, 1])
            scores = search.search(self.doc, d, limit=2)
            self.assertEqual(scores[0]["score"], 100)
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(search.cli(["search", "--index", str(self.index_path),
                                         "--asset", "red.geometry", "--json"]), 0)
        cli_result = json.loads(output.getvalue())
        self.assertEqual(cli_result["results"][0]["source"], "assets/red_copy.geometry")

    def test_ranking_uses_unrounded_similarity(self):
        # Both display as 50.00, but the genuinely better match must win.
        doc = {"grid": 2, "assets": [{"source": "a.geometry"},
                                     {"source": "z.geometry"}]}
        def fake_scores(query, candidate, grid):
            return {"shape": .50004 if candidate["source"] == "z.geometry" else .5}
        with patch.object(search, "visual_scores", side_effect=fake_scores):
            rows = search.search(doc, {}, weights={"shape": 1}, size_ratio=0)
        self.assertEqual([r["source"] for r in rows], ["z.geometry", "a.geometry"])
        self.assertEqual([r["score"] for r in rows], [50.0, 50.0])

    def test_invalid_size_ratio_rejected(self):
        with self.assertRaises(search.SearchError):
            search.search(self.doc, self.doc["assets"][0], size_ratio=.5)
        with self.assertRaises(search.SearchError):
            search.search(self.doc, self.doc["assets"][0], size_ratio=-1)
        with self.assertRaises(search.SearchError):
            search.search(self.doc, self.doc["assets"][0], size_ratio=float("nan"))


if __name__ == "__main__":
    unittest.main()
