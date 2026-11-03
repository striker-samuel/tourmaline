#!/usr/bin/env python3
"""Mocked-renderer Tourmaline capture tests; never need GPU or a window."""
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace

try:
    from PIL import Image
except ImportError:
    Image = None

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tourcapture


@unittest.skipUnless(Image is not None, "Pillow required for capture tests")
class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.exe = self.root / "App"
        self.exe.write_text("fake", encoding="utf-8")
        (self.root / "content").mkdir()
        (self.root / "content" / "game.tapp").write_text("fake", encoding="utf-8")
        self.args = ["--exe", str(self.exe), "--project", "content/game.tapp",
                     "--camera-pos", "0,1,2", "--camera-fwd", "0,0,1",
                     "--gpu-tag", "TestGPU-Driver1", "--capture-frame", "12", "--frames", "20",
                     "--json"]

    def invoke(self, extra=None, renderer=None):
        args = [*self.args, "--output", str(self.root / (extra or {}).get("out", "output"))]
        if extra:
            if extra.get("baseline"):
                args += ["--baseline", str(extra["baseline"])]
            if extra.get("threshold") is not None:
                args += ["--threshold", str(extra["threshold"])]
        out = io.StringIO()
        if renderer is not None:
            with patch.object(tourcapture.subprocess, "run", side_effect=renderer):
                with redirect_stdout(out):
                    rc = tourcapture.main(args)
        else:
            with redirect_stdout(out):
                rc = tourcapture.main(args)
        return rc, json.loads(out.getvalue())

    def writer(self, rgb=(0, 0, 0)):
        def do_write(argv, cwd, **kwargs):
            self.assertEqual(kwargs["env"]["TOURMALINE_CAPTURE"], "12")
            dest = Path(cwd) / "exports" / "screenshot_0001.png"
            dest.parent.mkdir(exist_ok=True)
            Image.new("RGB", (4, 4), rgb).save(dest)
            return SimpleNamespace(returncode=0)
        return do_write

    def test_dry_run(self):
        with redirect_stdout(io.StringIO()) as out:
            rc = tourcapture.main([*self.args, "--output", str(self.root / "dry"), "--dry-run"])
        self.assertEqual(rc, 0)
        self.assertFalse((self.root / "dry").exists())

    def test_capture_and_compatible_comparison(self):
        rc, first = self.invoke({"out": "first"}, self.writer())
        self.assertEqual(rc, 0)
        self.assertEqual(first["status"], "captured")
        base = self.root / "first" / "capture.json"
        rc, second = self.invoke({"out": "second", "baseline": base}, self.writer())
        self.assertEqual(rc, 0)
        self.assertEqual(second["status"], "passed")
        rc, third = self.invoke({"out": "third", "baseline": base}, self.writer((255, 255, 255)))
        self.assertEqual(rc, 1)
        self.assertEqual(third["status"], "different")
        self.assertTrue((self.root / "third" / "diff.png").is_file())

    def test_no_fresh_image_fails(self):
        rc, result = self.invoke({"out": "none"}, lambda *a, **k: SimpleNamespace(returncode=0))
        self.assertEqual(rc, 4)
        self.assertEqual(result["status"], "no_capture")

    def test_bad_baseline_gpu_pin(self):
        rc, _ = self.invoke({"out": "first"}, self.writer())
        self.assertEqual(rc, 0)
        base = self.root / "first" / "capture.json"
        args = [*self.args, "--output", str(self.root / "other"),
                "--baseline", str(base)]
        idx = args.index("--gpu-tag")
        args[idx + 1] = "OtherGPU"
        with redirect_stdout(io.StringIO()):
            rc = tourcapture.main(args)
        self.assertEqual(rc, 2)
        self.assertFalse((self.root / "other").exists())

    def test_invalid_capture_frame(self):
        args = [*self.args, "--output", str(self.root / "bad")]
        idx = args.index("--capture-frame")
        args[idx + 1] = "20"
        with redirect_stdout(io.StringIO()):
            self.assertEqual(tourcapture.main(args), 2)


if __name__ == "__main__":
    unittest.main()
