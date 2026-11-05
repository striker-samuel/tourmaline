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
