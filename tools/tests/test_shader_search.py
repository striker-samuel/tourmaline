"""CPU-only tests for shader optimization orchestration."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[1] / "shader_search.py"
spec = importlib.util.spec_from_file_location("shader_search_test_module", MODULE)
search = importlib.util.module_from_spec(spec)
spec.loader.exec_module(search)


class ShaderSearchTests(unittest.TestCase):
    def test_accepts_valid_plain_and_fenced_sources(self):
        self.assertEqual(search.normalize_source("float4 mainPS() { return 0; }"),
                         "float4 mainPS() { return 0; }\n")
        self.assertEqual(search.normalize_source("```hlsl\nfoo\n```"), "foo\n")
        with self.assertRaises(ValueError):
            search.normalize_source("```hlsl\nincomplete")
        with self.assertRaises(ValueError):
            search.normalize_source("   ")

    def test_per_scenario_regression_gate(self):
        baseline = {"a": 2.0, "b": 4.0}
        self.assertLess(search.score({"a": 1.8, "b": 3.7}, baseline, 3), 0)
        self.assertIsNone(search.score({"a": 1.0, "b": 4.3}, baseline, 3))
        self.assertEqual(search.score(baseline, baseline, 3), 0)

    def test_openai_compatible_and_native_ollama(self):
        class FakeResponse:
            def __init__(self, data):
                self.data = data

            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

            def read(self, *_):
                return self.data

        import json
        sent = []

        def fake_urlopen(req, timeout):
            sent.append((req.full_url, json.loads(req.data), req.headers, timeout))
            if req.full_url.endswith("/api/chat"):
                return FakeResponse(json.dumps({"message": {"content": "new shader"}}).encode())
            return FakeResponse(json.dumps({"choices": [{"message": {"content": "new shader"}}]}).encode())

        with patch.object(search.urllib.request, "urlopen", fake_urlopen):
            a = search.request_candidate("openai", "http://localhost:9999/v1", "any-model",
                                         "token", "old", "none", 30)
            b = search.request_candidate("ollama", "http://localhost:11434", "local-model",
                                         "", "old", "none", 30)
        self.assertEqual(a, "new shader\n")
        self.assertEqual(b, "new shader\n")
        self.assertEqual(sent[0][0], "http://localhost:9999/v1/chat/completions")
        self.assertEqual(sent[1][0], "http://localhost:11434/api/chat")
        self.assertEqual(sent[0][1]["model"], "any-model")
        self.assertEqual(sent[1][1]["stream"], False)
        self.assertIn("Authorization", sent[0][2])
        self.assertNotIn("Authorization", sent[1][2])

    def test_benchmark_requires_positive_measurement(self):
        import json
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = root / "runs"
            out.mkdir()
            (out / "result.json").write_text(json.dumps({
                "mode": "bench", "scenario": "abc", "summary": {"median_ms": 1.25}
            }), encoding="utf-8")
            with patch.object(search.subprocess, "run", return_value=SimpleNamespace(returncode=0)):
                self.assertEqual(search.scenario_benchmark(root, root / "benchmark",
                                 root / "catalog", "abc", 2, out, 10), 1.25)

    def test_run_command_never_uses_shell(self):
        with patch.object(search.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "ok"
            run.return_value.stderr = ""
            ok, _ = search.run_command('echo "hello world"', Path("."), {}, 10)
            self.assertTrue(ok)
            self.assertEqual(run.call_args.args[0], ["echo", "hello world"])
            self.assertNotIn("shell", run.call_args.kwargs)


if __name__ == "__main__":
    unittest.main()
