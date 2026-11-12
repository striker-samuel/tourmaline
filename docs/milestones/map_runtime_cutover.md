# Milestone: production direct Map/World asset pipeline

**Status:** planned beyond already landed foundations.
**Landed foundations:** #231/#232 Map identities, SMP8 packaging, World-owned direct instances and leases; #272 default direct MapSceneStreaming adapter. Do **not** reintroduce the obsolete default ProjectScenes::LoadClusterFromPak bridge.

## Work remaining
- [ ] Provide host cooked-scene ResolveScene decoding and explicit component/resource remapping for shipped content types. Unsupported types must fail visibly.
- [ ] Publish independently pooled physical mesh/material/texture assets and GPU-resident bindings with shared ownership across tiles/paks.
- [ ] Introduce cancellable asynchronous IO/decode and measurable dependency, IO, upload-byte and memory budgets.
- [ ] Add velocity-driven prefetch and eviction hysteresis, preserving generations through Map switches and stale callbacks.
- [ ] Make activation/retirement transactional, and release borrowed pages and GPU bindings only after relevant flights retire.
- [ ] Cook/serialize/load deterministic multi-LOD static-mesh BVHs with conservative transformed cross-cell bounds and malformed-page handling.
- [ ] Cover non-render component lifetimes and GPU-scene/LOD/shadow deltas.
- [ ] Execute real cooked Map reload/corrupt-page/rapid movement fixtures and before-after performance captures on Vulkan.

## Acceptance
A packaged Map loads via the default direct path without requiring Cluster-backed scene decoding; corrupt or cancelled content cannot become visible; shared assets survive independent tile unload safely; recorded performance/quality fixture passes.

## References
- [Current TODO](../../TODO.md)
- [Map integrator](../map_streaming_integrator.md)
- [Streaming epochs](../world_tile_streaming_epochs.md)
- [Direct world instances](../incremental_world_scene_instances.md)

This draft is a planning milestone, not completed production integration.