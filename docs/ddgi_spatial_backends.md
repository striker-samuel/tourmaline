# DDGI spatial tracing: specialized backends

Geometry remains authoritative. TLAS/BLAS, mesh ADFs and future voxel structures are derived acceleration data with concrete ownership and layouts. Do not introduce a virtual query service, runtime per-ray backend switch, universal node format or registry. Select a specialized compute pipeline on the CPU. Share only meaningful query semantics (nearest hit, occlusion, distance where native), lighting helpers and DDGI's 32-byte RaySample ABI.

## First slice (experimental, not exposed in WorldRenderer)

- Hardware RT remains the default and its 56-byte push ABI is unchanged.
- SDF uses the same ray generation, sky, output and blend, with a separate 64-byte push ABI. No TLAS or hardware ray queries. Compact emission and one shadowed sun term; no local lights or previous-history bounce yet.
- SDF retains its native octree and uses a compile-time variant for outward-facing classification, budget exhaustion and early-out shadow queries without normal/material reconstruction. SSR retains existing semantics. Unknown rays invalidate the probe rather than contributing sky. Quantized fields remain approximate.
- Volume::Create accepts a fixed TraceBackend; AddPasses for StaticSdf requires the scene BDA and six rendergraph buffer handles (header, instances, nodes, children, leaves, materials). The caller owns upload ordering, complete coverage and GPU lifetime. Backend changes require retirement, recreation and history reset.
- CMake discovers the new .cs.hlsl via its existing shader glob.

## Integration gates (not implemented here)

StaticSdfGpuScene is camera-prioritized, capped at 128 instances, static-only, and linearly scans those instances for every ray. A nonempty SDF is not proof of coverage. Do not wire existing SSR residency into DdgiActive. Keep the hardware gates intact until:

1. Publish coverage of active probe volumes expanded by the primary and shadow ray reach, or invalidate uncovered probes. Handle missing, dynamic, thin and unsigned geometry. Certify coverage independently of the camera; reset affected histories on generation changes.
2. Add a top-level instance BVH or grid, reuse mesh octrees. Benchmark against linear scan: 2048 probes x 128 rays x 128 instances can reach 33,554,432 AABB tests per volume update, before shadow rays.
3. Register concrete buffers and synchronize upload -> trace -> blend -> Main. No flight overwrites previous readers. Use backend-specific availability in both publish and emission gates; verify no-RT devices.
4. Validate lighting, energy, visibility, thin walls, probe interiors and camera stability. Add previous-history diffuse bounce with clamp, never read current output history. Compare RT and SDF with a reference (RT tracer is still diagnostic).
5. Add voxel/hybrid variants only when concrete producers exist. Hybrid traces nonduplicated geometry sets and selects nearest valid hit.

## Validation before enabling

Build App and Benchmark; compile/package both shaders (inspect SDF SPIR-V for absence of RayQuery capability); run CTest and text-integrity suite. GPU validation: empty/partial residency, step exhaustion, closed rooms/thin walls, shadowed colored surfaces, camera changes, toggling and scene reload with multiple flights. Record GPU pass times, throughput, quality and VRAM in the existing perf harness. No build or GPU results are asserted.
