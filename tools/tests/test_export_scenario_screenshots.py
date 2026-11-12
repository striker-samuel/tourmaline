# SPDX-License-Identifier: MIT
"""CPU-only batch snapshot export tests."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import export_scenario_screenshots as exporter


class ExportScenarioSnapshotsTests(unittest.TestCase):
    def scenario(self, sid="demo"):
        return {"id": sid, "source_args": ["--pak", "content/paks/demo.pak"],
                "environment": {"TOURMALINE_AA": "off"},
                "app_args": ["--with=ssr"], "frames": 120}

    def test_dry_run_plans_scoped_snapshot_and_state_dump_without_gpu(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            catalog = root / "catalog.json"
            catalog.write_text(json.dumps({"schema_version": 1,
                                            "scenarios": [self.scenario()]}), encoding="utf-8")
            output = root / "output"
            args = exporter.parser().parse_args(["--catalog", str(catalog),
                                                  "--app", str(root / "missing-app"),
                                                  "--output", str(output), "--dry-run"])
            report = exporter.run_scenario(args, self.scenario())
            self.assertEqual(report["status"], "planned")
            self.assertEqual(report["record"]["frames"], 120)
            self.assertIn("--snapshot-capture=scoped", report["record"]["argv"])
            self.assertIn("--with=ssr", report["record"]["argv"])
            self.assertEqual(report["record"]["environment_pins"]["TOURMALINE_AA"], "off")
            self.assertEqual(report["state_frame"], 120)
            self.assertFalse(output.exists())

    def test_app_is_the_only_executable_flag(self):
        self.assertEqual(exporter.parser().parse_args(["--app", "App"]).app, Path("App"))
        with self.assertRaises(SystemExit):
            exporter.parser().parse_args(["--exe", "App"])

    def test_invalid_id_rejected_before_io(self):
        with tempfile.TemporaryDirectory() as td:
            args = exporter.parser().parse_args(["--app", "App", "--output", td, "--dry-run"])
            with self.assertRaisesRegex(ValueError, "unsafe scenario"):
                exporter.run_scenario(args, self.scenario("../escape"))

    def test_invalid_state_frame_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            args = exporter.parser().parse_args(["--app", "App", "--output", td,
                                                   "--state-frame", "121", "--dry-run"])
            with self.assertRaisesRegex(ValueError, "state frame"):
                exporter.run_scenario(args, self.scenario())

    def test_existing_state_dump_not_overwritten(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            state = root / "demo/state.json"
            state.parent.mkdir()
            state.write_text("previous", encoding="utf-8")
            args = exporter.parser().parse_args(["--app", "App", "--output", str(root)])
            with self.assertRaises(FileExistsError):
                exporter.run_scenario(args, self.scenario())
            self.assertEqual(state.read_text(encoding="utf-8"), "previous")

    def test_existing_manifest_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            catalog = root / "catalog.json"
            catalog.write_text(json.dumps({"schema_version": 1,
                                           "scenarios": [self.scenario()]}), encoding="utf-8")
            output = root / "output"
            output.mkdir()
            manifest = output / "manifest.json"
            manifest.write_text("prior manifest", encoding="utf-8")
            with self.assertRaises(SystemExit):
                exporter.main(["--catalog", str(catalog), "--app", "App",
                               "--output", str(output)])
            self.assertEqual(manifest.read_text(encoding="utf-8"), "prior manifest")

    def test_state_write_failure_leaves_no_partial_state_or_temp(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            args = exporter.parser().parse_args(["--app", "App", "--output", str(root)])
            frames = [({"kind": "key"}, {"tick": 0})]
            with mock.patch.object(exporter.toursnapshot, "run_record",
                                   return_value={"status": "ok", "frames": 1}), \
                 mock.patch.object(exporter.toursnapshot.codec, "reconstruct",
                                   return_value=frames), \
                 mock.patch.object(exporter.os, "replace", side_effect=OSError("rename failed")):
                with self.assertRaisesRegex(OSError, "rename failed"):
                    exporter.run_scenario(args, {**self.scenario(), "frames": 1})
            self.assertFalse((root / "demo/state.json").exists())
            self.assertEqual(list((root / "demo").glob(".state-*.tmp")), [])

    def test_record_extracts_canonical_state(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            args = exporter.parser().parse_args(["--app", "App", "--output", str(root),
                                                  "--state-frame", "2"])
            frames = [({"kind": "key"}, {"tick": 0}),
                      ({"kind": "delta"}, {"tick": 1, "scope": "engine-scene-graph-v1"})]
            with mock.patch.object(exporter.toursnapshot, "run_record",
                                   return_value={"status": "ok", "frames": 120}), \
                 mock.patch.object(exporter.toursnapshot.codec, "reconstruct", return_value=frames):
                result = exporter.run_scenario(args, self.scenario())
            self.assertEqual(result["status"], "captured")
            self.assertEqual(json.loads((root / "demo/state.json").read_text()), frames[1][1])


if __name__ == "__main__":
    unittest.main()
