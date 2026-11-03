# Hardware-accelerated sun shadows (Vulkan KHR ray query)

Tourmaline's `WorldRenderer` supports an **experimental, opt-in hardware
ray-query sun-shadow path**. It builds triangle acceleration structures
(BLAS) from the renderer's shared static position/index GPU monoblocks,
instantiates them through a TLAS with world transforms, and traces the
sun's visibility from visible opaque receivers in
`raytraced_sun_shadow.cs.hlsl`. This is real Vulkan hardware ray traversal,
not screen-space depth marching. The resulting visibility texture is
`min()`-combined with CSM/EVSM in the existing lit shader, preserving
raster shadow maps for receiver material compatibility and fallback.

## Requirements and usage

* Vulkan 1.3 device with `VK_KHR_deferred_host_operations`,
  `VK_KHR_acceleration_structure`, and `VK_KHR_ray_query` extensions,
  and the corresponding `accelerationStructure` / `rayQuery` features.
  All are optional. On unsupported GPUs, `WorldRenderer` retains
  its existing contact/CSM/EVSM behavior.
* Use the Lighting panel's **Hardware ray-traced sun shadows (KHR ray
  query)** checkbox. The mode is off by default. Tune maximum sun-ray
  distance, ray origin bias (self-hit suppression), and half/full
  visibility resolution. The same settings are available through
  `RenderQualityConfig`.
* The ray-traced pass reuses the original `SunContactShadows` R32F
  visibility target, depth receiver prepass, render graph write/read
  barriers, material receive bit, and the Contact Shadows debug view.
  Exactly one producer runs per frame. Disabling hardware rays
  automatically restores the original screen-space contact pass.

## RHI implementation

The RHI exposes `SupportsRayQueries()`,
`BuildRayTracingWorld(span<RayTracingInstance>)`,
`RayTracingWorldReady()`, and `ClearRayTracingWorld()`.
The Vulkan implementation probes and enables the optional features,
adds a per-flight acceleration-structure descriptor at binding 8 (the
binding is an inert storage-buffer slot on unsupported GPUs), gives
the static vertex/index heaps the AS build-input usage bits, and
creates a BLAS for each unique opaque indexed surface. All instances
of a surface share that BLAS. The top-level structure contains each
instance's world-space 3x4 matrix. RHI ownership includes the BLAS/TLAS
buffers and internal scratch/instance staging resources. A single GPU
submission builds the structures with appropriate build/build and
build/compute barriers. The TLAS descriptor is written to every
frame-in-flight set only after the build completes.

**This v1 builder blocks** on a changed caster list or transform to
avoid modifying an acceleration structure still being read by an
in-flight frame. Unchanged static scenes reuse the built TLAS. Moving
large numbers of casters can cause severe frame stalls; a production
next step is persistent BLAS caching and multi-flight asynchronous
TLAS update/refit. The RHI currently has one active TLAS per
`ResourceManager`, so overlapping worlds/viewports sharing an RHI
must not use this experimental path concurrently.

## Coverage and limitations

* The TLAS contains static opaque shadow-casting triangles and, on
  `VK_EXT_opacity_micromap` devices, optional alpha-MASK geometry baked
  into 2-state micromaps. See [opacity_micromaps.md](opacity_micromaps.md)
  for the GPU bake, alpha coverage approximation and fallback semantics.
  Skinned meshes, transparent alpha-BLEND materials, layered MASK and
  procedural terrain still depend on existing raster shadow maps.
* The hardware path traverses off-screen geometry, unlike contact
  shadows. It still uses the opaque depth prepass to determine receiving
  pixels and shares the per-material contact-shadow receive bit.
* BLAS geometry references the static GPU monoblocks (12-byte float3
  positions, 32-bit indexed surfaces); geometry stream reallocations
  trigger a rebuild when their raster surface offsets change.
* The compute pass uses a hard sun ray. Soft sun disc sampling,
  denoising, transparent/masked any-hit, skinned geometry,
  instanced multi-world descriptors, and incremental AS builds/refits
  are future work.
* Test on actual RT GPUs with Vulkan validation and DXC: compare
  ray on/off in Contact Shadows debug view, with PCF and EVSM,
  masked/skinned assets, moving casters, scene reloads, and full/half
  resolution. The CPU `RendergraphContractTests` pins the RHI
  input layout and opt-in defaults; it cannot verify the Vulkan build,
  shader compilation, or visual correctness.
