"""Contract test: shader build and hot reload consume the same checked-in list."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = ROOT / "engine" / "shaders" / "variants.txt"


def read_variants():
    records = []
    for line, raw in enumerate(SPEC.read_text(encoding="utf-8").splitlines(), 1):
        value = raw.strip()
        if not value or value.startswith("#"):
            continue
        parts = value.split("|")
        if len(parts) < 3:
            raise ValueError(f"invalid variant on line {line}")
        shader, suffix, *defines = parts
        if not shader.endswith(".hlsl") or not suffix.startswith("."):
            raise ValueError(f"invalid shader/suffix on line {line}")
        if not all(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*=[A-Za-z_0-9+.-]+", d)
                   for d in defines):
            raise ValueError(f"invalid define on line {line}")
        records.append((shader, suffix, tuple(defines)))
    return records


class ShaderVariantsTests(unittest.TestCase):
    def test_unique_and_stable_variants(self):
        rows = read_variants()
        self.assertGreater(len(rows), 20)
        keys = [(shader, suffix) for shader, suffix, _ in rows]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertIn(("hzb_spd.cs.hlsl", ".min", ("HZB_REDUCE_MAX=0",)), rows)
        for shader in ("default_lit.frag.hlsl", "gfxmesh_lit.frag.hlsl"):
            for suffix in (".sg5", ".sg6", ".sg9", ".sg12", ".gi0",
                           ".gi2", ".gi3", ".std", ".gi0std",
                           ".gi2std", ".gi3std"):
                self.assertIn((shader, suffix), keys)

    def test_build_and_runtime_use_generated_manifest(self):
        cmake = (ROOT / "engine" / "CMakeLists.txt").read_text(encoding="utf-8")
        hot = (ROOT / "engine" / "src" / "Utils" /
               "shader_hot_reload.cpp").read_text(encoding="utf-8")
        self.assertIn("shaders/variants.txt", cmake)
        self.assertIn("${TOURMALINE_SHADER_VARIANTS}", cmake)
        self.assertIn('#include "shader_variants.inc"', hot)
        self.assertNotIn('"default_lit.frag.hlsl", {', hot)


if __name__ == "__main__":
    unittest.main()
