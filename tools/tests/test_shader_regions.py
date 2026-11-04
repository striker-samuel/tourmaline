"""Diff-based identity tests for independently addressable shader loops."""
import importlib.util
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import shader_regions as regions


SHADER = """float4 mainPS(float2 uv : TEXCOORD0) : SV_Target {
    float sum = 0;
    for (int i = 0; i < 4; ++i) { sum += uv.x; }
    if (sum > 0) { sum *= 2; }
    for (int j = 0; j < 8; ++j) { sum += uv.y; }
    return sum;
}
"""


class ShaderRegionsTests(unittest.TestCase):
    def test_discovers_two_distinct_loops_and_section(self):
        found = regions.discover(SHADER)
        self.assertEqual(len([r for r in found if r["kind"] == "loop"]), 2)
        self.assertTrue(any(r["kind"] == "section" and r["function"] == "mainPS" for r in found))

    def test_diff_preserves_identity_after_insertions(self):
        before = regions.discover(SHADER)
        target = [r for r in before if r["kind"] == "loop"][1]
        rewritten = SHADER.replace("    float sum = 0;", "    float sum = 0;\n    float additional = 1;")
        after = regions.preserve_identity(SHADER, rewritten, before)
        mapped = [r for r in after if r["id"] == target["id"]]
        self.assertEqual(len(mapped), 1)
        self.assertEqual(mapped[0]["status"], "matched")
        self.assertEqual(mapped[0]["line"], target["line"] + 1)
        edited, hints = regions.apply_identified_regions(
            rewritten, {target["id"]: "unroll(4)"}, after)
        self.assertIn("[unroll(4)] for (int j", edited)
        self.assertNotIn("[unroll(4)] for (int i", edited)
        self.assertEqual(hints[0]["id"], target["id"])

    def test_hash_is_stable_when_ai_inserts_loop_before_target(self):
        original = regions.discover(SHADER)
        target = next(r for r in original if r["kind"] == "loop"
                      and "int j" in r["header"])
        new_loop = "    for (int k = 0; k < 2; ++k) { sum += 1; }\\n"
        rewritten = SHADER.replace("    float sum = 0;\\n",
                                   "    float sum = 0;\\n" + new_loop)
        discovered = regions.discover(rewritten)
        # The hash addresses the loop itself, not its ordinal in the file.
        same = [r for r in discovered if r["id"] == target["id"]]
        self.assertEqual(len(same), 1)
        self.assertEqual(same[0]["header"], target["header"])
        self.assertEqual(same[0]["line"], target["line"] + 1)
        mapped = regions.preserve_identity(SHADER, rewritten, original)
        tracked = next(r for r in mapped if r["id"] == target["id"])
        self.assertEqual(tracked["status"], "matched")
        result, changes = regions.apply_identified_regions(
            rewritten, {target["id"]: "unroll(4)"}, mapped)
        self.assertIn("[unroll(4)] for (int j", result)
        self.assertNotIn("[unroll(4)] for (int k", result)
        self.assertEqual(changes[0]["id"], target["id"])

    def test_fail_closed_on_duplicate_loop_headers(self):
        duplicate = SHADER.replace("for (int j = 0; j < 8; ++j)",
                                   "for (int i = 0; i < 4; ++i)")
        found = regions.discover(duplicate)
        duplicated = [r for r in found if r["kind"] == "loop"]
        self.assertTrue(all(r["ambiguous"] for r in duplicated))
        with self.assertRaises(ValueError):
            regions.apply_identified_regions(duplicate, {duplicated[0]["id"]: "loop"}, found)

    def test_invalid_hint_fails(self):
        found = regions.discover(SHADER)
        loop = next(r for r in found if r["kind"] == "loop")
        with self.assertRaises(ValueError):
            regions.apply_identified_regions(SHADER, {loop["id"]: "flatten"}, found)

    def test_removed_region_is_lost(self):
        before = regions.discover(SHADER)
        loop = next(r for r in before if r["kind"] == "loop")
        changed = SHADER.replace("    for (int i = 0; i < 4; ++i) { sum += uv.x; }\n", "")
        result = regions.preserve_identity(SHADER, changed, before)
        missing = next(r for r in result if r["id"] == loop["id"])
        self.assertEqual(missing["status"], "lost")


if __name__ == "__main__":
    unittest.main()
