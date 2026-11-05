# Arena-backed renderer arrays (first migration)

This PR replaces the renderer's previous-frame transform cache
(`WorldRenderer::m_prevGlobalTransforms`) with an arena-backed contiguous
array using the project's existing `smvk::Arena`. It is deliberately
independent of the semantic scene-memory descriptor PR.

## Why this first

The cache is a renderer-owned snapshot of `World::scene.globalTransform`.
Its lifetime is the renderer lifetime; it can be copied each frame without
owning scene nodes or participating in scene mutation journals. It is a safe
place to exercise replacing `std::vector<T>` without changing core scene
data structures or asset serialization.

## Properties

- Trivially-copyable records only, contiguous and pointer-addressable.
- `Assign(std::span<const T>)` reuses capacity when possible.
- When larger input arrives, `smvk::Arena` bump-allocates a new region;
  old regions are reclaimed only when the owning array/arena is destroyed.
- Not a general `std::vector` replacement: no erase, insert, per-element
  destruction, or implicit automatic growth from push_back.
- No externally owned storage in this PR. Its exported `data()` is only
  valid until the next growth/renderer destruction.
- No fixed arbitrary scene-node limit: allocation grows to the observed
  node count. High-water memory consumption is the tradeoff of bump arenas.

## Test

`arena_flat_array_tests` checks empty, assignment, reuse after shrink,
growth, and clear. Full project build and CI are not yet verified.

## Follow-up

If this ownership shape works, a separate migration can move the
authoritative scene transforms/hierarchy behind a fixed-capacity or
arena-backed container, with explicit mutation/resize policies and
serialization compatibility. Those vectors should not be changed
implicitly in this PR.

## Additional renderer arrays migrated

- Previous-frame global transforms.
- Deferred-shadow and Forward+ z-bin / tile-mask staging (CPU builder
  outputs now accept either vectors or arena-backed arrays).
- Async shadow-statistics readback, GPU demand and filtering-audit snapshots.
- Debug-line capacities and revisions.
- Probe upload rows and local shadow-tile snapshots.

These arrays are **flat snapshots/staging**, not authoritative scene objects.
The implementation reuses existing `smvk::Arena` with small 4 KiB
initial block sizing for each array. Growth can retain prior blocks until
destruction; that is intentional for the bump allocator but should be
measured with scene-size churn.

## Remaining work required for full World/Scene replacement

`Scene::hierarchy`, `Scene::localTransform`, and
`Scene::globalTransform` are still `std::vector` in this PR.
They are consumed by authored-scene writers, merging, serializers and GPU
publication. A wholesale type substitution without migrating those
interfaces would break the renderer. `WorldRenderer` also retains vectors
for nontrivial or externally passed types (strings, futures, resource
holders, and parameter vectors), and pass-subsystem internals are not
converted.

A full conversion needs a distinct growable arena-vector implementation
with element construction/destruction and move/copy semantics, call-site
migration, lifetime tests, and end-to-end rendering CI. Do not use this
partial migration as evidence that scene vectors can be rebound externally.
