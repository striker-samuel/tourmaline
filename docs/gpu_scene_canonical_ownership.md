# Phase 2: canonical static-object raster ownership (draft)

This is an incremental step after merged PR #275, **not** a completed mixed
GPU/CPU rendering cutover. The live renderer still uses #275's whole-scene
eligibility and `TOURMALINE_GPU_SCENE_RASTER=1` opt-in.

## Canonical identity

A static object is identified by the source cluster's registration, local
node slot and node generation (`gpu_scene::SourceKey`). Neither a transient
`World::drawItems` array index nor a merged scene node ID may be used as an
owner key. `CanonicalRasterOwnership` is a fail-closed CPU contract:

- The caller publishes a complete, sorted source snapshot in durable storage
  (AppArena is the intended owner of the underlying span).
- Duplicate/invalid source keys make the entire publication invalid.
- Per-source compatibility is *admission*, not proof of rendering ownership.
- Each frame begins legacy-owned. Only a successful GPU preparation for the
  required graphics passes may commit eligible entries to GPU ownership.
- Unknown/stale generation keys, incompatible entries, invalid publication or
  GPU preparation failure remain legacy-owned.

The table is a non-owning view: callers **must** keep its storage alive until
the next publication/reset and must never mutate entries while queried.

## Why this PR cannot yet activate mixed draw submission

#275's GPU culling/work-expansion stream currently processes the entire
GPU-scene database, while GfxFilter and the Forward+ prepass are suppressed
at the *whole-scene* boundary. Merely omitting CPU direct draws for entries
in an ownership table would duplicate or drop renderables: both the GPU
candidate stream and legacy filtered ranges must be masked by the SAME
immutable per-flight ownership snapshot.

The remaining integration must land atomically, with:

1. Source-key-derived admission tags in persistent GPU instance rows,
   including stable generation and dirty-journal publication.
2. A GPU cull/work filter that emits work **only** for admitted and
   GPU-owned source instances. Old flight snapshots remain immutable.
3. A legacy filter/batch builder and direct-draw predicate that emit only
   legacy-owned sources, including fallback-only and occluder depth paths.
4. A prepass/G-buffer layout that composes both sets without duplicate
   clears; material incompatibility stays in legacy.
5. A transactional frame gate: if either GPU preparation or required
   pass pipeline fails, reclaim **all** objects for legacy before any draw.
6. Renderer-level mixed-scene regression and Vulkan visual parity tests.

**Do not remove the whole-world guard or enable mixed mode using this
contract alone.** Default and opt-in behavior are unchanged until all six
conditions are implemented and verified.
