"""Hardware-free checks for the perf CLI. Run: python -m unittest discover -s tools/tests."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("perf_harness", Path(__file__).resolve().parents[1] / "perf_harness.py")
mod = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mod)


class HarnessTests(unittest.TestCase):
    def test_positive_frames(self):
        self.assertEqual(mod.positive("12"), 12)
        with self.assertRaises(Exception):
            mod.positive("0")

    def test_metrics_pattern(self):
        log = "[perf] avg frame 12.3456 ms over 120 frames\n[perf] sections poll 0.100 ms game 0.200 ms draw 10.500 ms"
        self.assertEqual(mod.PERF.findall(log), [("12.3456", "120")])
        self.assertEqual(mod.SECTIONS.findall(log), [("0.100", "0.200", "10.500")])

    def test_compare(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = {"mode": "bench", "frames": 120, "app_args": [], "environment": {},
                    "summary": {"median_ms": 10.0}}
            candidate = dict(base, summary={"median_ms": 11.0})
            a, b = root / "base.json", root / "new.json"
            a.write_text(json.dumps(base))
            b.write_text(json.dumps(candidate))
            self.assertEqual(mod.main(["compare", "--baseline", str(a), "--candidate", str(b)]), 2)
            self.assertEqual(mod.main(["compare", "--baseline", str(a), "--candidate", str(b),
                                       "--max-regression", "11"]), 0)

    def test_scene_arguments(self):
        from types import SimpleNamespace
        opts = SimpleNamespace(exe=Path("/tmp/App"), project=None, pak=Path("/tmp/scene.pak"),
                               frames=64, app_arg=["--no-vfs"])
        # Path.resolve() is platform-dependent (POSIX keeps /tmp, Windows
        # anchors it to the current drive); compare against the SAME
        # resolution the harness performs instead of a literal string.
        self.assertEqual(mod.command(opts)[-8:], ["--pak", str(opts.pak.resolve()), "--disable_control",
                                                    "--bench", "64", "--present", "immediate", "--no-vfs"])

    def test_parse_resolutions(self):
        import math
        presets = mod.parse_resolutions("720p,1080p,1440p,4k")
        self.assertEqual([p["label"] for p in presets], ["720p", "1080p", "1440p", "4k"])
        self.assertEqual((presets[0]["width"], presets[0]["height"]), (1280, 720))
        self.assertAlmostEqual(presets[0]["render_scale"], 720 / 1080)
        self.assertEqual(presets[1]["render_scale"], 1.0)
        self.assertAlmostEqual(presets[2]["render_scale"], 1440 / 1080)
        self.assertEqual(presets[3]["render_scale"], 2.0)
        # Explicit WxH (16:9) and de-duplication with stable order.
        self.assertEqual(mod.parse_resolutions("1600x900,720p,720p")[0]["width"], 1600)
        self.assertEqual(len(mod.parse_resolutions("1080p,1080p")), 1)
        # 4k and 2160p are the same extent under different names.
        self.assertEqual(mod.parse_resolutions("2160p")[0]["width"], 3840)
        # Non-16:9 cannot be expressed by the single-scale pin.
        with self.assertRaises(ValueError):
            mod.parse_resolutions("1920x1200")
        # Inside the engine's 0.25..2.0 clamp: fine (16:9 WxH).
        self.assertEqual(mod.parse_resolutions("800x450")[0]["height"], 450)
        # Outside the engine's 0.25..2.0 clamp: refused.
        with self.assertRaises(ValueError):
            mod.parse_resolutions("400x225")    # scale 0.208 < 0.25
        with self.assertRaises(ValueError):
            mod.parse_resolutions("5120x2880")  # scale 2.667 > 2.0
        with self.assertRaises(ValueError):
            mod.parse_resolutions("bad")
        with self.assertRaises(ValueError):
            mod.parse_resolutions("")

    def test_priming_command(self):
        argv = ["App.exe", "--pak", "s.pak", "--disable_control", "--bench", "120",
                "--present", "immediate"]
        primed = mod.priming_command(argv)
        self.assertEqual(primed[primed.index("--bench") + 1], str(mod.PRIMING_BENCH_FRAMES))
        self.assertEqual(primed[0], "App.exe")
        # The measured command is untouched.
        self.assertEqual(argv[argv.index("--bench") + 1], "120")
        with self.assertRaises(ValueError):
            mod.priming_command(["App.exe", "--pak", "s.pak"])

    def test_priming_targets(self):
        plan = [
            {"env": {"A": "1", "RENDER_SCALE": "0.667"}, "resolution": "720p"},
            {"env": {"A": "1", "RENDER_SCALE": "0.667"}, "resolution": "720p"},
            {"env": {"A": "1", "RENDER_SCALE": "1"}, "resolution": "1080p"},
            {"env": {"A": "1"}, "resolution": None},
        ]
        targets = mod.priming_targets(plan)
        # One priming run per DISTINCT environment (order preserved).
        self.assertEqual([t["resolution"] for t in targets], ["720p", "1080p", None])

    def test_compare_multires(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def result(medians):
                return {"mode": "bench", "frames": 120, "app_args": [], "environment": {},
                        "scenario": "sponza-atrium", "source_args": ["--project", "x"],
                        "resolutions": [
                            {"label": lab, "summary": {"median_ms": ms}}
                            for lab, ms in zip(("720p", "1080p", "1440p", "4k"), medians)]}
            a, b = root / "a.json", root / "b.json"
            # 10% slower at 4k only: ANY resolution over threshold fails.
            a.write_text(json.dumps(result([1.0, 1.5, 2.2, 4.0])))
            b.write_text(json.dumps(result([1.0, 1.5, 2.2, 4.45])))
            self.assertEqual(mod.main(["compare", "--baseline", str(a), "--candidate", str(b)]), 2)
            # All within 5%: passes.
            b.write_text(json.dumps(result([1.01, 1.5, 2.25, 4.1])))
            self.assertEqual(mod.main(["compare", "--baseline", str(a), "--candidate", str(b)]), 0)
            # Different resolution lists refuse to compare.
            a.write_text(json.dumps(result([1.0, 1.5, 2.2, 4.0])))
            b.write_text(json.dumps(result([1.0, 1.5, 2.2])))
            with self.assertRaises(SystemExit):
                mod.main(["compare", "--baseline", str(a), "--candidate", str(b)])


if __name__ == "__main__":
    unittest.main()
