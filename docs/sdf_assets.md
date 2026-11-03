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
`PakBackend::TryRead` and `ParseSdfAsset`; it is not yet bound to a
GPU texture or renderer pass.

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
- No GPU upload, sampling API, collision/occlusion integration, sparse
  brick compression, or automatic LOD selection in this first increment.
- Add shape, winding, seam, thin-feature, and asset-package regression
  tests and benchmark varied mesh complexity before enabling by default.
- C++ API: `Engine/Asset/sdf.h`; tests: `SdfTests`.
