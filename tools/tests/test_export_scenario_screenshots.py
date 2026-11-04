# SPDX-License-Identifier: MIT
"""CPU-only tests for batch performance scenario screenshot export."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import export_scenario_screenshots as screenshots


class ExportScenarioScreenshotsTests(unittest.TestCase):
    def scenario(self):
        return {"id": "demo", "source_args": ["--pak", "content/paks/demo.pak"],
                "environment": {"TOURMALINE_AA": "off"}, "app_args": ["--with=ssr"],
                "frames": 120}

    def test_plan_replays_scenario_and_scrubs_inherited_pins(self):
        with tempfile.TemporaryDirectory() as root:
            workdir = Path(root)
            pak = workdir / "content/paks/demo.pak"
            pak.parent.mkdir(parents=True)
            pak.write_bytes(b"pak")
            with mock.patch.dict("os.environ", {"TOURMALINE_SSR": "1", "OTHER": "ok"}):
                item = screenshots.plan(self.scenario(), workdir / "App",
                                        workdir, workdir / "output", 48)
            self.assertEqual(item["command"][1:3], ["--pak", "content/paks/demo.pak"])
            self.assertIn("--with=ssr", item["command"])
            self.assertEqual(item["environment"]["TOURMALINE_CAPTURE"], "48")
            self.assertNotIn("TOURMALINE_SSR", item["_env"])
            self.assertEqual(item["_env"]["OTHER"], "ok")

    def test_plan_clamps_capture_frame(self):
        with tempfile.TemporaryDirectory() as root:
            workdir = Path(root)
            pak = workdir / "content/paks/demo.pak"
            pak.parent.mkdir(parents=True)
            pak.write_bytes(b"pak")
            scenario = self.scenario()
            scenario["frames"] = 12
            item = screenshots.plan(scenario, workdir / "App", workdir,
                                    workdir / "output", 48)
            self.assertEqual(item["capture_frame"], 11)

    def test_rejects_invalid_id(self):
        scenario = self.scenario()
        scenario["id"] = "../escape"
        with self.assertRaises(ValueError):
            screenshots.plan(scenario, Path("App"), Path("."), Path("out"), 48)

    def test_dry_run_does_not_require_app_or_assets(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            catalog = root / "scenarios.json"
            catalog.write_text(json.dumps({"schema_version": 1,
                                            "scenarios": [self.scenario()]}), encoding="utf-8")
            output = root / "screenshots"
            with mock.patch("sys.stdout") as stdout:
                result = screenshots.main(["--catalog", str(catalog), "--exe",
                                           str(root / "missing-App"), "--output",
                                           str(output), "--dry-run"])
            self.assertEqual(result, 0)
            self.assertFalse(output.exists())
            self.assertTrue(stdout.write.called)

    def test_missing_capture_fails_not_reuses_old_file(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            exports = root / "exports"
            exports.mkdir()
            (exports / "screenshot_old.png").write_bytes(b"stale")
            item = {"directory": str(root / "output/demo"), "cwd": str(root),
                    "command": ["App"], "_env": {}}
            with mock.patch.object(screenshots.subprocess, "run", return_value=mock.Mock(returncode=0)):
                with self.assertRaisesRegex(RuntimeError, "fresh screenshot"):
                    screenshots.export_one(item, timeout=1, overwrite=False)


if __name__ == "__main__":
    unittest.main()
