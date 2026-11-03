#!/usr/bin/env python3
"""Pillow-only, GPU-free tests for decoded screenshot comparison."""
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

try:
    from PIL import Image
except ImportError:
    Image = None

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import pixel_compare as pixel


@unittest.skipUnless(Image is not None, "Pillow is optional")
class PixelCompareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.a = Path(self.temp.name) / "a.png"
        self.b = Path(self.temp.name) / "b.png"
        Image.new("RGB", (4, 4), "black").save(self.a)
        Image.new("RGB", (4, 4), "black").save(self.b)

    def test_identical_and_differing(self):
        result = pixel.compare(self.a, self.b, stride=1)
        self.assertTrue(result["equal"])
        self.assertEqual(result["sampled_pixels"], 16)
        self.assertEqual(result["reference_pixel_sha256"],
                         result["candidate_pixel_sha256"])
        image = Image.open(self.b).convert("RGB")
        image.putpixel((1, 1), (255, 255, 255))
        image.save(self.b)
        result = pixel.compare(self.a, self.b, threshold=6, stride=1,
                               diff_out=Path(self.temp.name) / "diff.png")
        self.assertEqual(result["differing_pixels"], 1)
        self.assertFalse(result["equal"])
        self.assertTrue(Path(result["diff_image"]).is_file())

    def test_jsonl_piped_pairs(self):
        out = io.StringIO()
        with patch.object(sys, "stdin", io.StringIO(f"{self.a}\t{self.b}\n")):
            with redirect_stdout(out):
                code = pixel.main(["--stdin", "--jsonl", "--stride", "1"])
        self.assertEqual(code, 0)
        self.assertTrue(json.loads(out.getvalue())["equal"])

    def test_dimension_error(self):
        Image.new("RGB", (2, 2), "black").save(self.b)
        self.assertEqual(pixel.main([str(self.a), str(self.b), "--json"]), 2)


if __name__ == "__main__":
    unittest.main()
