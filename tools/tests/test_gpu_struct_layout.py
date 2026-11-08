# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""GPU struct marker and SPIR-V layout reflection regression tests."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_gpu_struct_layout as layout


class GpuLayoutTests(unittest.TestCase):
    def test_cpp_annotation_and_aggregate_initializers(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "sample.h"
            path.write_text(
                "namespace foo::bar {\n"
                "GPU_STRUCT_NAME(ShaderData)\n"
                "struct alignas(16) CpuData {\n"
                "    uint32_t first = 0;\n"
                "    glm::uvec4 refs{1u, 2u, 3u, 4u}; // comment\n"
                "};\n}\n", encoding="utf-8")
            found = layout.declarations(path, True)
            self.assertEqual(found["ShaderData"][:2],
                             ("foo::bar::CpuData", ["first", "refs"]))

    def test_shader_comment_annotation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "sample.hlsli"
            path.write_text(
                "// GPU_STRUCT_NAME(ShaderData)\n"
                "struct ShaderData { uint4 value; };\n", encoding="utf-8")
            self.assertEqual(layout.declarations(path, False)["ShaderData"][1],
                             ["value"])

    def test_reflection_member_offsets_and_stride(self):
        dump = '''
OpName %12 "ShaderData"
OpMemberName %12 0 "a"
OpMemberName %12 1 "b"
OpMemberDecorate %12 0 Offset 0
OpMemberDecorate %12 1 Offset 16
OpDecorate %13 ArrayStride 32
%12 = OpTypeStruct %v4float %v4float
%13 = OpTypeRuntimeArray %12
'''
        self.assertEqual(layout.inspect_spirv(dump, "ShaderData"),
                         (32, {"a": 0, "b": 16}))
        with self.assertRaisesRegex(ValueError, "missing Offset"):
            layout.inspect_spirv(dump.replace("Offset 16", "OffsetRemoved 16"),
                                 "ShaderData")
        with self.assertRaisesRegex(ValueError, "ArrayStride"):
            layout.inspect_spirv(dump.replace("ArrayStride", "BrokenStride"),
                                 "ShaderData")


if __name__ == "__main__":
    unittest.main()
