# C++ / HLSL GPU struct ABI tests

Run the headless layout contract test:

    python tools/check_gpu_struct_layout.py --cxx c++ --dxc dxc --spirv-dis spirv-dis

The test is registered as gpu_struct_layout_tests in CTest when Python 3,
DXC and spirv-dis (SPIRV-Tools) are available. No graphics device is needed.

## Annotations

Place GPU_STRUCT_NAME(Identifier) immediately before the C++ struct
declaration and include Engine/Renderer/gpu_struct_name.h. The macro is
empty and does not change layout. The C++ type may have another name.

In HLSL, place // GPU_STRUCT_NAME(Identifier) immediately before
the corresponding struct declaration.

Each shader identifier must occur exactly once in each language.
Unpaired structs are failures rather than silently skipped.

## Validation

The script compiles a generated C++20 executable to read sizeof and
offsetof for annotated fields. For every HLSL type, DXC compiles a
compute probe containing RWStructuredBuffer<T> into SPIR-V with
-fvk-use-dx-layout. SPIRV-Tools disassembly supplies member offsets
and the structured buffer's ArrayStride.

It compares member names, member byte offsets and struct size/stride.
Nested type layout changes that alter offsets or stride are detected.
Missing or ambiguous reflection also fails the test.

## Limitations

Supports simple POD-like structs and one field declaration per line.
No C++ inheritance, unions, bitfields or multiple fields per declaration.
The test does not compare scalar signedness, exact field types or matrix
major order when those changes preserve offsets and stride.
Template Row<T> instantiations are not yet covered.

To extend coverage, annotate further pairs and add their header paths
to the checker defaults (or supply --cpp and --hlsl).
