import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmark_profiles import resolve_profile, profile_applied


class HardwareProfileTests(unittest.TestCase):
    def test_presets(self):
        self.assertEqual(resolve_profile("native"), {"name": "native", "cpu_limit": None, "vram_mb": None})
        self.assertEqual(resolve_profile("ps4")["cpu_limit"], 7)
        self.assertEqual(resolve_profile("ps5")["vram_mb"], 12288)
        self.assertEqual(resolve_profile("rtx4060")["vram_mb"], 8192)

    def test_overrides(self):
        profile = resolve_profile("ps5", 4, 2048)
        self.assertEqual((profile["cpu_limit"], profile["vram_mb"]), (4, 2048))
        for cpu, vram in [(0, 1), (1025, 1), (1, 0), (1, 1048577)]:
            with self.assertRaises(ValueError):
                resolve_profile("native", cpu, vram)

    def test_requires_exact_startup_markers(self):
        profile = resolve_profile("ps4")
        log = "[Benchmark] logical CPU limit=7\n[Benchmark] VMA device-local limit=4096 MiB\n"
        self.assertTrue(profile_applied(profile, log))
        self.assertFalse(profile_applied(profile, log.replace("4096", "40960")))
        self.assertFalse(profile_applied(profile, ""))
        self.assertTrue(profile_applied(resolve_profile("native"), ""))
