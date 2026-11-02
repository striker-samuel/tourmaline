"""CPU-only checks for tracked benchmark locations and agent replay."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "perf_harness.py"
spec = importlib.util.spec_from_file_location("perf_harness_scenarios", MODULE)
perf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(perf)


def entry(label="Lighting corner"):
    return {
        "id": "lighting-corner",
        "label": label,
        "subsystem": "shadows",
        "notes": "Test shadow atlas work",
        "source_args": ["--pak", "content/paks/bistro.pak"],
        "app_args": ["--without=bloom"],
        "environment": {
            "TOURMALINE_CAM_POS": "1,2,3",
            "TOURMALINE_CAM_FWD": "0,0,-1",
            "TOURMALINE_SSR": "1"
        },
        "camera": {"position": [1, 2, 3], "forward": [0, 0, -1]},
        "frames": 90
    }


class CatalogTests(unittest.TestCase):
    def write_catalog(self, directory, scenarios):
        path = Path(directory) / "scenarios.json"
        path.write_text(json.dumps({"schema_version": 1, "scenarios": scenarios}), encoding="utf-8")
        return path

    def test_lookup_and_cli_listing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write_catalog(tmp, [entry()])
            self.assertEqual(perf.select_scenario(path, "lighting-corner")["frames"], 90)
            self.assertEqual(perf.main(["list", "--catalog", str(path)]), 0)
            with self.assertRaises(ValueError):
                perf.select_scenario(path, "not-in-catalog")

    def test_duplicate_ids_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write_catalog(tmp, [entry(), entry()])
            with self.assertRaisesRegex(ValueError, "duplicate"):
                perf.read_catalog(path)

    def test_invalid_launch_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = entry()
            bad["source_args"] = ["--bench", "0"]
            path = self.write_catalog(tmp, [bad])
            with self.assertRaisesRegex(ValueError, "source_args"):
                perf.select_scenario(path, bad["id"])

    def test_invalid_environment_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = entry()
            bad["environment"]["LD_PRELOAD"] = "tamper"
            path = self.write_catalog(tmp, [bad])
            with self.assertRaisesRegex(ValueError, "environment"):
                perf.select_scenario(path, bad["id"])

    def test_scenario_command_preserves_cooked_scene(self):
        e = entry()
        args = SimpleNamespace(exe=Path("/tmp/Benchmark"), source_args=e["source_args"],
                               project=None, pak=None, frames=e["frames"], app_arg=e["app_args"])
        command = perf.command(args)
        self.assertEqual(command[1:3], e["source_args"])
        self.assertIn("--bench", command)
        self.assertEqual(command[-1], "--without=bloom")

    def test_repo_catalog_is_valid(self):
        root = Path(__file__).resolve().parents[2] / "benchmarks/scenarios.json"
        items = perf.read_catalog(root)
        self.assertGreaterEqual(len(items), 1)
        for scenario in items:
            self.assertEqual(perf.select_scenario(root, scenario["id"]), scenario)


if __name__ == "__main__":
    unittest.main()
