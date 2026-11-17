# Shared tiled WorldSerialization: GI, terrain, and world resources

Status: **implementation contract / draft**. This document proposes one common
tiled WorldSerialization service for world resources, including the GI asset
cutover; adding it does **not** mean the service or runtime tiled-GI bridge exists.

## Decision: one tiled WorldSerialization service

**WorldSerialization is the shared serialization and persistent artifact
service** for world-owned spatial data, not a new GI-only packer. Terrain,
GI, scene placements, environment tiles, world fields, ecology, collision,
navigation, and later resource providers should use the same service for
identity, indexing, artifact lookup, validation, and transactional publication.

This is distinct from **WorldPartition**: WorldSerialization decides what
durable tile records exist and how to read/write them; WorldPartition decides
what to request and keep resident. The payload codec, bake algorithm, GPU
allocator, and physical dimensions belong to each resource provider.

There is no existing `WorldSerialization` service by that name in the
currently inspected code. The following are existing narrower precedents,
**not** a shared-service implementation:
- `WorldPartition` has signed 64-bit 2D world keys, negative-coordinate
  floor semantics, provider streaming interests and common budgets.
- `map_tile_pak.h` writes indexed, seekable Map tiles into SMP8 paks.
- `env_tile_pak.h` writes independently addressable environment subcell
  pages and a scene/world manifest.
- `WorldFieldAsset.h` writes indexed `.wfb` 3D bricks (signed XYZ + LOD)
  and exposes an immutable cooked-brick source.
- Terrain already has its own tile/blob serialization. The objective is
  **convergence**, not simultaneous replacement of all working codecs.

### Shared keys and records

Proposed *logical* API (illustrative, **not existing C++ API**):

```cpp
struct WorldResourceKey {
    WorldId world;             // world identity, not a scene or pak stem
    ResourceKind kind;         // Terrain, GI, Scene, Env, Field, ...
    AssetId resource;          // stable logical producer identity
};
struct WorldTileAddress {
    WorldResourceKey owner;
    uint32_t lod;
    int64_t x, z;              // signed, floor-addressed world tile
    std::optional<int64_t> y;  // 3D brick providers only
};
struct WorldTileRecord {
    WorldTileAddress address;
    ContentHash inputHash;
    ContentHash payloadHash;
    PayloadKind payloadKind;   // provider-specific codec/ABI
    BlobRef bytes;             // immutable, independently addressable
};
```

A single service indexes heterogeneous records. **A shared tile address
does not imply identical tile dimensions or physical storage**: terrain
heightfield pages are 2D, GI field bricks can be 3D and multi-LOD, scene
placements are lists, environment cells hold record indices. Subsystems
choose their grid interpretation through a registered resource descriptor
including origin, scale, coordinate scheme, and tile schema. Provider
identity must be included in every key so a terrain tile cannot collide
with a GI tile at the same world coordinates.

### Common service responsibilities

1. Register and resolve world resources, including stable world+resource
   identity and provider-specific schema descriptors.
2. Build/read sparse indices of independently addressable tile records.
   Infinite *logical* worlds require no dense world-size manifest.
3. Validate content hashes, payload integrity, codec/schema identity,
   dependencies, and tile-coordinate correctness; fail closed on corrupt
   or mismatched records.
4. Publish new resource generations transactionally. Reuse unchanged
   content-addressed tile payloads; keep old generations alive until readers
   finish, never publish a partial replacement index.
5. Package selected manifest/index closures and tile payloads through the
   existing Pak abstraction, **without running a GI baker, terrain builder,
   mesh importer, or other producer in Pak**.
6. Supply indexed reads of tile payloads from loose/editing storage or
   packaged paks, with the same keys and validation rules.

### Explicitly NOT the service's responsibilities

- Bake GI transport, generate terrain heightmaps, or serialize each
  provider's GPU-native payload by interpreting its content.
- Own cameras, interests, frame budgets, asynchronous residency policy, or
  GPU-fence retirement: those belong to WorldPartition and providers.
- Force one codec, atlas, cell dimension, coordinate cardinality or GPU
  resource type on heterogeneous world data.
- Treat a package as a world: one world may span paks; one pak may contain
  several worlds.

### Integration flow

```text
Editor / world authoring / source assets
                 |
     +-----------+------------+--------------+
     |           |            |              |
   GI baker   Terrain cook  Scene cook    Env / field cooks
     |           |            |              |
     +-----------+------------+--------------+
                 |
     WorldSerialization (tile keys, indices,
       dependency hashes, immutable blobs)
                 |
             Pak embedding
                 |
     WorldSerialization reader (VFS/Pak)
                 |
       WorldPartition request/admission
                 |
     Provider decode, upload and sampling
```

A service v1 should prove terrain + GI records share key/index/publish
mechanics, while retaining existing specialized payload layouts and reader
behavior. Map/environment migrations should be incremental adapters instead
of forced format changes. Add tests for cross-kind key separation,
negative coordinates, sparse missing tiles, deterministic index order,
atomic publication, content-hash invalidation, and packaged/loose parity.

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
GI-set member backed by the **shared WorldSerialization index and paged
payload service**, with GI-specific resource metadata.

The field should keep the WorldPartition/WorldFields addressing semantics
(floor division for negative coordinates, signed coordinates, stable origin,
LOD keys), but its lighting payload may require a GI-specific format. Confirm
the SG coefficient / occlusion / visibility footprint before choosing BC6H,
`.wfb`, 3D textures, or a packed buffer representation.

## Baker -> WorldSerialization -> Pak

```text
editor / CLI => same GiBaker implementation
                  |
                  +--> bounded volume bakes (per-volume incremental)
                  |
                  +--> world GI tile bakes (per-tile incremental; future bridge)
                  |
                  v
       WorldSerialization: immutable artifacts + fingerprints
                  |
                  v
              shared tile index
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

1. **Shared WorldSerialization service contract:** define heterogeneous
   resource keys, sparse tile indices, immutable blob references, producer
   adapters, transactional publication, and Pak/VFS reads; characterize
   existing terrain, Map, environment and field formats without changing
   their payload ABIs.
2. **Contract and characterization tests:** pin current hard scene bindings,
   legacy/catalog manifest behavior, Pak stale-hash rejection and editor/CLI
   parity. Specify a GI set containing bounded volumes **and** a world-field
   entry without silently dropping the latter.
3. **GI set source/product plumbing:** stable source identity, typed
   sub-artifact references, atomic index publication, Pak closure and
   fail-closed validation. Preserve existing .givolume scenes.
4. **World GI tile bake format:** explicit tile/LOD keys, transport encoding,
   per-tile dependency hashing, border/halo rules, generation-safe publication,
   and end-to-end baked-to-Pak readback. Reuse WorldFields only where the
   payload ABI actually fits.
5. **Runtime streaming bridge:** GI-specific resident sampling through
   WorldPartition interest and existing field cache patterns; compose local
   volume override and sky fallback.
6. **Dynamic updates:** assess direct-light change handling and incremental
   re-trace/dynamic GI after the static asset and streaming contracts work.

## Must-pass acceptance matrix

- Shared service round-trips independent terrain and GI records at the same
  world/tile coordinates without collision, duplicate producer logic, or a
  global tile-size assumption.
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
