# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""GPU-free contract tests for the public toursnapshot agent CLI."""
import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import toursnapshot as cli
import snapshot_sequence as codec


PIXEL = bytes((0x11, 0x22, 0x33, 0x44))


def chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def write_sequence(folder, count=2, interval=2, event_key=26, raw=False):
    folder.mkdir(parents=True, exist_ok=True)
    previous = None
    for index in range(1, count + 1):
        state = {"scope": "engine-scene-graph-v1", "nodes": {"0:0": {"x": index}},
                 "rng": {"seed": 7, "streams": {"gameplay": {"state": 9, "increment": 3}}}}
        key = index == 1 or (index - 1) % interval == 0
        record = {"format": "tourmaline.snapshot", "version": 1,
                  "sequence_id": "cli-unit", "frame": index,
                  "tick": index - 1, "dt_ns": 16666667,
                  "kind": "key" if key else "delta",
                  "previous_payload_fnv64": previous,
                  "state_fnv64": codec.fnv64(codec.canonical(state)),
                  "pixels_fnv64": codec.fnv64(PIXEL),
                  "events": [{"type": 3, "key": event_key}] if index == 1 else []}
        if raw:
            record["pixel_format"] = "rgba8"
        if key:
            record["state"] = state
            record["provenance"] = {"code_identity": "example-commit", "rng_seed": 7,
                                    "image_width": 1, "image_height": 1}
        else:
            record["previous_frame"] = index - 1
            record["patch"] = [{"op": "replace", "path": "/nodes/0:0/x", "value": index}]
        payload = codec.MARKER + codec.canonical(record)
        png = (codec.PNG_SIGNATURE +
               chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(b"\x00" + PIXEL)) +
               chunk(codec.CHUNK, payload) + chunk(b"IEND", b""))
        if raw:
            (folder / f"Frame_{index:04d}.rgba").write_bytes(PIXEL)
            (folder / f"Frame_{index:04d}.json").write_bytes(codec.canonical(record))
        else:
            (folder / f"Frame_{index:04d}.png").write_bytes(png)
        previous = codec.fnv64(payload)


def call(*args):
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        result = cli.main(list(args))
    return result, stdout.getvalue(), stderr.getvalue()


class SnapshotCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.frames = self.root / "frames"
        write_sequence(self.frames)
        self.app = self.root / "App"
        self.app.write_text("fake app")
        self.pak = self.root / "scene.pak"
        self.pak.write_text("fake pak")

    def test_help_and_usage_error(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), self.assertRaises(SystemExit) as e:
            cli.main(["--help"])
        self.assertEqual(e.exception.code, 0)
        self.assertIn("record", stdout.getvalue())
        rc, text, err = call("verify", "--json")
        self.assertEqual(rc, 64)
        self.assertEqual(json.loads(text)["status"], "error")
        self.assertEqual(err, "")

    def test_dry_run_is_json_and_has_no_side_effects(self):
        output = self.root / "planned"
        rc, text, err = call(
            "record", "--app", str(self.app), "--pak", str(self.pak),
            "--output", str(output), "--frames", "123", "--keyframe-interval", "60",
            "--seed", "0x2a", "--code-id", "game-abc", "--dry-run", "--json")
        self.assertEqual(rc, 0, err)
        report = json.loads(text)
        self.assertEqual(report["status"], "planned")
        self.assertEqual(report["seed"], "42")
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["operation"], "record")
        self.assertIn("--snapshot-count=123", report["argv"])
        self.assertIn("--snapshot-keyframe=60", report["argv"])
        self.assertIn("--snapshot-code-id=game-abc", report["argv"])
        self.assertIn("--snapshot-capture=scoped", report["argv"])
        self.assertNotIn("--disable_control", report["argv"])
        self.assertNotIn("--bench", report["argv"])
        self.assertFalse(output.exists())
        self.assertEqual(err, "")

    def test_raw_output_mode_planning(self):
        rc, output, err = call(
            "record", "--app", str(self.app), "--pak", str(self.pak),
            "--output", str(self.root / "planned_raw"),
            "--output-format", "raw", "--dry-run", "--json")
        self.assertEqual(rc, 0, err)
        self.assertIn("--snapshot-format=raw", json.loads(output)["argv"])

    def test_raw_sidecars_verify_and_detect_corruption(self):
        raw_folder = self.root / "raw"
        write_sequence(raw_folder, count=3, raw=True)
        rc, output, err = call("verify", str(raw_folder), "--json")
        self.assertEqual(rc, 0, err)
        self.assertEqual(json.loads(output)["frames"], 3)
        rc, output, err = call("verify", str(raw_folder), "--pixels", "--json")
        self.assertEqual(rc, 0, err)
        (raw_folder / "Frame_0002.rgba").write_bytes(b"bad!")
        rc, output, _ = call("verify", str(raw_folder), "--json")
        self.assertEqual(rc, 1)
        self.assertEqual(json.loads(output)["status"], "error")

    def test_raw_sidecar_missing_and_mixed_files_rejected(self):
        raw_folder = self.root / "raw_missing"
        write_sequence(raw_folder, raw=True)
        (raw_folder / "Frame_0002.json").unlink()
        rc, output, _ = call("verify", str(raw_folder), "--json")
        self.assertEqual(rc, 1)
        self.assertEqual(json.loads(output)["status"], "error")
        (raw_folder / "Frame_0001.png").write_bytes(b"bogus")
        rc, output, _ = call("verify", str(raw_folder), "--json")
        self.assertEqual(rc, 1)

    def test_legacy_capture_mode_is_explicit(self):
        rc, out, err = call(
            "record", "--app", str(self.app), "--pak", str(self.pak),
            "--output", str(self.root / "legacy"), "--capture-mode", "legacy",
            "--dry-run", "--json")
        self.assertEqual(rc, 0, err)
        self.assertIn("--snapshot-capture=legacy", json.loads(out)["argv"])

    def test_invalid_capture_mode(self):
        rc, out, _ = call(
            "record", "--app", str(self.app), "--pak", str(self.pak),
            "--output", str(self.root / "invalid"), "--capture-mode", "unknown",
            "--dry-run", "--json")
        self.assertNotEqual(rc, 0)

    def test_reproducible_scenario_and_override_pin(self):
        catalog = self.root / "scenarios.json"
        catalog.write_text(json.dumps({"schema_version": 1, "scenarios": [{
            "id": "demo", "source_args": ["--pak", "scene.pak"],
            "environment": {"TOURMALINE_RNG_SEED": "19", "TOURMALINE_AA": "off"},
            "app_args": ["--minimal"], "frames": 4}]}))
        rc, text, err = call(
            "record", "--app", str(self.app), "--scenario", "demo",
            "--catalog", str(catalog), "--output", str(self.root / "candidate"),
            "--seed", "7", "--dry-run", "--json")
        self.assertEqual(rc, 0, err)
        report = json.loads(text)
        self.assertEqual(report["frames"], 4)
        self.assertEqual(report["seed"], "7")
        self.assertEqual(report["environment_pins"]["TOURMALINE_AA"], "off")
        self.assertIn("--minimal", report["argv"])

    def test_invalid_options_and_forbidden_override(self):
        base = ["record", "--app", str(self.app), "--pak", str(self.pak),
                "--output", str(self.root / "out"), "--dry-run", "--json"]
        for extra in (["--frames", "0"], ["--seed", "-1"],
                      ["--app-arg=--snapshot-count=2"],
                      ["--app-arg=--bench"], ["--scene", "x", "--scene-index", "2"],
                      ["--env", "PATH=/tmp"], ["--code-id", "x" * 257]):
            rc, output, _ = call(*base, *extra)
            self.assertEqual(rc, 64, extra)
            self.assertEqual(json.loads(output)["status"], "error")

    def test_verify_inspect_and_export(self):
        rc, out, _ = call("inspect", str(self.frames), "--json")
        self.assertEqual(rc, 0)
        inspect = json.loads(out)
        self.assertEqual(inspect["keyframes"], [1])
        self.assertEqual(inspect["provenance"]["code_identity"], "example-commit")
        self.assertEqual(inspect["rng_seed"], 7)
        rc, out, _ = call("verify", str(self.frames), "--json")
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out)["frames"], 2)
        extracted = self.root / "state.json"
        rc, out, _ = call("extract", str(self.frames), "--frame", "2",
                          "--output", str(extracted), "--json")
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(extracted.read_text())["nodes"]["0:0"]["x"], 2)
        self.assertEqual(call("extract", str(self.frames), "--frame", "2",
                              "--output", str(extracted), "--json")[0], 1)
        tape = self.root / "out.tinp"
        rc, out, _ = call("input-tape", str(self.frames), "--output", str(tape), "--json")
        self.assertEqual(rc, 0)
        self.assertTrue(tape.read_bytes().startswith(b"TOURINP1"))

    def test_state_mismatch_exit_two(self):
        state = self.root / "candidate.json"
        state.write_bytes(codec.canonical(codec.reconstruct(self.frames)[1][1]))
        self.assertEqual(call("compare", str(self.frames), "--frame", "2",
                              "--candidate", str(state), "--json")[0], 0)
        state.write_text(json.dumps({"rng": {"seed": 99}}))
        rc, out, _ = call("compare", str(self.frames), "--frame", "2",
                          "--candidate", str(state), "--json")
        self.assertEqual(rc, 2)
        report = json.loads(out)
        self.assertEqual(report["status"], "different")
        self.assertEqual(report["first_difference"], "/nodes")

    def test_compare_sequence_checks_input_and_timestep(self):
        target = self.root / "other"
        write_sequence(target, event_key=27)
        rc, out, _ = call("compare-sequence", str(self.frames),
                          "--candidate", str(target), "--json")
        self.assertEqual(rc, 2)
        self.assertEqual(json.loads(out)["type"], "input")
        self.assertEqual(json.loads(out)["frame"], 1)
        write_sequence(target)
        self.assertEqual(call("compare-sequence", str(self.frames),
                              "--candidate", str(target), "--json")[0], 0)

    def test_corrupted_png_is_error_not_difference(self):
        path = self.frames / "Frame_0001.png"
        data = bytearray(path.read_bytes())
        data[data.find(b"tsNp") + 8] ^= 0x01
        path.write_bytes(data)
        rc, out, err = call("verify", str(self.frames), "--json")
        self.assertEqual(rc, 1)
        self.assertIn("CRC", json.loads(out)["message"])
        self.assertEqual(err, "")

    def test_record_app_process_and_postvalidation(self):
        output = self.root / "recorded"

        def fake_run(argv, *, cwd, env, stdout, stderr, timeout, check):
            self.assertEqual(env["TOURMALINE_RNG_SEED"], "5")
            self.assertEqual(stderr, cli.subprocess.STDOUT)
            self.assertFalse(check)
            self.assertEqual(timeout, 30)
            # Path-form tolerance: the CLI may resolve the temp root to
            # its canonical form (CI runners hand tempfile SHORT names
            # like RUNNER~1 while the CLI emits runneradmin) - compare
            # both sides through resolve().
            seq = next(a for a in argv
                if a.startswith("--snapshot-sequence="))
            self.assertEqual(
                Path(seq.split("=", 1)[1]).resolve(), output.resolve())
            stdout.write("fake render complete\n")
            write_sequence(output, count=2, interval=1)
            return SimpleNamespace(returncode=0)

        with mock.patch.object(cli.subprocess, "run", side_effect=fake_run):
            rc, out, err = call(
                "record", "--app", str(self.app), "--pak", str(self.pak),
                "--output", str(output), "--frames", "2",
                "--keyframe-interval", "1", "--seed", "5",
                "--timeout", "30", "--json")
        self.assertEqual(rc, 0, err)
        self.assertEqual(json.loads(out)["keyframes"], [1, 2])
        self.assertIn("fake render complete", (output / "app.log").read_text())

    def test_record_existing_frames_refused(self):
        rc, out, _ = call(
            "record", "--app", str(self.app), "--pak", str(self.pak),
            "--output", str(self.frames), "--json")
        self.assertEqual(rc, 1)
        self.assertIn("already contains", json.loads(out)["message"])


if __name__ == "__main__":
    unittest.main()
