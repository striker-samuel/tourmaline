# Directional cubemap face demand for virtual texturing

This is the CPU policy foundation for **per-face demand**; it does not
yet enable sparse cubemap allocation or change IBL sampling behavior.

## Why three faces is not a general IBL guarantee

The three outward-facing sides of an axis-aligned box are not the same
as the faces required by a cubemap lookup. A specular IBL lookup uses
the *reflected* ray and can point behind the viewer; diffuse convolution
and rough prefiltered specular sampling integrate a broad angular
neighborhood and may depend on every face. A camera-to-probe direction
alone therefore cannot safely cull IBL faces.

`smvk::CubemapFacesForCone(direction, halfAngleRadians)` takes a direction
in the cubemap's lookup space and an angular upper bound on **all actual
samples**. Its mask conservatively includes every cube face that could
intersect that cone. It may over-request at face edges, by design. A
zero-angle request for a face-corner direction can include three faces;
an unbounded IBL request returns all six. Invalid inputs also return
all six. Callers should OR masks over contributing reflective surfaces,
view tiles and sampling lobes rather than selecting faces by camera
direction only.

## Path to real residency savings

1. Keep a **complete, low-resolution six-face mip tail** as a fallback,
   including for far-away probes. Otherwise missing faces can turn into
   black reflections, invalid sampling, or garbage.
2. Make each high-resolution face/mip a separately addressable page or
   physical allocation. The existing `TextureStreamer` retargets entire
   images to a contiguous mip tail; `CreateStreamingTexture` does not
   currently expose face-selective residency.
3. Build a demand producer using reflection directions and conservative
   roughness cone bounds, union masks for all shader consumers and account
   for trilinear filtering at mip/face seams. Diffuse, uncertain
   rough-specular, skybox, and consumers with no safe cone request all
   six faces.
4. Stream selected high-res pages into a **sparse-resident cube** (where
   supported), or into an indirection-backed face array with explicit
   sampling and fallback. Ordinary six-layer `TextureCube` allocation is
   not reduced by merely skipping a copy to the unused layers.
5. Use hysteresis, cross-face border coverage, async transfer fences and
   GPU-safe eviction. Compare resident **physical bytes**, upload bytes,
   hitches and image quality, not only the number of selected faces.
6. Integrate with the existing world-partition capture tiers. Outside
   their far range the environment sky/IBL tier remains authoritative.

## Safe integration contract

The face mask is demand metadata only: never publish an incompletely
initialized cube to an existing bindless `TextureCube` slot. The current
runtime keeps loading complete KTX2 cubemaps and is intentionally
unchanged by this PR. The maximum theoretical face-only reduction for
high-resolution detail with a complete tail is about 50% when exactly
three of six faces are actually needed; measured savings may be much
smaller after lobe expansion, mip overlap and resident fallback.

## Validation

`cubemap_tests` checks axis/corner demand, broad cones, fail-open inputs,
and a sampled ray sweep proving that sampled face directions are always
included. Run the existing CPU cubemap suite; GPU integration and Vulkan
walk/teleport/roughness validation are required before enabling actual
per-face streaming.
