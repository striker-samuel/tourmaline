# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""CPU-only fixture tests for the .geometry capture atlas processor."""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import geometry_capture_atlas as atlas


def u32(v):
    return struct.pack("<I", v)


def string(s):
    b = s.encode("utf-8")
    return u32(len(b)) + b


def identity():
    return struct.pack("<16f", *atlas.IDENTITY)


def geometry_fixture(path, material_guid="material-test", include_light=False):
    """MESH v3 with three vertices, one face, and optional light/camera records."""
    out = bytearray(u32(0x4D455348) + u32(3))
    out += u32(1) + struct.pack("<ii", -1, 0)  # node hierarchy
    out += u32(1) + string("triangle")          # name
    out += u32(1) + identity()                   # raw local matrix
    out += u32(1) + b"\x00" + u32(1) + b"\x00"  # flags
    out += u32(1) + u32(0) + u32(0) + string(material_guid)
    out += u32(1 if include_light else 0)
    if include_light:
        out += u32(0) + u32(0) + struct.pack("<3f", 1, 1, 1)
        out += struct.pack("<4f", 5, 2, .2, .4) + u32(0) + string("sun")
    out += u32(1) + u32(0) + u32(0) + struct.pack("<4f", 70, 1, .1, 100) + string("cam")
    out += u32(1) + u32(3) + u32(3) + u32(1)
    out += struct.pack("<IIi", 0, 3, 0)
    out += struct.pack("<6f", -.5, -.5, 0, .5, .5, 0)
    out += struct.pack("<9f", -.5, -.5, 0, .5, -.5, 0, 0, .5, 0)
    packed_n = 0xFFFFFFFF  # +Z oct normal
    for _ in range(3):
        out += struct.pack("<IIII", 0xFFFFFFFF, 0, packed_n, 0x80008001)
    out += u32(0) + struct.pack("<III", 0, 1, 2)
    path.write_bytes(out)


@unittest.skipIf(atlas.Image is None, "Pillow not installed")
class GeometryCaptureAtlasTests(unittest.TestCase):
    def test_capture_and_rectangles_with_light_camera(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            g = root / "triangle.geometry"
            geometry_fixture(g, include_light=True)
            (root / "triangle.mat0.material").write_text(
                "guid=material-test\nbaseColor=1 0 0 1\nroughness=0.3\nmetallic=0.7\n",
                encoding="utf-8")
            (root / "triangle.gltf.meta").write_text(
                "geometry=triangle.geometry geometry-guid triangle\n", encoding="utf-8")
            asset = atlas.prepare(g, 16)
            self.assertEqual(asset["triangles"], 1)
            self.assertEqual(asset["guid"], "geometry-guid")
            self.assertGreater(asset["captures"]["+Z"]["coverage"].getbbox() is not None, False)
            self.assertIsNone(asset["captures"]["-X"]["coverage"].getbbox())
            index = atlas.write_atlas([asset], root / "out", 16, 64, 4, 5)
            self.assertEqual(len(index["mips"]), 5)
            self.assertEqual(index["assets"][0]["materials"][0]["guid"], "material-test")
            for mip in index["mips"]:
                self.assertTrue((root / "out" / mip["files"]["albedo"]).is_file())
            for mip in index["assets"][0]["mips"]:
                page = index["mips"][mip["level"]]
                for rect in mip["views"].values():
                    self.assertGreater(rect["width"], 0)
                    self.assertLessEqual(rect["x"] + rect["width"], page["width"])
                    self.assertLessEqual(rect["y"] + rect["height"], page["height"])
            disk = json.loads((root / "out" / "index.json").read_text())
            self.assertEqual(disk["assets"][0]["geometry_guid"], "geometry-guid")

    def test_malformed_geometry_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.geometry"
            geometry_fixture(path)
            path.write_bytes(path.read_bytes()[:-4])
            with self.assertRaises(atlas.GeometryError):
                atlas.load_geometry(path)

    def test_missing_material_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "triangle.geometry"
            geometry_fixture(path)
            with self.assertRaisesRegex(atlas.GeometryError, "material GUID"):
                atlas.prepare(path, 8)

    def test_deterministic_packing(self):
        self.assertEqual(atlas.next_power_two(1), 1)
        self.assertEqual(atlas.next_power_two(65), 128)
        self.assertEqual(atlas.oct_decode(0xFFFFFFFF), (0.0, 0.0, -1.0))


if __name__ == "__main__":
    unittest.main()
