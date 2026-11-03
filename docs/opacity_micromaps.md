# Opacity micromaps for hardware ray-query shadows (experimental)

Tourmaline's optional hardware ray-query renderer can attach **Vulkan
`VK_EXT_opacity_micromap`** micromaps to the BLAS of alpha-MASK static
triangles. Hardware traversal ignores transparent subtriangles instead
of treating foliage, grates, and fences as fully opaque ray blockers.
The raster CSM/EVSM path remains active for receivers and unsupported
hardware.

This feature builds on the acceleration-structure RHI in the Vulkan
hardware ray-shadow PR. It does **not** affect the screen-space contact
shadow ray marcher.

## Enable

In **Lighting**, enable **Hardware ray-traced sun shadows (KHR ray
query)**. If the Vulkan driver supports
`VK_EXT_opacity_micromap` with `VkPhysicalDeviceOpacityMicromapFeaturesEXT
::micromap` and subdivision level 2, the additional **Opacity
micromaps (alpha MASK foliage)** checkbox is available (enabled by
default within the hardware ray-shadow mode).

On devices without the optional OMM extension, masked surfaces are
not inserted into the ray-query TLAS as opaque impostors; the existing
masked PCF/EVSM depth passes continue to cast those shadows. Opaque
static triangle casters use ordinary BLAS as before. Other materials,
including alpha BLEND, skinned geometry, and layered MASK, are not
treated as OMM candidates.

## Data flow

1. `WorldRenderer` walks static shadow casters, supplies the
   `RayTracingInstance` geometry/transform plus the material's
   alpha cutoff, base-color factor alpha, and resolved bindless
   base-color texture index. The BLAS deduplication key includes
   these material values, not just index ranges.
2. The backend's `opacity_micromap_bake.cs.hlsl` reads the packed
   shading UVs and indexed triangle list directly from Tourmaline's
   static GPU heaps. For each MASK triangle it evaluates the
   glTF-style alpha test (`baseColorFactor.a * texture.a >= cutoff`)
   at 16 microtriangle centroids (subdivision level **2**).
3. The compute shader packs one bit per microtriangle in the Vulkan
   specification's **bird-curve ordering** into a 32-bit word
   (the upper 16 bits are unused).
4. The RHI creates `VkMicromapEXT` objects, builds them using
   `vkCmdBuildMicromapsEXT`, then includes
   `VkAccelerationStructureTrianglesOpacityMicromapEXT` in the
   BLAS build geometry's `pNext` chain, using `VK_INDEX_TYPE_NONE_KHR`
   and one micromap entry per source triangle. The TLAS shares
   transformed instances of the resulting BLAS.
5. The hardware sun-shadow shader leaves the ray-query force-opaque
   flag **unset**. The traversal hardware uses the OMM bits to
   ignore transparent subtriangles and report opaque hits.
   No pixel-shader alpha test runs during the ray query.

Micromaps remain allocated for the lifetime of their associated BLAS.
Temporary build input/scratch resources are released only after the
submission completes. Scene changes or streamed texture residency
changes invalidate the cache and rebuild the static BLAS/TLAS and OMM
data; the initial implementation blocks the GPU for this update.
Only one active world TLAS is supported per RHI context, as in the
underlying hardware ray-shadow implementation.

## Known limitations

* **2-state approximation:** each microtriangle is classified using
  its *centroid* sampled at texture LOD0. A microtriangle crossing an
  alpha edge can be misclassified. This is not conservative
  texel-footprint classification, 4-state unknown coverage, or a
  per-intersection any-hit shader. Thin leaves may alias.
* The bake uses base-color texture alpha and factor alpha, following
  the current single-layer material evaluation. It intentionally skips
  layered MASK materials and does not support separate alpha
  textures, animation, or vertex-color opacity.
* The build is synchronous, potentially costly for large worlds or
  frequently streaming/animated texture sets. Geometry with more
  than 4,194,240 triangles in a single MASK surface currently cannot
  be baked in one compute dispatch; a failed build leaves raster
  shadows active.
* `VK_EXT_opacity_micromap` support is **not** implied by having
  ray-query-capable hardware. This prototype doesn't use the newer
  incompatible `VK_KHR_opacity_micromap` API.
* Shader/ABI compilation, validation-layer testing, and on-GPU
  screenshots are necessary before promoting the draft PR.

## Checks

Use `RendergraphContractTests` for the extended RHI instance ABI and
default settings. On an EXT-capable GPU, compare ray shadows before
and after enabling OMM on alpha-MASK leaves/fences, with the Contact
Shadows debug view. Also test a GPU without OMM, scene reload,
masked/opaque instances sharing mesh ranges, full/half resolution,
mip-streaming promotions, and shader hot reload.

Reference: [Vulkan VK_EXT_opacity_micromap specification](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html)
and [ray traversal opacity micromap semantics](https://docs.vulkan.org/spec/latest/chapters/raytraversal.html).
