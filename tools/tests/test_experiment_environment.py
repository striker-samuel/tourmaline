"""CPU-only checks of reproducibility probing; no installed GPU/DXC required."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[1] / "experiment_environment.py"
spec = importlib.util.spec_from_file_location("experiment_environment_test", MODULE)
env = importlib.util.module_from_spec(spec)
spec.loader.exec_module(env)


class EnvironmentProbeTests(unittest.TestCase):
    def test_missing_tools_are_nullable(self):
        with patch.object(env.shutil, "which", return_value=None), \
             patch.dict(env.os.environ, {}, clear=True), \
             patch.object(env.sys, "platform", "linux"):
            data = env.collect_environment("nonexistent/Benchmark")
        self.assertEqual(data["schema_version"], 1)
        self.assertIsNone(data["dxc"]["version_text"])
        self.assertIsNone(data["gpu"]["nvidia_smi"])
        self.assertEqual(data["gpu"]["windows_adapters"], [])
        self.assertIsNone(data["os"]["windows"])

    def test_windows_gpu_driver_cim_and_nvidia(self):
        cim = ('[{"Name":"Example GPU","DriverVersion":"31.2.1",'
               '"PNPDeviceID":"PCI\\\\EXAMPLE","VideoProcessor":"example"}]')
        def probe(argv, timeout=5):
            if argv[0] == "powershell.exe":
                return cim
            return "Example GPU, 570.12, 98.12.34"
        with patch.object(env.sys, "platform", "win32"), \
             patch.object(env.shutil, "which", side_effect=lambda x: x if x in ("powershell.exe", "nvidia-smi") else None), \
             patch.object(env, "probe_command", side_effect=probe):
            gpu = env.gpu_info()
        self.assertEqual(gpu["windows_adapters"][0]["driver_version"], "31.2.1")
        self.assertEqual(gpu["nvidia_smi"][0]["driver_version"], "570.12")

    def test_dxc_version_and_build_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "TOURMALINE_SHADER_DEBUG:BOOL=OFF\n"
                "TOURMALINE_SHADER_OD_RETRY:BOOL=OFF\n", encoding="utf-8")
            with patch.object(env, "probe_command", return_value="OVERVIEW: dxc\nVersion: 1.9.0"):
                compiler = env.compiler_info(str(root / "dxc"))
            self.assertIn("1.9.0", compiler["version_text"])
            build = env.build_info(root / "Benchmark")
            self.assertEqual(build["build_type"], "Release")
            self.assertEqual(build["shader_debug"], "OFF")

    def test_probe_failures_do_not_crash(self):
        with patch.object(env.subprocess, "run", side_effect=OSError("not installed")):
            self.assertIsNone(env.probe_command(["not-installed"]))


if __name__ == "__main__":
    unittest.main()
