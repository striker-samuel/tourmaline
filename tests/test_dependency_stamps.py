#!/usr/bin/env python3
"""Regression suite for cmake/DependencyStamps.cmake (the configure-time
vendored-dependency staleness gate).

The gate compares every <tree>/.depstamp-<name> record against the pin the
platform's install_dependencies script declares TODAY. These tests drive the
module in cmake -P script mode over throwaway repos so the parsing (two
different pin-table formats), the stamp comparison and the ImGui docking probe
are pinned without touching the real checkout.
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MODULE = REPO_ROOT / "cmake" / "DependencyStamps.cmake"


def find_cmake():
    """cmake for the -P driver: ctest hands us its own interpreter first (the
    Windows build machines keep CMake inside Visual Studio, off PATH)."""
    candidates = [os.environ.get("TOURMALINE_CMAKE"), shutil.which("cmake")]
    for pattern in (
        "C:/Program Files/Microsoft Visual Studio/*/*/Common7/IDE/"
        "CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe",
        "C:/Program Files/CMake/bin/cmake.exe",
    ):
        candidates.extend(str(p) for p in sorted(glob.glob(pattern), reverse=True))
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


CMAKE = find_cmake()

PS1_PINS = """\
$Pins = @(
    @{ name='imgui';      type='tar';  target='imgui';
       url='https://codeload.github.com/ocornut/imgui/tar.gz/62275e877a691bd2e51d1ca86bdfed49fa5cb297';
       sha256='1ffd96bd103ce35a9cb0119c099eb63d07645276fb9aa1cd851c2e6ad8bbbbbd' }
    @{ name='lz4';        type='raw';  target='lz4'; multi=$true;
       urls=@('https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/lz4.h',
              'https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/lz4.c');
       files=@('lz4.h','lz4.c');
       sha256=@('26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f',
                 '9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539') }
)
"""

SH_PINS = """\
install_raw stb-image   stb stb_image.h \\
    'https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h' \\
    594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3 || fail stb-image
install_archive imgui imgui tar \\
    'https://codeload.github.com/ocornut/imgui/tar.gz/62275e877a691bd2e51d1ca86bdfed49fa5cb297' \\
    1ffd96bd103ce35a9cb0119c099eb63d07645276fb9aa1cd851c2e6ad8bbbbbd \\
    '*' '' '' '' '' || fail imgui
"""

IMGUI_PIN = ("https://codeload.github.com/ocornut/imgui/tar.gz/"
             "62275e877a691bd2e51d1ca86bdfed49fa5cb297")
IMGUI_SHA = "1ffd96bd103ce35a9cb0119c099eb63d07645276fb9aa1cd851c2e6ad8bbbbbd"
OLD_IMGUI_PIN = ("https://codeload.github.com/ocornut/imgui/tar.gz/"
                 "1c544ee9414f9b7f4dbabf2c2ff5cbe0e1f3b8b2")

DOCKING_HEADER = '#define IMGUI_VERSION       "1.92.3"\n#define IMGUI_HAS_DOCK\n'
MAINLINE_HEADER = '#define IMGUI_VERSION       "1.92.3"\n'


@unittest.skipUnless(CMAKE, "cmake is not on PATH")
class DependencyStampTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "external").mkdir()
        self.ps1 = self.root / "install_dependencies.ps1"
        self.sh = self.root / "install_dependencies.sh"
        self.ps1.write_text(PS1_PINS, encoding="utf-8")
        self.sh.write_text(SH_PINS, encoding="utf-8")

    def tearDown(self):
        self._tmp.cleanup()

    # -- fixtures -----------------------------------------------------
    def tree(self, target, name, url, sha, files=None):
        d = self.root / "external" / target
        d.mkdir(parents=True, exist_ok=True)
        (d / f".depstamp-{name}").write_text(f"{url}|{sha}\n", encoding="utf-8")
        for fname, text in (files or {}).items():
            (d / fname).write_text(text, encoding="utf-8")
        return d

    def check(self, pins=None, extra_args=()):
        cmd = [CMAKE, f"-DTOURMALINE_DEPS_ROOT={self.root}"]
        if pins is not None:
            cmd.append(f"-DTOURMALINE_DEPS_PINS={pins}")
        cmd += list(extra_args)
        cmd += ["-P", str(MODULE)]
        return subprocess.run(cmd, capture_output=True, text=True)

    def assert_fails(self, result, *needles):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        output = result.stdout + result.stderr
        for needle in needles:
            self.assertIn(needle, output)
        return output

    # -- stamp comparison ---------------------------------------------
    def test_matching_pin_passes(self):
        self.tree("imgui", "imgui", IMGUI_PIN, IMGUI_SHA,
                  {"imgui.h": DOCKING_HEADER})
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("match", r.stdout)

    def test_stale_pin_fails_with_the_refresh_command(self):
        self.tree("imgui", "imgui", OLD_IMGUI_PIN, "deadbeef",
                  {"imgui.h": DOCKING_HEADER})
        # The refresh command is the PLATFORM's installer (install_dependencies
        # .ps1 on Windows, .sh elsewhere), so match the shared stem, not the
        # extension.
        out = self.assert_fails(self.check(),
                                "Stale vendored dependencies",
                                "external/imgui",
                                OLD_IMGUI_PIN,
                                IMGUI_PIN,
                                "install_dependencies")
        self.assertIn("TOURMALINE_SKIP_DEP_STAMP_CHECK", out)

    def test_mainline_imgui_is_named_even_when_the_stamp_matches(self):
        # The recorded failure mode: a hand-managed mainline tree whose stamp
        # was rewritten to the current pin. The feature probe still catches it.
        self.tree("imgui", "imgui", IMGUI_PIN, IMGUI_SHA,
                  {"imgui.h": MAINLINE_HEADER})
        out = self.assert_fails(self.check(),
                                "MAINLINE ImGui",
                                "IMGUI_HAS_DOCK",
                                "imgui_overlay.cpp")
        self.assertIn("install_dependencies.ps1", out)

    def test_unpinned_tree_is_ignored(self):
        self.tree("imgui", "imgui", IMGUI_PIN, IMGUI_SHA,
                  {"imgui.h": DOCKING_HEADER})
        self.tree("host-extra", "host-extra", "https://example.invalid/old",
                  "beef")
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_multi_url_pin_has_no_single_url_stamp_to_compare(self):
        # lz4's raw pin carries two URLs; its stamp starts with an empty URL.
        # The gate must not read that as a mismatch.
        self.tree("lz4", "lz4",
                  "", "26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f "
                  "9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539")
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    # -- the POSIX pin-table format -----------------------------------
    def test_posix_pin_table_is_parsed(self):
        self.tree("imgui", "imgui", OLD_IMGUI_PIN, "deadbeef",
                  {"imgui.h": DOCKING_HEADER})
        self.assert_fails(self.check(pins=self.sh),
                          "Stale vendored dependencies", "external/imgui")

    def test_posix_raw_pin_table_is_parsed(self):
        self.tree("stb", "stb-image",
                  "https://raw.githubusercontent.com/nothings/stb/OLD/stb_image.h",
                  "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3")
        self.assert_fails(self.check(pins=self.sh),
                          "Stale vendored dependencies", "external/stb")

    def test_posix_matching_pin_passes(self):
        self.tree("imgui", "imgui", IMGUI_PIN, IMGUI_SHA,
                  {"imgui.h": DOCKING_HEADER})
        r = self.check(pins=self.sh)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    # -- escape hatches ------------------------------------------------
    def test_skip_option_bypasses_the_gate(self):
        self.tree("imgui", "imgui", OLD_IMGUI_PIN, "deadbeef",
                  {"imgui.h": DOCKING_HEADER})
        r = self.check(extra_args=["-DTOURMALINE_SKIP_DEP_STAMP_CHECK=ON"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("skipped", r.stdout)

    def test_missing_external_tree_is_a_noop(self):
        shutil.rmtree(self.root / "external")
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    # -- the real installer scripts ------------------------------------
    def test_real_pin_tables_are_parseable(self):
        # Guards against a pin-table refactor silently disabling the gate:
        # one tree per platform script, stamped with the pin's own URL.
        for script_name, target in (("install_dependencies.ps1", "imgui"),
                                    ("install_dependencies.sh", "imgui")):
            script = REPO_ROOT / script_name
            if not script.is_file():
                continue
            with self.subTest(script=script_name):
                self.tree(target, "imgui", "https://example.invalid/stale",
                          "beef", {"imgui.h": DOCKING_HEADER})
                r = self.check(pins=script)
                out = self.assert_fails(r, "Stale vendored dependencies")
                self.assertIn("imgui/tar.gz/", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
