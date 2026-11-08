#!/usr/bin/env python3
# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
"""Compare annotated C++ GPU POD layouts against DXC's SPIR-V reflection.

Requires a C++20 compiler, DXC and spirv-dis (Vulkan SDK / SPIRV-Tools).
No GPU or running renderer is needed.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CPP_FILES = [
    ROOT / "engine/include/Engine/Core/double_float.h",
    ROOT / "engine/include/Engine/Renderer/gpu_scene.h",
]
HLSL_FILES = [
    ROOT / "engine/shaders/double_float.hlsli",
    ROOT / "engine/shaders/gpu_scene.hlsli",
]
ANNOTATION = re.compile(r"(?:^|\n)\s*(?://\s*)?GPU_STRUCT_NAME\((\w+)\)\s*\n\s*struct\s+(?:alignas\(\d+\)\s+)?(\w+)\s*\{", re.M)
MEMBER = re.compile(r"^\s*(?:(?:const|volatile)\s+)?[\w:<> ,]+\s+(\w+)\s*(?:\{[^;]*\}|=[^;]*)?;\s*(?://.*)?$")
SPV_MEMBER = re.compile(r'OpMemberName\s+(%\S+)\s+(\d+)\s+"([^"]+)"')
SPV_OFFSET = re.compile(r'OpMemberDecorate\s+(%\S+)\s+(\d+)\s+Offset\s+(\d+)')
SPV_STRIDE = re.compile(r'OpDecorate\s+(%\S+)\s+ArrayStride\s+(\d+)')
SPV_RUNTIME_ARRAY = re.compile(r'(%\S+)\s*=\s*OpTypeRuntimeArray\s+(%\S+)')
SPV_STRUCT = re.compile(r'(%\S+)\s*=\s*OpTypeStruct\b')
SPV_NAME = re.compile(r'OpName\s+(%\S+)\s+"([^"]+)"')


def declarations(path, cpp):
    source = path.read_text(encoding="utf-8")
    found = {}
    for match in ANNOTATION.finditer(source):
        shader_name, name = match.groups()
        # Walk braces, not a regex that stops on aggregate member initializers.
        brace = source.index("{", match.start(2) + len(name))
        depth = 1
        end = brace + 1
        while depth and end < len(source):
            if source[end] == "{":
                depth += 1
            elif source[end] == "}":
                depth -= 1
            end += 1
        if depth:
            raise ValueError(f"{path}: unclosed {name}")
        body = source[brace + 1:end - 1]
        fields = []
        for line in body.splitlines():
            m = MEMBER.match(line)
            if m:
                fields.append(m.group(1))
        if not fields:
            raise ValueError(f"{path}: {name} has no supported fields")
        prefix = source[:match.start()]
        namespaces = list(re.finditer(r"\bnamespace\s+([\w:]+)\s*\{", prefix))
        ns = namespaces[-1].group(1) if cpp and namespaces else ""
        if shader_name in found:
            raise ValueError(f"{path}: duplicate annotation {shader_name}")
        found[shader_name] = (f"{ns}::{name}" if ns else name, fields, path)
    return found


def inspect_spirv(disassembly, shader_name):
    names = dict(SPV_NAME.findall(disassembly))
    structs = set(SPV_STRUCT.findall(disassembly))
    struct_ids = [sid for sid in structs if names.get(sid) == shader_name]
    if len(struct_ids) != 1:
        raise ValueError(f"{shader_name}: expected one SPIR-V struct, found {len(struct_ids)}")
    sid = struct_ids[0]
    member_names = {(s, int(i)): n for s, i, n in SPV_MEMBER.findall(disassembly)}
    member_offsets = {(s, int(i)): int(n) for s, i, n in SPV_OFFSET.findall(disassembly)}
    fields = {}
    for (s, i), name in member_names.items():
        if s == sid:
            if (s, i) not in member_offsets:
                raise ValueError(f"{shader_name}.{name}: missing Offset decoration")
            fields[name] = member_offsets[s, i]
    strides = dict((s, int(n)) for s, n in SPV_STRIDE.findall(disassembly))
    arrays = SPV_RUNTIME_ARRAY.findall(disassembly)
    candidates = [strides[a] for a, element in arrays if element == sid and a in strides]
    if len(candidates) != 1:
        raise ValueError(f"{shader_name}: missing/ambiguous StructuredBuffer ArrayStride")
    return candidates[0], fields


def run(command, **kwargs):
    completed = subprocess.run(command, text=True, capture_output=True, **kwargs)
    if completed.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))}\n{completed.stdout}\n{completed.stderr}")
    return completed.stdout


def verify(cxx, dxc, spirv_dis, cpp_files, hlsl_files):
    cpu = {}
    gpu = {}
    for path in cpp_files:
        for key, value in declarations(path, True).items():
            if key in cpu:
                raise ValueError(f"Duplicate C++ annotation: {key}")
            cpu[key] = value
    for path in hlsl_files:
        for key, value in declarations(path, False).items():
            if key in gpu:
                raise ValueError(f"Duplicate HLSL annotation: {key}")
            gpu[key] = value
    if not cpu or not gpu:
        raise ValueError("No annotated GPU structs found")
    if cpu.keys() != gpu.keys():
        raise ValueError(f"Unpaired structs: C++ only {sorted(cpu.keys() - gpu.keys())}, "
                         f"HLSL only {sorted(gpu.keys() - cpu.keys())}")

    with tempfile.TemporaryDirectory(prefix="gpu-struct-layout-") as directory:
        temp = Path(directory)
        headers = sorted({p for _, _, p in cpu.values()})
        includes = "\n".join(f'#include "{p.as_posix()}"' for p in headers)
        code = [includes, "#include <cstddef>", "#include <iostream>", "int main() {"]
        for key, (typename, fields, _) in sorted(cpu.items()):
            code.append(f' std::cout << "{key}|size|" << sizeof({typename}) << "\\n";')
            for field in fields:
                code.append(f' std::cout << "{key}|{field}|" << offsetof({typename}, {field}) << "\\n";')
        code.append("}")
        source = temp / "layout.cpp"
        source.write_text("\n".join(code), encoding="utf-8")
        exe = temp / ("layout.exe" if os.name == "nt" else "layout")
        command = [cxx, "-std=c++20", "-I" + str(ROOT / "engine/include"),
                   "-I" + str(ROOT / "core/include"),
                   "-I" + str(ROOT / "external/glm"), str(source), "-o", str(exe)]
        # MSVC uses different CLI switches.
        if Path(cxx).stem.lower() in ("cl", "cl.exe"):
            command = [cxx, "/nologo", "/std:c++20",
                       "/I" + str(ROOT / "engine/include"),
                       "/I" + str(ROOT / "core/include"),
                       "/I" + str(ROOT / "external/glm"),
                       str(source), "/Fe:" + str(exe)]
        run(command)
        cpu_layout = {}
        for line in run([str(exe)]).splitlines():
            name, field, offset = line.split("|")
            cpu_layout.setdefault(name, {})[field] = int(offset)

        errors = []
        for key, (_, fields, hlsl_path) in sorted(gpu.items()):
            if set(fields) != set(cpu[key][1]):
                errors.append(f"{key}: member mismatch C++={cpu[key][1]} HLSL={fields}")
                continue
            probe = temp / f"{key}.cs.hlsl"
            # A writable structured buffer keeps the annotated type live in SPIR-V.
            probe.write_text(
                f'#include "{hlsl_path.name}"\n'
                f'RWStructuredBuffer<{key}> layoutProbe : register(u0);\n'
                '[numthreads(1,1,1)] void mainCS(uint3 id : SV_DispatchThreadID) {\n'
                f'    layoutProbe[id.x] = ({key})0;\n'
                '}\n', encoding="utf-8")
            spv = temp / f"{key}.spv"
            run([dxc, "-spirv", "-T", "cs_6_7", "-E", "mainCS",
                 "-fvk-use-dx-layout", "-I", str(ROOT / "engine/shaders"),
                 "-Fo", str(spv), str(probe)])
            shader_size, shader_fields = inspect_spirv(run([spirv_dis, str(spv)]), key)
            actual = cpu_layout[key]
            if actual["size"] != shader_size:
                errors.append(f"{key}: sizeof C++={actual['size']} HLSL stride={shader_size}")
            for field in fields:
                if field not in shader_fields:
                    errors.append(f"{key}.{field}: missing SPIR-V member")
                elif actual[field] != shader_fields[field]:
                    errors.append(f"{key}.{field}: offsetof C++={actual[field]} "
                                  f"HLSL={shader_fields[field]}")
            print(f"{key}: C++ sizeof={actual['size']}, HLSL stride={shader_size}")
        if errors:
            raise ValueError("GPU layout mismatches:\n  " + "\n  ".join(errors))
        print(f"Verified {len(gpu)} C++/HLSL struct pairs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--dxc", default=os.environ.get("DXC", "dxc"))
    parser.add_argument("--spirv-dis", default=os.environ.get("SPIRV_DIS", "spirv-dis"))
    parser.add_argument("--cpp", type=Path, nargs="+", default=CPP_FILES)
    parser.add_argument("--hlsl", type=Path, nargs="+", default=HLSL_FILES)
    args = parser.parse_args()
    try:
        verify(args.cxx, args.dxc, args.spirv_dis, args.cpp, args.hlsl)
    except (ValueError, RuntimeError, OSError) as exc:
        print(f"GPU struct layout test failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
