# Incremental World-owned scene instances — PR #232

Stacked on PR #231. This introduces the World scene-instance ownership
path needed before replacing the legacy Cluster loader, without claiming the
legacy renderer/AssetStore has already been migrated.

## Contract

- There is **one runtime `tour::World`** and **one active authored Map ID**.
- A `tour::scene::Scene` is immutable authoring input. Each activation
  creates a synthetic instance root and descendants directly inside the
  one authoritative `World::scene` (no second mutable Scene mirror).
- Each runtime instance has a monotonic ID, a registered Region root, a
  source-node-slot to generational-World-NodeHandle mapping and per-instance
  asset leases. On retirement only those node slots are deleted.
- `AssetResidency` is keyed by durable content ID and stores `weak_ptr`
  records. Each `AssetLease` is a `shared_ptr` that pins independently
  loaded physical backing. Multiple scene instances share one backing;
  retirement of one does not release the others' resources.
- `WorldSceneResourceMap` is an explicit mapping from a local Scene's
  mesh/material/light/camera/decal source indices to **existing World
  resource indices**. Missing, stale or unresolved resources fail before
  world-node mutation; source indices are NEVER used as World indices.
- Renderable nodes with resolved mesh/material rows enter the existing
  draw-item material sort and flag caches incrementally. Retirement removes
  only the owned draw records. The topology revision increments, global
  transform journal runs for the new subtree, and static BVH caches become
  invalid without a full `ResolveAndFixup_clusters` call.
- Untyped hierarchy nodes and existing resource-mapped mesh/material rows
  are supported. Non-render runtime components (audio, physics, schemas,
  GI/cubemaps/volumes, animations) still fail closed until their independent
  lifetime/publication adapters are implemented.
- The legacy cluster merge/delete path explicitly refuses to run while
  a direct instance is active: failing loudly is safer than silently
  invalidating runtime handles. This is a temporary coexistence gate.

## Important remaining dependencies

The **World's actual mesh/material/light/camera/decal location entries
remain Cluster-backed**. The new asset lease ledger is independent of
Clusters, but production page-backed upload/registration into these World
location tables is NOT implemented by this PR.

The MapSceneStreaming bridge from #231 still invokes ProjectScenes and
the legacy Cluster deserializer. Replacing its loader requires a production
CookedScene reader yielding immutable Scene + resource dependency IDs and
the GPU upload/residency provider that produces WorldSceneResourceMap.
Do not switch the bridge over until those dependencies exist.

Static BVH cooking/consumption, eviction hysteresis, asynchronous IO,
GPU scene direct instance publication, lightmap/LOD parity, and removal of
retainedClusters/assetRefs are also open migration gates. No CPU/GPU perf
claim is made by this PR.

## Integrator verification

First configure and build the engine and test targets with the project's
supported CMake preset. Then:

```sh
ctest --test-dir <build-dir> -N
ctest --test-dir <build-dir> --output-on-failure -R "world_scene_instance_tests|world_map_tests|scene_tile_provider_tests|scene_table_tests|gpu_scene"
ctest --test-dir <build-dir> --output-on-failure
```

Run the CPU tests under ASan/UBSan if available. Verify:
1. Multiple instances of the same source Scene preserve independent
   generational World NodeHandles through unrelated retirement.
2. Source cycle/missing-parent/depth/NaN and incomplete resource remaps
   fail without publishing partial nodes.
3. Lease acquisition is all-or-nothing; a shared asset stays alive until
   the final independent lease drops.
4. Map switch is refused with active instances, succeeds after retirement.
5. Any attempt to legacy-resolve/unload a Cluster while direct instances
   exist fails **before** mutating old cluster ownership.
6. Material-sorted draw items from a direct instance are inserted and
   removed while unrelated World render items survive unchanged.
7. Resource mapping into the World must reference already-resident
   resources and remain valid until the instance's last asset lease drops.

**No tests were run by this author.** This is a reviewable intermediate
architecture epoch, not the finished Cluster-free streaming renderer.
