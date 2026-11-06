"""Optional analytics tests: stdlib parser always works; DuckDB tests skip if absent."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "export_experiments.py"
spec = importlib.util.spec_from_file_location("experiment_export_test", MODULE)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class ExportTests(unittest.TestCase):
    def test_normalize_matrix_record(self):
        record = {
            "record_id": "one", "machine": "gpu1", "setup": "release",
            "scenario": "sponza-atrium", "mode": "bench", "status": "pass",
            "result": {"summary": {"median_ms": 2.5}},
            "environment": {
                "os": {"system": "Windows", "version": "10.0", "windows": {"build": 26100}},
                "gpu": {"windows_adapters": [{"name": "GPU", "driver_version": "42"}]},
                "dxc": {"version_text": "DXC 1.9", "sha256": "hash"},
            },
        }
        row = dict(zip(mod.COLUMNS, mod.row_from_record(record, Path("record.json"))))
        self.assertEqual(row["frame_ms"], 2.5)
        self.assertEqual(row["windows_build"], 26100)
        self.assertEqual(row["gpu_driver"], "42")
        self.assertEqual(row["dxc_sha256"], "hash")
        self.assertEqual(row["experiment_kind"], "scenario_matrix")

    def test_collect_shader_search_candidates(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "environment.json").write_text(json.dumps({"os": {"system": "Linux"}}))
            candidate = root / "candidate-001"
            candidate.mkdir()
            (candidate / "result.json").write_text(
                json.dumps({"iteration": 1, "accepted": False, "gain_percent": -0.5}))
            rows = mod.collect_rows(root)
            self.assertEqual(len(rows), 1)
            row = dict(zip(mod.COLUMNS, rows[0]))
            self.assertEqual(row["candidate_id"], "candidate-001")
            self.assertEqual(row["os_system"], "Linux")
            self.assertEqual(row["gain_percent"], -0.5)

    def test_candidate_metrics_export_one_row_per_scenario(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "environment.json").write_text(json.dumps({"os": {"system": "Windows"}}))
            item = root / "candidate-001"
            item.mkdir()
            (item / "result.json").write_text(json.dumps({
                "iteration": 1, "accepted": True,
                "times_ms": {"bistro": 4.3, "sponza": 6.1},
                "gain_percent": 2.0, "target_resolution": "1080p",
                "candidate_sha256": "abcdef",
                "visual_error": {"difference_fraction": 0.002,
                                 "average_channel_delta": 0.15},
                "register_metrics": {"registers_per_thread": 32, "spill_bytes": 0},
            }))
            rows = [dict(zip(mod.COLUMNS, r)) for r in mod.collect_rows(root)]
            self.assertEqual(len(rows), 2)
            self.assertEqual({r["scenario"] for r in rows}, {"bistro", "sponza"})
            self.assertEqual({r["frame_ms"] for r in rows}, {4.3, 6.1})
            self.assertEqual(rows[0]["visual_difference_fraction"], 0.002)
            self.assertEqual(rows[0]["registers_per_thread"], 32)
            self.assertEqual(rows[0]["target_resolution"], "1080p")
            self.assertEqual(rows[0]["shader_sha256"], "abcdef")

    def test_export_parquet_and_duckdb_when_installed(self):
        try:
            import duckdb
        except ImportError:
            self.skipTest("extended analytics not installed")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "records"
            root.mkdir()
            (root / "record.json").write_text(json.dumps({
                "schema_version": 1, "record_id": "one", "mode": "bench",
                "status": "pass", "result": {"summary": {"median_ms": 1.25}}
            }))
            parquet = Path(temp) / "data.parquet"
            database = Path(temp) / "data.duckdb"
            self.assertEqual(mod.export_records(root, parquet), 1)
            self.assertEqual(mod.export_records(root, database, "duckdb"), 1)
            self.assertEqual(duckdb.sql("SELECT count(*) FROM read_parquet(?)",
                                        params=[str(parquet)]).fetchone()[0], 1)
            with duckdb.connect(str(database)) as db:
                self.assertEqual(db.sql("SELECT count(*) FROM experiments").fetchone()[0], 1)


if __name__ == "__main__":
    unittest.main()
