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


if __name__ == "__main__":
    unittest.main()
