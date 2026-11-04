"""CPU-only verification of shader hint insertion and rejection."""
import importlib.util
from pathlib import Path
import unittest

MODULE = Path(__file__).resolve().parents[1] / "shader_hints.py"
spec = importlib.util.spec_from_file_location("hints_test", MODULE)
hints = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hints)


class ShaderHintTests(unittest.TestCase):
    def test_indexed_loop_modes(self):
        src = "for(int a=0;a<4;a++) {}\nfor(int b=0;b<8;b++) {}\n"
        out, mods = hints.apply_hints(src, loop_mode="unroll", loop_index=1)
        self.assertIn("[unroll] for(int b", out)
        self.assertTrue(out.startswith("for(int a"))
        self.assertEqual(mods[0]["index"], 1)
        rolled, _ = hints.apply_hints(src, loop_mode="loop")
        self.assertIn("[loop] for(int a", rolled)

    def test_branch_flatten(self):
        src = "if (a) {} if (b) {}"
        out, _ = hints.apply_hints(src, branch_mode="flatten", branch_index=1)
        self.assertEqual(out, "if (a) {} [flatten] if (b) {}")

    def test_comments_strings_preprocessor_ignored(self):
        src = ('// for(;;) {}\n'
               '#define TEST if(x)\n'
               'const char* s = "for(;;)";\n'
               '/* if(x) */\n'
               'for(int i=0;i<2;i++) {}\n')
        out, _ = hints.apply_hints(src, loop_mode="loop")
        self.assertIn("[loop] for(int i", out)
        self.assertEqual(out.count("[loop]"), 1)

    def test_existing_attributes_and_out_of_range_rejected(self):
        with self.assertRaisesRegex(ValueError, "already has an attribute"):
            hints.apply_hints("[unroll] for(int i=0;i<4;i++){}", loop_mode="loop")
        with self.assertRaisesRegex(ValueError, "not found"):
            hints.apply_hints("if(x) {}", loop_mode="unroll")
        with self.assertRaises(ValueError):
            hints.inject_attribute("if(x) {}", "if", "branch", -1)

    def test_auto_preserves_original_shader(self):
        src = "for(;;){}"
        self.assertEqual(hints.apply_hints(src), (src, []))


if __name__ == "__main__":
    unittest.main()
