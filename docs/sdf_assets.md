# Mesh-local signed-distance assets (experimental)

Tourmaline now has an **opt-in CPU distance-field bake** for derived
`.geometry` families. Add `bake sdf on` in a `.tourrecipe` header,
before any `[pak]` sections. Default is off. Each imported family is
baked **after Cook** and **before Pak**, producing a sibling
`<path>.geometry.sdf`. The existing `.geometry`, meshlets, LODs, and
material/scene identity remain unchanged. One file contains volumes for
every nonempty base primitive, keyed by its stable mesh URI fragment.
Recipe builds include these bytes in the owning scene pak as
`PakAssetKind::Sdf = 20`. Consumers can read the raw sidecar via
`PakBackend::TryRead` and `ParseSdfAsset`. The optional static SDF
reflection mode below uploads the same encoded samples as GPU storage
buffers (no per-mesh 3D texture required).

## Representation

`TSDF` magic, little-endian version 1, mesh count; per mesh:
UTF-8 fragment byte count + bytes, cubic resolution, float3 voxel-zero
center in mesh-local coordinates, float voxel size, float saturation
distance, uint32 sign-validity flag, sample count, then tightly packed
little-endian signed 16-bit samples. Data order is X-fastest, then Y, Z.
Decode in mesh-local units as
`distance = sample / 32767.0 * maxDistance`. Magnitudes beyond the
narrow band are clamped. In v1, each primitive gets its own cubic grid
(default 24³, two padding cells each side, eight-cell band). This is a
**dense narrow-band representation**, not sparse bricks or a GPU
distance-field atlas.

The baker computes exact triangle distances (not distances to vertices)
and uses ray parity for sign only when welded undirected edges appear
exactly twice, with no degenerate triangles. Open / clearly nonmanifold
meshes return **unsigned** values and `signedDistance=false`; callers
must not use those values as negative interior. Closed-edge parity alone
does not prove an oriented, non-self-intersecting manifold, so signed
results on intersecting or non-orientable geometry are not guaranteed.
Coordinates are the primitive's local positions, not the .tour node's
world transform; transformed consumers must account for nonuniform
scales.

## Static SDF reflection ray queries (CPU reference)

`Engine/Asset/sdf_reflections.h` exposes `TraceStaticSdfRay` and
`TraceStaticSdfReflection`. Pass a fixed collection of
`StaticSdfInstance` entries pairing an already-parsed mesh-local
`SdfVolume` with its affine mesh-to-world transform. Only **static
scene geometry** belongs in this list: do not insert animated,
skinned, or dynamically moved nodes. The functions return the nearest
geometric hit (world-space distance, position and facing normal), or
a miss. Reflection rays take the receiver's incident direction and
world normal, reflect the direction and apply an origin bias to avoid
self-intersections. Misses leave the output unchanged.

The tracer intersects each volume's transformed grid bounds, samples
the existing X-fastest R16_SNORM distance values with trilinear
interpolation, and sphere-traces conservatively within the narrow band.
It handles signed and unsigned (open) surfaces and nonuniformly scaled
affine instances. It is intentionally a **CPU reference/query**:
there is no GPU sidecar upload, material/radiance hit evaluation,
integration with the SSSR/SSR composite, or animated geometry updates.
It does **not** make visible in-game reflections yet. A production
SDF reflection feature still needs static instance collection, GPU
residency/acceleration, shader tracing, and lighting/compositing.
The distance-field resolution and narrow-band saturation can miss thin
features; do not treat these queries as exact collision predicates.

## Experimental GPU static SDF reflections

Enable SSR and the **Static SDF off-screen reflections** checkbox in
the Rendering panel, or set `TOURMALINE_SSR=1` and
`TOURMALINE_SDF_REFLECTIONS=1` for a headless A/B run. Both the
scene's `.tourrecipe` `bake sdf on` and a recooked pak are required.
With no valid baked SDFs the renderer runs exactly its original SSR
path; the GPU SDF fallback is **off by default**. The system does not
require Vulkan hardware ray-query extensions.

The scene loader now retains `PakAssetKind::Sdf` sidecars as cluster
payloads; uncompressed spans borrow the pinned pak image, while
compressed payloads own their bytes. At runtime the optional
`StaticSdfGpuScene` walks **static, opaque, unmasked, unskinned**
draw items, maps the cooked mesh URI's family and fragment to a
`.geometry.sdf` volume, and builds immutable GPU buffers:

- One 96-byte descriptor per instance, containing inverse mesh-to-world
  rows, voxel origin/size, base-color factor/roughness and grid metadata
- One shared sample-word buffer: two signed R16 values packed in each
  uint32, deduplicated across instances of the same mesh URI
- A hard cap of 128 resident instances and 2,097,152 sample words
  (4,194,304 voxels); excess candidates are skipped (the nearest candidates to the camera
  are selected, republished after an 8 m camera move) so GPU memory and
  shader loop bounds are predictable

The existing compute SSR intersect first attempts its normal
hierarchical depth trace. For **misses and rejected screen hits**, the
shader intersects the packed static-instance grid AABBs and
conservatively marches trilinearly decoded SDF distances in mesh space
(with affine, nonuniform-scale-safe world-distance steps). The closest
SDF hit provides position, gradient normal and receiver confidence.
A hit visible in the current screen's depth buffer borrows the sharp
scene HDR only if the reconstructed depth position agrees with the SDF
hit. Off-screen SDF hits use an approximate base-color + sky/sun
irradiance fallback (no material texture UVs, dynamic lights or cast
shadows). They then use **the same receiver Fresnel/F0 tint,
roughness gate, firefly cap, bilateral blur, temporal history and HDR
composite** as the ordinary SSR hit. Turning the static mode on/off
invalidates reflection history.

Only authored frozen static scene nodes enter the scene buffer. The
path does NOT reflect animated/skinned/moving objects, transparent or
alpha-masked meshes, or procedural terrain. It also cannot reproduce
textured, shadowed off-screen shading with exact fidelity from the
current TSDF format alone. Low-resolution narrow-band volumes can miss
very thin features; high instance counts can cost significant compute
time because this initial shader uses a bounded linear AABB scan instead
of a spatial acceleration hierarchy. Frozen data refreshes on world
topology changes (or explicit `StaticSdfGpuScene::Invalidate`);
editing a supposedly frozen mesh transform requires republication.

### Validation

`SdfTests` covers SDF bake/parse, signed/unsigned CPU reference
reflection rays and the GPU R16-word packing contract. GPU validation
still needs a Vulkan/DXC build and on-screen/off-screen capture
comparisons with SSR on/off and the static fallback on/off. Test a
scene with `bake sdf on`, inspect `[StaticSdf] GPU resident` on
stderr, capture `TOURMALINE_SSR_DEBUG=1` contributions and compare
frame timings with the exact same camera and content. This code path
is experimental until those captures and perf baselines are reviewed.

## meshoptimizer 1.3

Tourmaline already pins a meshoptimizer revision with the experimental
`meshopt_remesh` API. It is a **voxel remesher**, not an SDF voxelizer:
the baker performs distance and sign computations independently.
`SdfBakeOptions::voxelRemesh` is opt-in through the C++ API only; it
may fuse nearby components, alter topology, and change sign behavior.
The recipe bake intentionally uses the unmodified imported mesh.

## Boundaries / follow-ups

- Bake is CPU-only, brute-force per grid voxel over all triangles:
  suitable for bounded/static props, not unlimited high-poly meshes.
  Add a BVH/triangle bins and job scheduling before production scale.
- No collision/occlusion integration, sparse
  brick compression, or automatic LOD selection in this first increment.
- Add shape, winding, seam, thin-feature, and asset-package regression
  tests and benchmark varied mesh complexity before enabling by default.
- C++ API: `Engine/Asset/sdf.h`; tests: `SdfTests`.
