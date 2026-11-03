"""DCC runner contract tests, using subprocesses but no Maya/Blender install."""
from __future__ import annotations

from pathlib import Path
import os
import sys
import tempfile
import unittest

from dcc_plugins import available_plugins, load_plugin
from dcc_runner import ProcessJob, run_local, validate_environment
from publish_gltf import publish


class TestDccRunner(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tourmaline dcc test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tools = Path(__file__).resolve().parent

    def test_plugin_discovery_and_rejection(self):
        self.assertIn("maya", available_plugins())
        self.assertIn("blender", available_plugins())
        self.assertEqual(load_plugin("maya").software, "maya")
        with self.assertRaises(ValueError):
            load_plugin("../../untrusted")
        with self.assertRaises(ValueError):
            load_plugin("not_installed")

    def test_maya_job_specification(self):
        source = self.root / "model with spaces.ma"
        staged = self.root / "export file.gltf"
        job = load_plugin("maya").build_job(
            source, staged, self.tools,
            executable="/opt/maya bin/mayapy",
            environment={"CUSTOM_PLUGIN_PATH": "/studio/maya plugins"})
        self.assertEqual(job.command(), [
            "/opt/maya bin/mayapy",
            str(self.tools / "maya_export_gltf.py"),
            str(source), str(staged)])
        self.assertEqual(job.environment["CUSTOM_PLUGIN_PATH"],
                         "/studio/maya plugins")
        self.assertEqual(job.environment["MAYA_DISABLE_CIP"], "1")
        self.assertEqual(job.software, "maya")
        self.assertGreaterEqual(job.priority, 1)

    def test_blender_job_specification(self):
        source = self.root / "hero.blend"
        staged = self.root / "hero.gltf"
        job = load_plugin("blender").build_job(
            source, staged, self.tools, executable="blender-bin")
        self.assertEqual(job.command(), [
            "blender-bin", "--background", str(source),
            "--python", str(self.tools / "blender_export_gltf.py"),
            "--", str(staged)])

    def test_local_runner_env_arguments_and_working_dir(self):
        source = self.root / "fake exporter.py"
        output = self.root / "output with spaces.txt"
        source.write_text(
            "from pathlib import Path\n"
            "import os, sys\n"
            "Path(sys.argv[1]).write_text("
            "os.environ['TOURMALINE_TEST_RUNNER'] + '|' + os.getcwd())\n",
            encoding="utf-8")
        job = ProcessJob(
            name="fake-export", software="python",
            executable=sys.executable, script=source,
            script_args=(str(output),),
            environment={"TOURMALINE_TEST_RUNNER": "value with spaces"},
            working_dir=self.root, timeout_seconds=10)
        run_local(job)
        self.assertEqual(output.read_text(),
                         "value with spaces|" + str(self.root))

    def test_nonzero_job_preserves_failure_details(self):
        source = self.root / "failure.py"
        source.write_text(
            "import sys\nprint('export failed in plugin')\nsys.exit(7)\n",
            encoding="utf-8")
        job = ProcessJob(name="bad-export", software="python",
                         executable=sys.executable, script=source)
        with self.assertRaisesRegex(RuntimeError,
                                    r"exited 7.*export failed in plugin"):
            run_local(job)

    def test_timeout(self):
        source = self.root / "sleep.py"
        source.write_text("import time\ntime.sleep(2)\n", encoding="utf-8")
        job = ProcessJob(name="slow-export", software="python",
                         executable=sys.executable, script=source,
                         timeout_seconds=0.02)
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            run_local(job)

    def test_nonfinite_and_nonpositive_timeouts_are_rejected(self):
        source = self.root / "noop.py"
        source.write_text("pass\n", encoding="utf-8")
        for timeout in (float("nan"), float("inf"), float("-inf"), 0, -1):
            with self.subTest(timeout=timeout):
                job = ProcessJob(name="invalid-timeout", software="python",
                                 executable=sys.executable, script=source,
                                 timeout_seconds=timeout)
                with self.assertRaisesRegex(ValueError, "positive and finite"):
                    run_local(job)

    def test_environment_validation(self):
        self.assertEqual(validate_environment({"PATH": "some path"}),
                         {"PATH": "some path"})
        for invalid in ["INVALID-NAME", "A=B", "", "A B"]:
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    validate_environment({invalid: "x"})
        with self.assertRaises(ValueError):
            validate_environment({"GOOD": "nul\x00bad"})

    def test_custom_plugin_file_needs_no_core_dispatch(self):
        plugins = self.root / "plugins"
        plugins.mkdir()
        (plugins / "otherdcc.py").write_text(
            "from dcc_runner import ExportPlugin, ProcessJob\n"
            "class Plugin(ExportPlugin):\n"
            "    name = 'otherdcc'\n"
            "    software = 'otherdcc'\n"
            "    extensions = ('.custom',)\n"
            "    def build_job(self, source, staged, tools_dir, *, "
            "executable=None, environment=None):\n"
            "        return ProcessJob(name='other', software='other', "
            "executable=executable or 'python', script=tools_dir / 'noop.py')\n",
            encoding="utf-8")
        self.assertIn("otherdcc", available_plugins(plugins))
        self.assertTrue(load_plugin("otherdcc", plugins)
                        .accepts(self.root / "asset.custom"))

    def test_wrong_dcc_does_not_replace_published_output(self):
        source = self.root / "model.ma"
        source.write_text("maya scene")
        published = self.root / "model.gltf"
        published.write_text("last-good")
        with self.assertRaises(ValueError):
            publish(source, published, "blender")
        self.assertEqual(published.read_text(), "last-good")


if __name__ == "__main__":
    unittest.main()
