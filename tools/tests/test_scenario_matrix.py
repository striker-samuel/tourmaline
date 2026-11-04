"""Hardware-free tests for multi-setup scenario tooling."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import uuid


def load(name):
    path = Path(__file__).resolve().parents[1] / (name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


matrix = load("scenario_matrix")
collector = load("scenario_collector")


class ScenarioMatrixTests(unittest.TestCase):
    def test_matrix_validation(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "matrix.json"
            good = {"schema_version": 1, "scenarios": ["sponza-atrium"],
                    "setups": [{"name": "gpu", "exe": "Benchmark", "env": {"TOURMALINE_AA": "off"}}]}
            path.write_text(json.dumps(good))
            self.assertEqual(matrix.load_matrix(path), good)
            for bad in (dict(good, scenarios=["../bad"]),
                        dict(good, setups=good["setups"] * 2),
                        dict(good, setups=[dict(good["setups"][0], env={"LD_PRELOAD": "evil"})])):
                path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):
                    matrix.load_matrix(path)

    def test_verdicts(self):
        rid = str(uuid.uuid4())
        def verdict(mode, rc, result):
            return matrix.make_record("gpu", "setup", "scene", mode, rid, "commit", result, rc)["status"]
        self.assertEqual(verdict("cull", 0, {"ok": True, "cull_status": "match"}), "pass")
        self.assertEqual(verdict("cull", 2, {"ok": True}), "mismatch")
        self.assertEqual(verdict("cull", 0, {"ok": True}), "error")
        self.assertEqual(verdict("snapshot", 0, {"ok": True}), "pass")
        self.assertEqual(verdict("bench", 1, {"ok": False}), "error")

    def test_record_preserves_reproducibility_metadata(self):
        metadata = {
            "os": {"windows": {"build": 26100}},
            "gpu": {"windows_adapters": [{"name": "GPU", "driver_version": "123"}]},
            "dxc": {"version_text": "DXC 1.9", "sha256": "abc"},
        }
        record = matrix.make_record("gpu", "setup", "scene", "bench",
                                    str(uuid.uuid4()), "commit", {"ok": True}, 0,
                                    metadata)
        self.assertEqual(record["environment"], metadata)
        self.assertEqual(collector.validate_record(record), record)

    def test_collector_rejects_traversal_and_bad_ids(self):
        record = matrix.make_record("gpu", "setup", "scene", "cull", str(uuid.uuid4()),
                                    "commit", {"ok": True}, 0)
        self.assertEqual(collector.validate_record(record), record)
        for patch in ({"machine": "../bad"}, {"record_id": "../../bad"},
                      {"status": "fake"}, {"result": []}):
            with self.assertRaises(ValueError):
                collector.validate_record(dict(record, **patch))

    def test_http_collection_auth_and_duplicates(self):
        from http.server import ThreadingHTTPServer
        import threading
        import urllib.error
        import urllib.request
        with tempfile.TemporaryDirectory() as d:
            server = ThreadingHTTPServer(("127.0.0.1", 0), collector.handler_for(d, "secret"))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                record = matrix.make_record("gpu", "setup", "scene", "cull",
                                            str(uuid.uuid4()), "rev", {"ok": True}, 0)
                url = "http://127.0.0.1:%d/records" % server.server_port
                def send(auth):
                    headers = {"Content-Type": "application/json"}
                    if auth:
                        headers["Authorization"] = "Bearer secret"
                    return urllib.request.urlopen(urllib.request.Request(
                        url, data=json.dumps(record).encode(), headers=headers))
                with self.assertRaises(urllib.error.HTTPError) as e:
                    send(False)
                self.assertEqual(e.exception.code, 401)
                with send(True) as response:
                    self.assertEqual(response.status, 201)
                self.assertTrue((Path(d) / "gpu/setup/scene" / (record["record_id"] + ".json")).is_file())
                with self.assertRaises(urllib.error.HTTPError) as e:
                    send(True)
                self.assertEqual(e.exception.code, 409)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()


if __name__ == "__main__":
    unittest.main()
