"""CPU-only tests of the HLSL lexical scanner used by hashed regions."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from shader_hints import _code_mask


class HlslMaskTests(unittest.TestCase):
    def test_comments_strings_and_directives_are_masked(self):
        source = ('// for(;;)\n'
                  '#define TEST for(int x=0;x<2;x++)\n'
                  'const char* text = "for (;;)";\n'
                  '/* if(1) */\n'
                  'for (int j=0;j<3;j++) {}\n')
        masked = _code_mask(source)
        self.assertEqual(masked.count("for"), 1)
        self.assertNotIn("if(1)", masked)
        self.assertEqual(masked.count("\n"), source.count("\n"))

    def test_strings_with_escapes(self):
        source = 'const char* s="fake \\"for\\\" loop"; for(int i=0;i<2;i++){}'
        masked = _code_mask(source)
        self.assertEqual(masked.count("for"), 1)


if __name__ == "__main__":
    unittest.main()
