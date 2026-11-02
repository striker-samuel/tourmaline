"""CPU-only tests: private output locations and 10-frame Nsight CLI planning."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "perf_harness.py"
spec = importlib.util.spec_from_file_location("perf_harness_paths", MODULE)
perf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(perf)


class NsightTracePathTests(unittest.TestCase):
    def test_saved_paths_are_per_user_and_can_be_overridden(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = root / "settings" / "paths.json"
            captures = root / "captures"
            scratch = root / "scratch"
            with contextlib.redirect_stdout(io.StringIO()):
                rc = perf.main(["trace", "--paths-config", str(config), "--save-paths",
                                "--capture-dir", str(captures), "--scratch-dir", str(scratch)])
            self.assertEqual(rc, 0)
            self.assertFalse(captures.exists())
            self.assertFalse(scratch.exists())
            data = json.loads(config.read_text(encoding="utf-8"))
            self.assertEqual(data["capture_dir"], str(captures.resolve()))
            self.assertEqual(data["scratch_dir"], str(scratch.resolve()))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(perf.main(["trace", "--paths-config", str(config), "--show-paths"]), 0)
            self.assertEqual(json.loads(output.getvalue())["scratch_dir"], str(scratch.resolve()))
            other = root / "other-disk"
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(perf.main(["trace", "--paths-config", str(config), "--show-paths",
                                            "--scratch-dir", str(other)]), 0)
            self.assertEqual(json.loads(output.getvalue())["scratch_dir"], str(other.resolve()))
            self.assertEqual(json.loads(config.read_text(encoding="utf-8"))["scratch_dir"],
                             str(scratch.resolve()))

    def test_ten_frame_trace_plan_uses_selected_disks_without_running(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "out" / "build" / "x64-Dev"
            (build / "content" / "paks").mkdir(parents=True)
            (build / "Benchmark.exe").write_bytes(b"not a real executable")
            (build / "content" / "paks" / "test.pak").write_bytes(b"test input")
            catalog = root / "scenarios.json"
            catalog.write_text(json.dumps({"schema_version": 1, "scenarios": [{
                "id": "ssr-test", "source_args": ["--pak", "content/paks/test.pak"],
                "environment": {"TOURMALINE_SSR": "1"},
                "app_args": [], "frames": 180
            }]}), encoding="utf-8")
            captures = root / "fast-captures"
            scratch = root / "fast-scratch"
            log = io.StringIO()
            with contextlib.redirect_stdout(log):
                rc = perf.main(["trace", "--scenario", "ssr-test", "--catalog", str(catalog),
                                "--exe", str(build / "Benchmark.exe"), "--dry-run",
                                "--capture-dir", str(captures), "--scratch-dir", str(scratch),
                                "--paths-config", str(root / "missing-settings.json")])
            self.assertEqual(rc, 0)
            result = json.loads(log.getvalue())
            cmd = result["command"]
            self.assertIn("--start-after-frames=10", cmd)
            self.assertIn("--limit-to-frames=10", cmd)
            self.assertIn("--num-frames=10", cmd)
            self.assertIn("--bench 12", " ".join(cmd))
            self.assertTrue(any(x.startswith("--output-dir=" + str(captures)) for x in cmd))
            self.assertTrue(any("TOURMALINE_SSR=1" in x for x in cmd))
            self.assertTrue(any("TMP=" + str(scratch) in x for x in cmd))
            self.assertFalse(captures.exists(), "dry-run must not write outputs")
            self.assertFalse(scratch.exists(), "dry-run must not write scratch files")


if __name__ == "__main__":
    unittest.main()
