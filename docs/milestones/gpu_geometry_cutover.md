# Milestone: authoritative GPU-scene geometry ownership

**Status:** planned; none of the unchecked gates is claimed complete.
**Existing foundation:** #294/#295 persistent generation/LOD geometry identities, per-flight descriptor publication, alias mapping, and raster-command handoff.

## Work remaining
- [ ] Expose authoritative resident indexed/vertex draw-range metadata from GeometryStreamer with generation and residency revisions.
- [ ] Replace the GfxFilter cluster-ordered physical heap lookup, preserving fail-closed admission when metadata is missing or stale.
- [ ] Remove the transient work-ID alias only after all command producers and consumers use stable identities.
- [ ] Design and implement generation-aware descriptor-slot reuse after flight-fenced retirement; keep the existing 65,535-slot fail-closed cap until safe.
- [ ] Add tests for registration churn, residency transitions, generation mismatch and transactional publication failures.
- [ ] Perform Vulkan validation with >=2 frames in flight, eviction/re-registration, mixed LODs, alpha-masked foliage, fallback and retirement.

## Acceptance
No production GPU-scene consumer resolves draw geometry from GfxFilter registration IDs; fail-closed mixed legacy/GPU rendering remains correct. No stale GPU work can alias a reused slot. Attach native build, CTest, shader compile, Vulkan validation and image-parity evidence before marking complete.

## References
- [Active TODO](../../TODO.md)
- [Persistent geometry design](../gpu_scene_persistent_geometry.md)

This PR adds a milestone specification only, **not an implementation**.