#!/usr/bin/env python3
"""Safe, platform-independent input script validation tests (no GUI needed)."""
import io
import json
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import input_emulator as emulator


class InputEmulatorTests(unittest.TestCase):
    def test_valid_script_and_pipe(self):
        script = ('{"action":"key_down","key":"w"}\n'
                  '{"action":"wait","seconds":0.1}\n'
                  '{"action":"key_up","key":"w"}\n'
                  '{"action":"screenshot","path":"out/smoke.png"}\n')
        events = emulator.parse_script(io.StringIO(script))
        self.assertEqual(len(events), 4)
        out = io.StringIO()
        with patch.object(sys, "stdin", io.StringIO(script)):
            with redirect_stdout(out):
                rc = emulator.main(["validate", "--stdin", "--json"])
        self.assertEqual(rc, 0)
        self.assertFalse(json.loads(out.getvalue())["executed"])

    def test_reject_invalid_keys_and_unreleased_keys(self):
        with self.assertRaises(emulator.InputError):
            emulator.parse_script(['{"action":"key_down","key":"w"}'])
        with self.assertRaises(emulator.InputError):
            emulator.parse_script(['{"action":"key","key":"win"}'])
        with self.assertRaises(emulator.InputError):
            emulator.parse_script(['{"action":"wait","seconds":500}'])
        with self.assertRaises(emulator.InputError):
            emulator.parse_script(['{"action":"click","x":-1,"y":0}'])

    def test_execute_needs_target_window(self):
        with self.assertRaises(SystemExit) as result:
            emulator.main(["run", "--stdin", "--execute"])
        self.assertEqual(result.exception.code, 2)

    def test_unknown_fields_refused(self):
        with self.assertRaises(emulator.InputError):
            emulator.parse_script(['{"action":"key","key":"tab","shell":"echo hello"}'])


if __name__ == "__main__":
    unittest.main()
