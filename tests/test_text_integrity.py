"""Regression tests for encoding damage caused by local agent file editing."""
from pathlib import Path
import importlib.util
import unittest

CHECKER = Path(__file__).resolve().parents[1] / "tools" / "check_text_integrity.py"
spec = importlib.util.spec_from_file_location("check_text_integrity", CHECKER)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

REPO_ROOT = Path(__file__).resolve().parents[1]


class TextIntegrityTests(unittest.TestCase):
    def test_ascii_hyphen_and_unicode_comments_are_safe(self):
        content = (
            "# An em dash — is safe in commentary.\n"
            "param([switch]$Force)\n"
            "Write-Output -InputObject 'hello'\n"
        ).encode("utf-8")
        self.assertEqual([], checker.check_text(Path("script.ps1"), content))

    def test_non_ascii_dash_in_powershell_code_is_rejected(self):
        for dash in sorted(checker.DASH_LOOKALIKES):
            with self.subTest(dash=hex(ord(dash))):
                content = f"Write-Output {dash}InputObject 'hello'\n".encode("utf-8")
                errors = checker.check_text(Path("script.ps1"), content)
                self.assertTrue(errors, dash)
                self.assertIn("non-ASCII dash", errors[0])

    def test_replacement_character_is_rejected(self):
        errors = checker.check_text(Path("script.ps1"), "Write-Output �\n".encode("utf-8"))
        self.assertTrue(any("U+FFFD" in error for error in errors))

    def test_mojibake_is_rejected(self):
        errors = checker.check_text(Path("script.ps1"), "# Wrong â€” encoding\n".encode("utf-8"))
        self.assertTrue(any("mojibake" in error for error in errors))

    def test_mojibake_in_markdown_is_rejected(self):
        errors = checker.check_text(Path("docs/notes.md"), "damaged â€“ dash".encode("utf-8"))
        self.assertTrue(any("mojibake" in error for error in errors))

    def test_invalid_utf8_is_rejected(self):
        errors = checker.check_text(Path("script.ps1"), b"Write-Output \xff\n")
        self.assertTrue(any("invalid UTF-8" in error for error in errors))

    def test_unicode_text_is_not_banned_in_other_sources(self):
        self.assertEqual([], checker.check_text(Path("example.py"), "'café — résumé'\n".encode("utf-8")))

    def test_tracked_tree_is_clean(self):
        # THE enforcement: the whole tracked tree scans clean (vendored
        # external/ and the checker's own fixture files are exempted by
        # the tool). This single test is the "unicode contamination
        # verification" - CI runs it on every push/PR, and agents can
        # run it locally with one unittest command.
        count, errors = checker.scan(REPO_ROOT)
        self.assertEqual([], errors)
        self.assertGreater(count, 1000)


if __name__ == "__main__":
    unittest.main()
