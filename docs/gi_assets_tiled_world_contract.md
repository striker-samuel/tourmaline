# GI assets: bounded volumes and infinite tiled world

Status: **implementation contract / draft**. This document proposes the GI asset
cutover; adding it does **not** mean the runtime tiled-GI bridge exists.

## Non-negotiable scope

GI is not only a list of finite convex volumes. Two domains must coexist:

1. **Local authored GI volumes**: `.givolume` directional SG probe grids, with
   the existing scene-owned `GiAssetCmd` / `SceneGiVolume::bakeSource`
   bindings and per-volume `GiVolumeBakeUnitHash`.
2. **Infinite logical world GI volume**: a sparse, independently resident 3D
   tiled field, keyed by signed coordinates and LOD. Never allocate a
   world-sized 3D texture, enumerate all possible tiles, or infer completeness
   from a finite scene bounding box. Missing tiles must have a defined
   coarse/sky fallback. The logical field is unbounded; the *cooked dataset*
   may of course be sparse and finite at any point in time.

The current `WorldFields` infrastructure provides virtual addressing,
clipmap/page-table residency, physical brick atlases, and the indexed `.wfb`
brick asset reader. This is an **infrastructure precedent**, not evidence
that `GiBaker` currently produces a world-GI field or that the GI shader
consumes `.wfb` as directional SG transport. Such a bridge needs an explicit
data-layout, compression, sampling, and runtime integration design.

## Current contracts and mismatch

- `tools/gi_baker/main.cpp` already bakes authored convex probe volumes and
  skips an unchanged, present bound artifact using its unit input hash.
- `engine/include/Engine/Asset/tour.h` stores hard per-volume bake bindings;
  Pak validates these and packages bound results.
- `engine/include/Engine/Asset/cook_asset.h` defines `.gicook` settings and
  `.givolume` typed assets. Its comments describe filling
  `blobGuid/blobPath/bakedHash` as follow-up integration, not a proven
  complete bake-owner workflow.
- `app/src/AssetManager.cpp::LoadGI` still expects a catalog
  `.givolumemanifest` containing volume paths, while the convex-bake path
  comments in `tools/gi_baker/main.cpp` declare that loose manifest retired
  in favor of hard per-volume scene bindings. Resolve this mismatch before
  relying on `LoadGI` for freshly baked scene assets.
- The old `.sggi` octree world-field path is explicitly retired. This
  proposal **does not revive** the octree or reinterpret `.wfb` as an
  already-shipping SG-GI product.

## Ownership: authored GI set versus cooked payloads

An authored GI **set** owns configuration and references; cooked payloads are
immutable, independently addressable artifacts. A set can include a bounded
volume collection, a tiled-world field reference, or both. A project may own
multiple sets for day/night/weather, with the existing
`AssetManager::SetGIBlend` providing set transitions where supported.

Proposed logical contract (not serialized syntax):

```text
GiSet (stable asset identity, scene/world reference, bake settings)
  boundedVolumes[]:
    source-volume identity -> cooked .givolume id/path + input fingerprint
  worldField (optional):
    stable world id + field id
    lattice origin, voxel scale, LODs, encoding/fallback
    index of cooked tile records (signed XYZ + LOD -> content hash + payload)
  setFingerprint: dependency/settings/schema identity
```

**Do not** put an unbounded world's tiles into `GiAssetCmd` as an
ever-growing linear list; that command is appropriate for authored
per-volume bindings, not global tile residency. The world field is a distinct
GI-set member backed by an index and paged tile payloads.

The field should keep the WorldPartition/WorldFields addressing semantics
(floor division for negative coordinates, signed coordinates, stable origin,
LOD keys), but its lighting payload may require a GI-specific format. Confirm
the SG coefficient / occlusion / visibility footprint before choosing BC6H,
`.wfb`, 3D textures, or a packed buffer representation.

## Baker -> artifact store -> Pak

```text
editor / CLI => same GiBaker implementation
                  |
                  +--> bounded volume bakes (per-volume incremental)
                  |
                  +--> world GI tile bakes (per-tile incremental; future bridge)
                  |
                  v
             immutable cooked artifacts + fingerprints
                  |
                  v
               GI set index
                  |
                  v
           Pak validates and copies artifacts
                  |
                  v
       runtime GI set loader + resident tile provider
```

- The editor must invoke the existing linked `gibaker::Run` path; do not
  duplicate transport algorithms in the editor.
- GI baking must **not** happen inside Pak. Pak resolves selected sets,
  checks availability, schema compatibility, and fingerprints, and embeds
  their referenced bytes/indexes. Reject stale/missing required artifacts.
- Publish a GI set/index only **after** all required new records are
  materialized and validated. Keep previous live generation until the new
  generation is safe to adopt.
- Prefer content-based invalidation over mtimes. Hash authored lighting,
  relevant geometry and material dependencies, GI algorithm/configuration
  parameters, coordinate grid definition, and the payload ABI.
- For tiled GI, the unit is **tile + LOD**, with neighboring halo/lighting
  influence dependencies explicitly modeled. Never reuse a tile just because
  its interior geometry is unchanged when off-tile lights or bounces can
  alter its radiance.
- One GI set can be packaged into one or more paks; the asset identity and
  tile index cannot depend on the output pak filename.

## Runtime composition

1. Resolve the world-GI field (coarse/missing-tile fallback first).
2. Sample resident field tiles with stable seam/LOD interpolation.
3. Composite local authored convex GI volumes by existing priority/ownership
   rules. The field is the base layer, not a replacement for interiors.
4. Retain sky fallback when neither field nor local volume covers a sample.
5. Keep dynamic lighting/indirect updates a **separate** feature from
   assetizing and packaging static GI.

The infinite field must use bounded CPU/GPU residency and generation-tagged
tile completion. Fast travel, negative coordinates, world swaps, cancellation,
and eviction cannot publish stale page-table mappings or reuse an in-flight
atlas slot. Fallback must remain valid during all of these transitions.

## Implementation sequence

1. **Contract and characterization tests:** pin current hard scene bindings,
   legacy/catalog manifest behavior, Pak stale-hash rejection and editor/CLI
   parity. Specify a GI set containing bounded volumes **and** a world-field
   entry without silently dropping the latter.
2. **GI set source/product plumbing:** stable source identity, typed
   sub-artifact references, atomic index publication, Pak closure and
   fail-closed validation. Preserve existing .givolume scenes.
3. **World GI tile bake format:** explicit tile/LOD keys, transport encoding,
   per-tile dependency hashing, border/halo rules, generation-safe publication,
   and end-to-end baked-to-Pak readback. Reuse WorldFields only where the
   payload ABI actually fits.
4. **Runtime streaming bridge:** GI-specific resident sampling through
   WorldPartition interest and existing field cache patterns; compose local
   volume override and sky fallback.
5. **Dynamic updates:** assess direct-light change handling and incremental
   re-trace/dynamic GI after the static asset and streaming contracts work.

## Must-pass acceptance matrix

- Bounded-only scene preserves its current `.givolume` output, bindings,
  and stale-bake failure behavior.
- World-only GI set works without requiring a dummy convex volume.
- Mixed world + local volumes preserve local override priority.
- Signed tile coordinates around zero and negative world positions address
  the correct brick; seam/LOD crossings stay continuous.
- Rebuild of unchanged tiles and volumes does not retrace; changing a
  contributing light/material/geometry/config invalidates every affected unit.
- Missing, corrupt, incompatible, or stale tile records never masquerade as
  valid baked data; fallback and diagnostics are explicit.
- Camera teleport, high-speed motion, world switch, cancel, and repeated
  eviction/reload never expose unpublished tiles or stale residency.
- Editor and CLI call the same baker and produce byte-equivalent packaged
  outputs for equivalent authored inputs.
- Pak performs **no** GI trace and no GI derivation; its work is
  validation/serialization/embed only.
- Real-device and representative-world benchmarks show bounded residency,
  tile IO, bake skip counts, and frame behavior. No performance claim before
  these measurements.

## Explicit non-goals of this contract PR

This PR contains **documentation only**. It does not add a GI-specific world
tile baker, expand `AssetManager::LoadGI`, alter binary formats, or implement
dynamic global illumination. Those require separately testable code changes.
