#!/usr/bin/env python3
"""Verify the C++/HLSL push-constant mirror against push_constants.yaml.

Usage:
  python codegen.py --check   # parse Renderer.h static_asserts + push_constants.hlsli field order, compare to YAML
  python codegen.py --emit    # print canonical C++ and HLSL snippets (for manual sync, not auto-write)

The repo norm is heavy explanatory comments + manual mirrors; this tool makes
drift a CI failure instead of async device loss. No dependencies (stdlib only,
tiny YAML-subset parser for this file's shape).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
YAML = Path(__file__).with_name("push_constants.yaml")
RENDERER_H = ROOT / "engine" / "include" / "Engine" / "Renderer" / "Renderer.h"
PC_HLSLI = ROOT / "engine" / "shaders" / "push_constants.hlsli"
COMMON_HLSLI = ROOT / "engine" / "shaders" / "common.hlsli"


def parse_yaml(path: Path):
    fields = []
    total = scratch = None
    for line in path.read_text().splitlines():
        m = re.match(r"total_size:\s*(\d+)", line)
        if m:
            total = int(m.group(1))
        m = re.match(r"scratch_start:\s*(\d+)", line)
        if m:
            scratch = int(m.group(1))
        m = re.match(r"\s*-\s*\{\s*(.*)\}\s*$", line)
        if m:
            body = m.group(1)
            kv = {}
            for km in re.finditer(r'(\w+):\s*(?:"([^"]*)"|([^,}]+))', body):
                k, v2, v3 = km.group(1), km.group(2), km.group(3)
                kv[k] = v2 if v2 else v3.strip()
            kv["offset"] = int(kv["offset"])
            kv["size"] = int(kv["size"])
            fields.append(kv)
    return total, scratch, fields


def parse_cpp_asserts(path: Path):
    text = path.read_text()
    asserts = dict(re.findall(
        r'static_assert\(offsetof\(IRenderer::PushConstants,\s*(\w+)\)\s*==\s*(\d+)', text))
    asserts = {k: int(v) for k, v in asserts.items()}
    m = re.search(r'static_assert\(sizeof\(IRenderer::PushConstants\)\s*==\s*(\d+)', text)
    total = int(m.group(1)) if m else None
    return total, asserts


def parse_hlsl_fields(path: Path):
    # Field order as declared in struct PushConstants { ... }; names normalized
    # (bLights->lightsBuffer, bMats->materialsBuffer for comparison).
    text = path.read_text()
    m = re.search(r"struct PushConstants\s*\{(.*?)\};", text, re.S)
    if not m:
        return []
    body = m.group(1)
    names = re.findall(r"(?:uint|float|float4x4|float3x4|uint4|vk::BufferPointer<[^>]+>)\s+(\w+)\s*;", body)
    norm = {"bLights": "lightsBuffer", "bMats": "materialsBuffer", "padEnd": "padEnd_"}
    return [norm.get(n, n) for n in names]


def main() -> int:
    total, scratch, fields = parse_yaml(YAML)
    errors = []

    # 1. YAML self-consistency: offsets contiguous, total exact, scratch rule.
    cursor = 0
    for f in fields:
        if f["offset"] != cursor:
            errors.append(f"YAML gap/overlap before {f['name']}: cursor={cursor} offset={f['offset']}")
        cursor = f["offset"] + f["size"]
    if cursor != total:
        errors.append(f"YAML total {cursor} != declared {total}")
    for f in fields:
        if f["offset"] < scratch and f["offset"] + f["size"] > scratch:
            errors.append(f"YAML field {f['name']} straddles scratch boundary {scratch}")
        if f["offset"] >= scratch and f["name"] not in ("prevModel", "padEnd_"):
            errors.append(f"YAML field {f['name']} in scratch 192..256 without documented tail contract")

    # 2. C++ asserts cover every field.
    cpp_path = RENDERER_H
    cpp_total, cpp_asserts = parse_cpp_asserts(cpp_path)
    if cpp_total != total:
        errors.append(f"Renderer.h sizeof assert {cpp_total} != YAML {total}")
    for f in fields:
        if f["name"] not in cpp_asserts:
            errors.append(f"Renderer.h missing offsetof assert for {f['name']}")
        elif cpp_asserts[f["name"]] != f["offset"]:
            errors.append(f"Renderer.h {f['name']} offset {cpp_asserts[f['name']]} != YAML {f['offset']}")

    # 3. HLSL declares the same fields in the same order.
    hlsl_path = PC_HLSLI if PC_HLSLI.exists() else COMMON_HLSLI
    hlsl_names = parse_hlsl_fields(hlsl_path)
    yaml_names = [f["name"] for f in fields]
    if hlsl_names != yaml_names:
        errors.append(f"HLSL field order mismatch ({hlsl_path.name}):\n  hlsl={hlsl_names}\n  yaml={yaml_names}")

    if "--emit" in sys.argv:
        print("// Canonical C++ asserts (paste into Renderer.h):")
        for f in fields:
            print(f"static_assert(offsetof(IRenderer::PushConstants, {f['name']}) == {f['offset']},")
            print(f'    "{f["name"]} mirror drift");')
        print("// Canonical HLSL order (struct PushConstants):")
        for f in fields:
            print(f"//   {f['hlsl']};  // @{f['offset']} size {f['size']}")

    if errors:
        print("PUSH-CONSTANT MIRROR DRIFT:")
        for e in errors:
            print(f"  - {e}")
        return 1
    print(f"OK: {len(fields)} fields, {total}B, C++ asserts + {hlsl_path.name} order match.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
