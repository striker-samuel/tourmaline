"""Offline scenario GI quality tests (GPU not required)."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    import numpy as np
except ImportError:
    np = None
import quality_compare as qc


@unittest.skipIf(np is None, "numpy optional; install to run quality tests")
class QualityMetricsTests(unittest.TestCase):
    def sample(self, v):
        return {"irradiance": np.full((4, 4, 3), v, dtype=np.float32),
                "surface_id": np.array([[0, 0, 1, 1]] * 4),
                "wall_mask": np.array([[1, 0, 0, 0]] * 4, dtype=bool)}

    def test_identical(self):
        r = qc.compare(self.sample(0.5), self.sample(0.5))
        self.assertEqual(r["regions"]["all"]["log_luma_stops"]["max"], 0)
        self.assertEqual(r["regions"]["wall"]["log_luma_stops"]["max"], 0)
        self.assertEqual(r["regions"]["all"]["chroma_distance"]["max"], 0)
        self.assertEqual(r["multiscale"]["2"]["max"], 0)

    def test_exposure_stops(self):
        r = qc.compare(self.sample(1.0), self.sample(2.0), epsilon=1e-9)
        self.assertAlmostEqual(r["regions"]["all"]["log_luma_stops"]["mean"], 1, places=5)
        self.assertAlmostEqual(r["regions"]["all"]["brightening_stops"]["mean"], 1, places=5)

    def test_chroma_detects_equal_luminance_shift(self):
        a = self.sample(0.5)
        b = self.sample(0.5)
        # Red/green adjustment chosen to preserve luminance.
        b["irradiance"][:, :, :] = [0.7, 0.5 - 0.2 * 0.2126 / 0.7152, 0.5]
        r = qc.compare(a, b)
        self.assertLess(r["regions"]["all"]["log_luma_stops"]["mean"], 1e-6)
        self.assertGreater(r["regions"]["all"]["chroma_distance"]["mean"], 0.01)

    def test_geometry_mismatch_fails(self):
        a, b = self.sample(1), self.sample(1)
        b["surface_id"][0, 0] = 4
        with self.assertRaises(ValueError):
            qc.compare(a, b)

    def test_masked_region(self):
        a, b = self.sample(1), self.sample(1)
        b["irradiance"][:, 0, :] = 2
        r = qc.compare(a, b, epsilon=1e-9)
        self.assertGreater(r["regions"]["wall"]["log_luma_stops"]["mean"],
                           r["regions"]["all"]["log_luma_stops"]["mean"])

    def test_shape_and_invalid_rejected(self):
        a, b = self.sample(1), self.sample(1)
        b["irradiance"] = b["irradiance"][:3]
        with self.assertRaises(ValueError):
            qc.compare(a, b)


if __name__ == "__main__":
    unittest.main()
