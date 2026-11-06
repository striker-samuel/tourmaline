"""Contract checks for the separate CPU and rendered-scene runners."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from subprocess import CompletedProcess

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cpu_scenario_harness as cpu


class CpuScenarioTests(unittest.TestCase):
    def test_catalog(self):
        entries = cpu.scenarios(cpu.CATALOG)
        self.assertEqual(entries["draw-items-105k"]["instances"], 105000)

    def test_duplicate_and_invalid_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "scenarios.json"
            item = {"id": "example", "instances": 32, "frames": 5, "camera": "fixed"}
            path.write_text(json.dumps({"schema_version": 1, "scenarios": [item, item]}))
            with self.assertRaisesRegex(ValueError, "duplicate"):
                cpu.scenarios(path)
            item["instances"] = 0
            path.write_text(json.dumps({"schema_version": 1, "scenarios": [item]}))
            with self.assertRaisesRegex(ValueError, "invalid count"):
                cpu.scenarios(path)

    def test_repeated_synthetic_results(self):
        item = {"id": "example", "instances": 32, "frames": 5, "camera": "sweep"}
        output = {"mode": "cpu-only-synthetic", "frames": 5, "instances": 32,
                  **{key: 2.5 for key in cpu.METRICS}}
        with patch.object(cpu.subprocess, "run", return_value=CompletedProcess(
                [], 0, stdout=json.dumps(output), stderr="")) as mocked:
            result = cpu.run(Path("CpuBenchmark"), item, 2, 3, 5)
        self.assertEqual(mocked.call_count, 3)
        self.assertEqual(result["median_ms"]["frame_ms"], 2.5)
        output["instances"] = 99
        with patch.object(cpu.subprocess, "run", return_value=CompletedProcess(
                [], 0, stdout=json.dumps(output), stderr="")):
            with self.assertRaisesRegex(ValueError, "workload differs"):
                cpu.run(Path("CpuBenchmark"), item, 2, 1, 5)

    def test_scene_listing_uses_real_catalog(self):
        self.assertEqual(cpu.main(["scene-list"]), 0)


if __name__ == "__main__":
    unittest.main()
