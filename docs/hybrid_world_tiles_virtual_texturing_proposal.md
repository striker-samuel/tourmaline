# Proposal: hybrid world-tile streaming and virtual texturing

**Status:** Design proposal only. No runtime, cooker, asset format, or shader behavior changes in this PR.

## Decision proposed

Keep `WorldPartition` authoritative for **world-space content residency** and add **optional page-granular virtual texturing (VT)** as a separate physical texture residency backend for suitable assets. Do not replace world tiles with VT, make texture pages world-tile identities, or require VT for normal materials.

This proposal extends the existing principle in [world_partition.md](world_partition.md): resource providers determine physical storage; world partition owns spatial interest and the shared residency-accounting contract.

## Why these are different problems

| Concern | World-tile streaming | Texture mip-tail streaming | Virtual texturing |
| --- | --- | --- | --- |
| Unit of demand | World cell/subcell and provider resource | Whole texture at a contiguous resident mip tail | Individual texture mip page |
| World scene instances / mesh / collision | Yes, through providers | No | No |
| Fine-grained texture detail near the viewer | Indirectly through asset residency | Entire mip level of an image | Selected spatial regions and mip levels |
| Residency key | World-qualified tile and stream layer | Stable texture identity | Stable texture identity + mip + page coordinates |
| Main costs | Tile IO, dependency activation, scene publication | Retarget allocation, transcode and upload | Page table, page cache, feedback, borders and synchronization |

A world tile is a **logical content/ownership cell**, not necessarily one pak file. A texture page is a **physical sampling residency unit** and may cover material used by many world cells. Their identities and lifetimes must remain independent.

### Existing Tourmaline foundations and current limitations

- `Engine/WorldPartition/WorldPartition.h`: interest/lifecycle/provider seam. Default root cell 256 m; terrain-related subcells 64 m. Spatial residency is not the same as per-view visibility.
- `Engine/Renderer/texture_streamer.h`: feedback + CPU distance heuristics, budgeted upgrades/downgrades, contiguous mip-tail residency. Retargeting changes backing image while preserving logical texture handle/bindless slot. This **is not page-granular VT**.
- `Engine/WorldFields/WorldFieldAtlas.h`: persistent atlas with independently resident 3D brick slots. Useful model for physical cache ownership/publication, **not** a 2D texture VT implementation to reuse directly.
- `docs/world_tile_streaming_epochs.md` and `docs/map_scene_world_architecture.md`: production Map bridge still has synchronous/Cluster-based integration and lacks the full async asset/dependency, incremental GPU-scene and safe retirement cutover. Those gates take priority over VT.
- `docs/terrain_world_semantics.md` is a **proposal**, not an implemented world-level terrain data model. This VT proposal does not presume that migration has occurred.
- `docs/terrain_legacy_migration_contract.md` requires preserving existing `.tour` terrain creation/paint references. VT must not silently change those serialized semantics.

## Proposed ownership boundaries

```text
Camera / gameplay interests / optional prefetch
                 |
           WorldPartition
       spatial tile demand + budgets
                 |
       +---------+---------------+-------------------+
       |                         |                   |
  Scene / geometry          Terrain provider     World fields
  instances, assets         height / topology    3D brick atlas
       |                         |
       +-------------------------+
                 |
         Texture asset demand
                 |
       Texture residency policy
        +--------+---------+
        |                  |
   Existing mip-tail   Optional VT backend
   normal materials    huge unique texture data
                       page cache + page table
                       feedback + coarse fallback
```

1. **WorldPartition** supplies coarse camera/gameplay interest, lookahead and budget pressure. It does **not** track every texel page as a `WorldTileKey`.
2. **Texture asset ownership** remains independent of tile lifetime; two resident tiles referencing the same material share texture residency. Unloading one tile must not invalidate another tile's material.
3. **Texture policy** chooses mip-tail or VT per cooked texture asset (or a documented class/profile); preserve existing ordinary material handling and stable bindless references.
4. **VT backend** manages its own virtual page IDs, page-table mapping, cache slot generations, GPU feedback and fine-grained eviction. It can integrate its IO/upload accounting with shared budget infrastructure without using world-cell IDs as VT IDs.
5. **Terrain provider** retains heightfield/geometry/paint authority and may reference virtualized material/paint textures. VT is **not** a replacement for heightfield streaming, collision or terrain edit storage.
6. **Published GPU state** is frame-safe: no page-table entry may reference an unuploaded/reused slot; eviction cannot reuse physical storage while in-flight graphics submissions can sample it.

## Recommended first adopters

Use VT selectively for very large **uniquely addressed** 2D data, such as world paint/mask layers or large non-repeating terrain imagery, where visible high-resolution regions are a small fraction of the entire asset. Continue mip-tail streaming for ordinary tiling/shared PBR material textures, UI and assets small enough that page metadata/feedback overhead would dominate.

Do not introduce VT solely because a texture is used by terrain. Verify there is overfetch and a meaningful VRAM or streaming improvement first.

## Proposed design, not yet API

### Asset and cook contract

- Introduce an explicitly versioned optional cooked VT representation: texture dimensions, format, page interior size, border size, mip levels, page offsets/checksums, always-available coarse tail, and stable texture/content identity.
- Keep the existing non-VT KTX2/pak path valid. Reuse positional SMP8 IO where appropriate but do not assume one archive page equals one GPU texture page or one world tile.
- Offline-build independently addressable, block-compression-aligned pages with sampling gutters/borders. Document address modes, mip filtering, anisotropy behavior, seam correctness and editable paint invalidation.
- The initial implementation should target 2D read-only sampled textures, not render targets, arbitrary 3D textures, or editable GPU writes. Editing may recook/invalidate affected pages and neighbors in a later phase.

### GPU and runtime

- Start with a **software-managed physical 2D atlas + GPU page table** for portability across Vulkan 1.3 devices, rather than making sparse-image residency a prerequisite. Assess sparse residency as a separate later backend.
- Use explicit `(texture ID, mip, page X, page Y)` keys and generation-tagged physical slots; page-table lookup must support fallback to a resident parent/coarse tail when a desired page is missing.
- Request pages from GPU screen-space feedback, with CPU coarse interest/lookahead for prewarming and teleports. Deduplicate requests, cap feedback buffers, prioritize visible high-impact requests, and avoid making current-frustum visibility the sole residency source.
- Stage IO/decompression and GPU uploads asynchronously within independently measured IO, CPU, upload and VRAM budgets; publish mappings only after upload completion and required synchronization.
- Prevent stale completion after world switch, texture destruction, edit/recook or page eviction via generation/ticket checks. Delay slot reuse and mapping retirement until every in-flight sampling frame has passed.
- Track page-table storage, border overhead, upload volume and cache fragmentation as real costs, not just texel payload bytes.

### Interface direction

Keep the current `TextureStreamer` contract intact for mip-tail assets while exploring a `VirtualTextureManager` or an explicit backend interface. Avoid prematurely modifying existing `TextureHandle` or material GPU layouts: test how virtual and conventional sampling can coexist, including shader permutations, stable descriptors and material ID lookup. Define the public interface only after a vertical slice demonstrates the necessary shader/RHI constraints.

## Milestones and gates

1. **Baseline first:** finish the world tile async dependency/publication and GPU-safe retirement gates described in `world_tile_streaming_epochs.md`. Instrument texture VRAM, uploads, visible texel demand and latency on representative open-world scenes.
2. **Offline proof:** cook a single large unique 2D texture into page records plus a pinned coarse tail; validate mip borders, compression layout, page metadata and deterministic recook.
3. **Minimal renderer slice:** atlas, page table, shader sample fallback, feedback readback, budgeted uploads and fence-safe slot reuse on a fixed test scene. Keep the existing mip-tail path unchanged.
4. **Terrain integration:** optional VT-backed terrain paint/mask material sampling; preserve heightfield tiles, legacy terrain scene references and missing-tile fallback semantics.
5. **Production evaluation:** benchmark alongside mip-tail streaming on camera pans, high-speed travel, teleport, camera spin, zoom transitions, scene unload/reload and mixed repeated/unique materials. Only expand default use when metrics justify it.

## Acceptance criteria for a future implementation PR

- **Correctness:** no invalid reads or stale page-table mappings under rapid reuse, in-flight frames, canceled requests, world switches or cache exhaustion. Parent/coarse mip fallback is always valid; seams and anisotropic filtering remain acceptable.
- **Ownership:** unloading a world tile cannot evict a texture page still needed by another resident tile; shared materials never duplicate physical pages unnecessarily.
- **Compatibility:** normal non-VT KTX2, cooked paks, non-tiled scenes and legacy terrain/paint serialization remain functional without migration.
- **Budget behavior:** bounded cache VRAM, bounded staging/feedback/readback allocations, capped per-frame uploads and request queues; well-defined graceful behavior under IO errors and OOM.
- **Quality:** no persistent missing-page checkerboards/pink sampling in supported cases; measure high-frequency shimmer, mip transitions, gutter/seam leakage and teleport recovery.
- **Benchmarking:** publish A/B GPU memory (including page table/borders), CPU ms, GPU ms, IO bytes, upload bytes/frame, feedback latency, page hit/miss and time-to-sharp-detail for (a) repeated tiling PBR materials, (b) huge uniquely painted terrain, and (c) mixed scenes. Compare equal image quality and frame budgets; no unqualified claim that VT universally saves memory or improves speed.

## Non-goals

- Replacing `WorldPartition`, `WorldTileKey`, the Map/Scene/World ownership model, terrain heightfield streaming, or WorldFields' 3D brick atlas.
- Moving gameplay entities or collision/nav into a texture pager.
- Forcing VT on all textures or making Vulkan sparse binding mandatory.
- Changing `.tour` legacy terrain paint semantics in this proposal PR.
- Implementing runtime VT, an asset migration, or changing benchmark results as part of this documentation-only change.

## Open decisions for implementation design

- What measured asset-size/visibility threshold enables VT versus mip-tail?
- Which offline page interior/border sizes provide acceptable BC formats, anisotropy and IO efficiency?
- Should page cache pools be per format, per texture class, or shared where formats permit?
- How are shader sampling and bindless texture identities represented without breaking current materials?
- How is per-asset minimum coarse-tail residency charged to budgets during world/asset eviction?
- What is the correct invalidation contract for terrain brush edits spanning tile and mip-page seams?

**Recommendation:** proceed with this architecture as an opt-in experiment after baseline profiling and the existing world-tile async lifecycle work, not as an immediate replacement of the current streamer.
