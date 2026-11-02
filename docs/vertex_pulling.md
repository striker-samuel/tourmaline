# Vertex pulling: opt-in scaffolding

This PR introduces **value-only CPU and HLSL helpers**, not a new raster
vertex-fetch mode. Its goal is to make packed-stream addressing testable
before modifying any Vulkan descriptor or graphics-pipeline ABI.

## Current contract

- Position: float3, 12 bytes per global vertex.
- Shading: packed color / half UV / oct normal / oct tangent, 16 bytes.
- Optional lightmap: packed chart UV / chart ID, 8 bytes.
- The position and shading streams share a global vertex ID.
- Lightmaps are suballocated **independently**; a per-draw base delta maps
  the global vertex ID to the optional stream.
- The filter's indirect firstInstance selects the GfxDraw record.
- The GPU-driven raster VS currently uses hardware vertex inputs; the
  mesh-shader path already pulls all three streams.

The helpers are in:
- `engine/include/Engine/Renderer/vertex_pull.h` (checked CPU windows and byte ranges).
- `engine/shaders/vertex_pull.hlsli` (unbound ByteAddressBuffer load helpers).

The shader functions are **not included by any live shader entry point**.
No new SPIR-V, descriptor sets, shader variants, shader push-constant
fields, or render behavior are added in this PR.

## Follow-up implementation (not in this PR)

1. Choose and benchmark access strategy for indexed raster shaders:
   a single globally bound storage buffer for optional attributes is
   preferable to expanding the existing large BDA push chain.
2. Audit bindless set 0 binding 7 before reserving it, including Vulkan
   descriptor lifetime and the DX12 boundary.
3. Wire optional lightmap loads into `gfxfiltered.vert.hlsl` first,
   preserving hardware position/shading fetch and all existing PSOs.
4. Restore direct/fallback per-mesh metadata separately; bytes 192–240
   overlap motion and skin tails, so the push ABI must be audited.
5. Benchmark full position/shading vertex pulling as a separate project.
   Do not make that refactor a prerequisite for lightmap restoration.

## Important correctness constraints

- A signed conceptual delta of -1 has unsigned representation
  `0xFFFFFFFF`. This is a **valid mapping**, not a reliable absence flag.
- Validate allocation windows and resource residency CPU-side; the
  unchecked shader helpers assume validated inputs.
- Avoid passing a giant typed buffer-pointer aggregate into the
  filtered VS: existing comments document NVIDIA pipeline-compiler hangs.
- Keep GPU-driven indirect batches grouped by material and SG lobe count;
  never split them by optional vertex-buffer allocation.

## Validation

`VertexPullTests` is a headless CTest that checks index mapping, wrap,
window containment, overflow and buffer byte bounds.

No GPU-path changes or image-quality claims are made.
