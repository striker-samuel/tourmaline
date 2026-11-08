#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Hardware-free checks for the temporal parity gate CLI.

Run: python -m unittest discover -s tools/tests
"""
import importlib.util
import sys
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
# gpu_scene_temporal_parity imports its sibling pixel_compare module; the
# importlib load below does not put tools/ on sys.path the way executing
# the script does.
sys.path.insert(0, str(TOOLS_DIR))

SPEC = importlib.util.spec_from_file_location(
    "gpu_scene_temporal_parity", TOOLS_DIR / "gpu_scene_temporal_parity.py")
mod = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mod)


class WitnessTests(unittest.TestCase):
    def test_count(self):
        log = ("[GfxFilter] dispatch ok\n"
               "[GpuSceneTemporal] engaged: commands=2001 slots=4 history=1\n"
               "[GfxProbe] avg/frame\n"
               "[GpuSceneTemporal] engaged: commands=2001 slots=4 history=1 hiz=960x540 mips=10\n")
        self.assertEqual(mod.witness_count(log), 2)
        self.assertEqual(mod.witness_count(""), 0)
        self.assertEqual(mod.witness_count("[GfxFilter] dispatch ok\n"), 0)


class LegPinTests(unittest.TestCase):
    def test_on_legs_carry_no_extra_pins(self):
        self.assertEqual(mod.leg_pins("on-a", {}, "0x20b"), {})
        self.assertEqual(mod.leg_pins("on-b", {"TOURMALINE_RNG_SEED": "1234"}, "0x20b"),
                         {"TOURMALINE_RNG_SEED": "1234"})

    def test_temporal_off_isolates_only_the_temporal_chain(self):
        pins = mod.leg_pins("temporal-off", {"TOURMALINE_RNG_SEED": "1234"}, "0x20b")
        self.assertEqual(pins, {"TOURMALINE_RNG_SEED": "1234",
                                "TOURMALINE_GPU_SCENE_TEMPORAL": "0"})
        self.assertNotIn(mod.CULL_FLAGS_NAME, pins)

    def test_hiz_off_clears_only_the_occlusion_bit(self):
        pins = mod.leg_pins("hiz-off", {}, "0x20b")
        self.assertEqual(pins, {mod.CULL_FLAGS_NAME: "0x20b"})
        self.assertEqual(int(pins[mod.CULL_FLAGS_NAME], 0) & mod.HZB_OCCLUSION_BIT, 0)
        self.assertNotIn("TOURMALINE_GPU_SCENE_TEMPORAL", pins)

    def test_hiz_pin_matches_engine_runconfig_name(self):
        # RunConfig reads CULLFLAGS, not CULL_FLAGS. A wrong pin makes
        # the Hi-Z OFF leg vacuous while the image comparison still passes.
        self.assertEqual(mod.CULL_FLAGS_NAME, "TOURMALINE_CULLFLAGS")
        self.assertEqual(mod.leg_pins("hiz-off", {}, "0x20b"),
                         {"TOURMALINE_CULLFLAGS": "0x20b"})

    def test_default_hiz_off_flags_clear_the_bit(self):
        self.assertEqual(int(f"{mod.DEFAULT_HIZ_OFF_FLAGS:#x}", 0) & mod.HZB_OCCLUSION_BIT, 0)

    def test_unknown_leg_rejected(self):
        with self.assertRaises(ValueError):
            mod.leg_pins("on-c", {}, "0x20b")


class EvaluateTests(unittest.TestCase):
    @staticmethod
    def leg(status="captured", witness=1, pixels="x.png"):
        return {"status": status, "witness": witness, "pixels": pixels}

    def setUp(self):
        self.saved = mod.pixel_compare.compare
        self.comparisons = []
        self.diff_counts = []

        def fake_compare(left, right):
            self.comparisons.append((left, right))
            differing = self.diff_counts.pop(0) if self.diff_counts else 0
            return {"equal": differing == 0, "differing_pixels": differing,
                    "sampled_pixels": 518400}

        mod.pixel_compare.compare = fake_compare

    def tearDown(self):
        mod.pixel_compare.compare = self.saved

    def legs(self, **overrides):
        base = {"on-a": self.leg(), "on-b": self.leg(), "temporal-off": self.leg(witness=0)}
        base.update(overrides)
        return base

    def test_failed_capture_short_circuits(self):
        legs = {"on-a": self.leg(status="app_failed"),
                "on-b": self.leg(), "temporal-off": self.leg(witness=0)}
        verdict, ok = mod.evaluate(legs)
        self.assertFalse(ok)
        self.assertFalse(verdict["legs-captured"]["pass"])

    def test_witness_must_be_present_on_and_absent_off(self):
        verdict, ok = mod.evaluate(self.legs())
        self.assertTrue(verdict["witness-on"]["pass"])
        self.assertTrue(verdict["witness-off"]["pass"])

    def test_witness_in_the_off_log_fails_the_gate(self):
        # Identical pixels but the OFF run also dispatched the chain: the
        # isolation switch is broken, the comparison is meaningless.
        legs = self.legs(**{"temporal-off": self.leg(witness=2)})
        verdict, ok = mod.evaluate(legs)
        self.assertFalse(ok)
        self.assertFalse(verdict["witness-off"]["pass"])

    def test_missing_witness_on_fails_even_with_identical_pixels(self):
        legs = self.legs(**{"on-a": self.leg(witness=0), "on-b": self.leg(witness=0)})
        verdict, ok = mod.evaluate(legs)
        self.assertFalse(ok)
        self.assertFalse(verdict["witness-on"]["pass"])

    def test_hiz_leg_optional(self):
        verdict, ok = mod.evaluate(self.legs())
        self.assertTrue(ok)
        self.assertNotIn("hiz-parity", verdict)

    def test_parity_within_floor_plus_margin_passes(self):
        # The indexed lane's own noise floor is 8; the temporal leg adds
        # 4 more -> within margin.
        self.diff_counts = [8, 12]
        verdict, ok = mod.evaluate(self.legs())
        self.assertTrue(ok)
        self.assertTrue(verdict["determinism"]["pass"])
        self.assertTrue(verdict["temporal-parity"]["pass"])
        self.assertIn("floor 8", verdict["temporal-parity"]["detail"])

    def test_strict_bit_exact_on_on_rejects_noise_even_below_default_cap(self):
        self.diff_counts = [8, 0]
        verdict, ok = mod.evaluate(self.legs(), require_bit_exact=True)
        self.assertFalse(ok)
        self.assertFalse(verdict["determinism"]["pass"])
        self.assertTrue(verdict["temporal-parity"]["pass"])

    def test_strict_bit_exact_temporal_and_hiz_reject_single_pixel(self):
        self.diff_counts = [0, 1, 1]
        legs = self.legs(**{"hiz-off": self.leg(witness=1)})
        verdict, ok = mod.evaluate(legs, require_bit_exact=True)
        self.assertFalse(ok)
        self.assertTrue(verdict["determinism"]["pass"])
        self.assertFalse(verdict["temporal-parity"]["pass"])
        self.assertFalse(verdict["hiz-parity"]["pass"])

    def test_strict_bit_exact_accepts_all_zero_diffs_and_engaged_hiz(self):
        self.diff_counts = [0, 0, 0]
        legs = self.legs(**{"hiz-off": self.leg(witness=1)})
        verdict, ok = mod.evaluate(legs, require_bit_exact=True)
        self.assertTrue(ok)
        self.assertTrue(verdict["hiz-parity"]["pass"])

    def test_parity_beyond_floor_plus_margin_fails(self):
        self.diff_counts = [8, 40]
        verdict, ok = mod.evaluate(self.legs())
        self.assertFalse(ok)
        self.assertFalse(verdict["temporal-parity"]["pass"])

    def test_noise_floor_above_cap_fails_outright(self):
        # 200 differing pixels ON vs ON: a determinism regression, not
        # noise. The gate fails on the determinism leg itself; a temporal
        # leg merely MATCHING that floor is consistent with the noise,
        # but the overall verdict is still a failure.
        self.diff_counts = [200, 200]
        verdict, ok = mod.evaluate(self.legs())
        self.assertFalse(ok)
        self.assertFalse(verdict["determinism"]["pass"])


if __name__ == "__main__":
    unittest.main()
