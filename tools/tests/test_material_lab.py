# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""CPU-only contract tests for environment-aware material regression suites."""
import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import material_lab
import material_lab_fixtures


class MaterialLabTests(unittest.TestCase):
    def setUp(self):
        self.suite = material_lab.load_suite(material_lab.DEFAULT_SUITE)

    def test_selects_bundled_environments_by_default(self):
        cases = material_lab.plans(self.suite, material="metal_smooth", view="lit,health")
        self.assertEqual([c["name"] for c in cases], [
            "studio/metal_smooth/lit", "studio/metal_smooth/health",
            "cornell/metal_smooth/lit", "cornell/metal_smooth/health",
            "outdoor/metal_smooth/lit", "outdoor/metal_smooth/health"])

    def test_explicit_ibl_requires_provided_cooked_probe(self):
        case = material_lab.plans(self.suite, environment="ibl_overcast",
                                  material="copper_brushed", view="lit")[0]
        self.assertTrue(case["external"])
        self.assertEqual(case["pins"]["TOURMALINE_CUBEMAP"], "1")
        command = material_lab.capture_command(case, "App", "out/case",
                                               gpu_tag="GPU_driver")
        self.assertIn("content/paks/material_lab_ibl_overcast.pak", command)
        self.assertIn("--scene", command)
        self.assertIn("GPU_driver", command)

    def test_debug_view_pins_and_signatures(self):
        a, b = material_lab.plans(self.suite, environment="studio",
                                  material="metal_smooth", view="lit,roughness")
        self.assertNotIn("TOURMALINE_DEBUGVIEW", a["pins"])
        self.assertEqual(b["pins"]["TOURMALINE_DEBUGVIEW"], "pbrrough")
        self.assertNotEqual(a["signature"], b["signature"])
        self.assertIn("TOURMALINE_DEBUGVIEW=pbrrough",
                      material_lab.capture_command(b, "App", "out"))

    def test_invalid_sources_and_pins_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = copy.deepcopy(self.suite)
            data["environments"]["studio"]["source"]["pak"] = "../../other.pak"
            file = Path(tmp) / "bad.json"
            file.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(material_lab.SuiteError):
                material_lab.load_suite(file)
            data = copy.deepcopy(self.suite)
            data["environments"]["studio"]["pins"]["TOURMALINE_CAPTURE"] = "3"
            file.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(material_lab.SuiteError):
                material_lab.load_suite(file)

    def test_baseline_identity_drift_rejected_before_launch(self):
        case = material_lab.plans(self.suite, environment="studio",
                                  material="metal_smooth", view="lit")[0]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / "baseline" / case["name"]
            folder.mkdir(parents=True)
            (folder / "case.json").write_text(json.dumps({"signature": "bad"}),
                                              encoding="utf-8")
            args = SimpleNamespace(output=root / "new",
                baseline_dir=root / "baseline", gpu_tag="GPU_driver",
                dry_run=False, exe=Path("App"), overwrite=False, fail_fast=False)
            with patch.object(material_lab.subprocess, "run") as process:
                with self.assertRaises(material_lab.SuiteError):
                    material_lab.run(args, self.suite, [case])
                process.assert_not_called()

    def test_dry_run_never_launches(self):
        case = material_lab.plans(self.suite, environment="studio",
                                  material="metal_smooth", view="lit")[0]
        with tempfile.TemporaryDirectory() as tmp:
            args = SimpleNamespace(output=Path(tmp) / "out", baseline_dir=None,
                gpu_tag=None, dry_run=True, exe=Path("App"), overwrite=False,
                fail_fast=False)
            with patch.object(material_lab.subprocess, "run") as process:
                result = material_lab.run(args, self.suite, [case])
                process.assert_not_called()
            self.assertEqual(result["status"], "planned")
            self.assertFalse(args.output.exists())

    def test_fixture_generator_outputs_valid_gltf_accessors(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = material_lab_fixtures.generate(Path(tmp))
            for env in ("studio", "cornell", "outdoor"):
                path = project / "content/models" / env / f"{env}.gltf"
                gl = json.loads(path.read_text(encoding="utf-8"))
                blob = path.with_suffix(".bin").read_bytes()
                self.assertEqual(len(blob), gl["buffers"][0]["byteLength"])
                self.assertEqual(len(gl["materials"]), 14 if env == "cornell" else 10)
                for accessor in gl["accessors"]:
                    view = gl["bufferViews"][accessor["bufferView"]]
                    size = {5123: 2, 5126: 4}[accessor["componentType"]]
                    count = {"SCALAR": 1, "VEC3": 3}[accessor["type"]]
                    self.assertLessEqual(view["byteOffset"] +
                        accessor["count"] * count * size, len(blob))
                tour = (project / "content/scenes" / f"{env}.tour").read_text()
                self.assertIn("env.default.sunIntensity", tour)
                self.assertIn(f"content/models/{env}/{env}.geometry", tour)
                ref = json.loads(path.with_suffix(".refscene.json").read_text())
                self.assertEqual(ref["schema_version"], 1)
                self.assertEqual(len(ref["positions"]), len(ref["normals"]))
                self.assertEqual(len(ref["indices"]) % 3, 0)
                self.assertEqual(len(ref["indices"]) // 3,
                                 len(ref["triangle_materials"]))
                self.assertEqual(len(ref["materials"]), len(gl["materials"]))
                self.assertLess(max(ref["indices"]), len(ref["positions"]))
                self.assertLess(max(ref["triangle_materials"]), len(ref["materials"]))
                if env == "cornell":
                    self.assertEqual(len(ref["environment"]["spots"]), 1)

    def test_cpu_reference_command_uses_same_fixture_and_camera(self):
        case = material_lab.plans(self.suite, "studio", "metal_smooth", "lit")[0]
        cmd = material_lab.reference_command(
            case, Path("ReferenceScene"), Path("out/material-lab/source"),
            Path("out/reference/studio/metal_smooth/lit"), 128, 16, 2)
        self.assertIn("studio.refscene.json", " ".join(cmd))
        self.assertIn("--camera-pos", cmd)
        self.assertEqual(cmd[cmd.index("--spp") + 1], "16")
        self.assertEqual(cmd[cmd.index("--bounces") + 1], "2")
        with self.assertRaises(material_lab.SuiteError):
            material_lab.reference_command(
                material_lab.plans(self.suite, "ibl_softbox",
                                   "metal_smooth", "lit")[0],
                "ReferenceScene", "src", Path("out"))

    def test_reference_dry_run_does_not_launch_or_write(self):
        case = material_lab.plans(self.suite, "studio", "metal_smooth", "lit")[0]
        with tempfile.TemporaryDirectory() as tmp:
            args = SimpleNamespace(exe=Path("ReferenceScene"), project=Path(tmp),
                output=Path(tmp) / "out", resolution=128, spp=4, bounces=2,
                exposure=0.001, fail_fast=True, dry_run=True)
            with patch.object(material_lab.subprocess, "run") as process:
                report = material_lab.run_reference(args, [case])
                process.assert_not_called()
            self.assertEqual(report["status"], "planned")
            self.assertFalse(args.output.exists())

    def test_fixture_project_disables_the_cubemap_capture_bake(self):
        # The legacy single-scene build lane would otherwise run Cook's
        # --cubemap stage: the auto probe lands inside the centre sphere,
        # the capture bakes black and the pak gate refuses the empty
        # manifest. These are analytic-sky environments by design.
        with tempfile.TemporaryDirectory() as tmp:
            project = material_lab_fixtures.generate(Path(tmp))
            text = (project / ".tourmaline/build-settings.txt").read_text()
            self.assertIn("cubemap=off", text)


if __name__ == "__main__":
    unittest.main()
