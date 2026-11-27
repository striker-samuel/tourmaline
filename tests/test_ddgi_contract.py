# SPDX-License-Identifier: MIT
"""CPU contract checks for DDGI; shader execution still requires GPU validation."""
import math
from pathlib import Path
import unittest

SHADERS = Path(__file__).resolve().parents[1] / "engine" / "shaders"


def oct_tap(x, y, n):
    if x < 0 or x >= n:
        x = -1 - x if x < 0 else 2 * n - 1 - x
        y = n - 1 - y
    if y < 0 or y >= n:
        y = -1 - y if y < 0 else 2 * n - 1 - y
        x = n - 1 - x
    return x, y


class DdgiContractTests(unittest.TestCase):
    def test_octahedral_seams_and_corners(self):
        for n in (8, 16):
            for i in range(n):
                self.assertEqual(oct_tap(-1, i, n), (0, n - 1 - i))
                self.assertEqual(oct_tap(n, i, n), (n - 1, n - 1 - i))
                self.assertEqual(oct_tap(i, -1, n), (n - 1 - i, 0))
                self.assertEqual(oct_tap(i, n, n), (n - 1 - i, n - 1))
            self.assertEqual(oct_tap(-1, -1, n), (n - 1, n - 1))
            for x in range(-1, n + 1):
                for y in range(-1, n + 1):
                    self.assertTrue(all(0 <= v < n for v in oct_tap(x, y, n)))

    def test_constant_environment_irradiance(self):
        cosines = [0.17, 0.93, 0.42, 0.0]
        radiance = 2.5
        e = sum(c * radiance for c in cosines) * math.pi / sum(cosines)
        self.assertAlmostEqual(e, math.pi * radiance)

    def test_history_offset_contract(self):
        common = (SHADERS / "ddgi_common.hlsli").read_text(encoding="utf-8")
        self.assertIn("DDGI_IRR_STRIDE = DDGI_IRR_BINS + 1", common)
        for name in ("ddgi_sample.hlsli", "ddgi_trace_shared.hlsli",
                     "ddgi_trace.cs.hlsl", "ddgi_blend.cs.hlsl"):
            source = (SHADERS / name).read_text(encoding="utf-8")
            self.assertIn("DdgiHistoryOffset(", source, name)
        for name in ("ddgi_trace.cs.hlsl", "ddgi_blend.cs.hlsl"):
            source = (SHADERS / name).read_text(encoding="utf-8")
            self.assertIn("DdgiSameOrigin(", source, name)

    def test_reset_and_scroll_contract(self):
        blend = (SHADERS / "ddgi_blend.cs.hlsl").read_text(encoding="utf-8")
        self.assertNotIn("seedDist", blend)
        self.assertNotIn("seedIrr", blend)
        self.assertIn("if (carry && sp.reset == 0u)", blend)
        sdf = (SHADERS / "ddgi_trace_sdf.cs.hlsl").read_text(encoding="utf-8")
        self.assertIn("if (sp.reset == 0u)\n                    eIndirect", sdf)


if __name__ == "__main__":
    unittest.main()
