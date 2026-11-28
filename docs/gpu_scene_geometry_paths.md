# GPU-scene geometry pipeline

This is the authoritative map of how scene geometry reaches the GPU: the
shared GPU cull chain, the four publish paths that turn culled work into draw
commands, how each path is selected, the limits, and the trade-offs. It
supersedes the ad-hoc path notes scattered across the renderer and the
candidate docs. See also `gpu_scene_pso_prefix_compaction.md` (the standalone
atomics-free prefix prototype) and `gpu_scene_barrier_hzb_optimization.md`
(the synchronization/HZB roadmap this branch implements).

## 1. At a glance

```
 CPU coarse selection (leaf frustum test, per leaf)
   -> GPU instance cull (per instance, frustum + Hi-Z)
   -> compact count/prefix/scatter (visible instances -> dense verdict stream)
   -> work select (persistent submesh/LOD resolution -> work records)
   -> work scan/scatter (work records -> dense work stream)
   -> [ ONE of the publish paths below ]
        A. direct          (legacy, no GPU-scene chain; CPU-issued draws)
        B. indexed 8-bin   (whole-submesh VkDrawIndexedIndirect)
        C. task/mesh       (meshlet task/mesh shader, VkDrawMeshTasksIndirect)  <- canonical
        D. meshlet compact  (dense candidates -> PSO prefix -> VkDrawIndexedIndirectCount)  <- superseded
   -> indirect args + verify (DispatchIndirect, debug only)
```

The GPU-scene raster cutover is a **live member** `m_gpuSceneRasterEnabled`,
initialized once from `TOURMALINE_GPU_SCENE_RASTER` (only `=0` disables it)
and flippable at runtime via the `GPU_SCENE_TOGGLE` keybind — so a scene can
A/B GPU-scene vs direct without a relaunch.

## 2. The shared cull chain

`RecordGpuSceneCullChain` (`WorldRenderer.cpp`) is the common prefix for every
GPU-scene publish path. Stage -> kernel -> outputs:

| # | kernel (shader) | output | dispatch | skipped when |
|---|---|---|---|---|
| 1 | `gpu_scene_instance_cull` | per-instance verdicts + optional fused group counts | `candidateCount/64` groups | — |
| 2 | `gpu_scene_compact_count` | per-group visible counts | `candidateCount/64` | `GpuSceneFusedCount()` (candidate B) |
| 3 | `gpu_scene_compact_prefix` | exclusive scan -> candidate offsets | 1 | — |
| 4 | `gpu_scene_compact_scatter` | dense compact verdict stream | `candidateCount/64` | — |
| 5 | `gpu_scene_work_select` | work records (persistent submesh/LOD) | `candidateCount/64` | — |
| 6a | `gpu_scene_work_prefix_block` | block sums | `workCapacity/256` | — |
| 6b | `gpu_scene_work_prefix_groups` | exclusive scan -> block bases | 1 | — |
| 6c | `gpu_scene_work_prefix_add` | offsets[i] += base | `workCapacity/256` | `GpuSceneFusedWorkAdd()` (candidate C) |
| 7 | `gpu_scene_work_scatter` | dense work stream (`work[i]`, `workOffsets`) | `workCapacity/256` | — |

Barriers between stages are `GpuSceneComputeDependency` — scoped compute
barriers under `TOURMALINE_GPU_SCENE_SCOPED_BARRIERS=1` (candidate A), else
global memory barriers.

The work stream `work[i]` / `workOffsets[i]` (identity + detail + exclusive
offset) is the contract every publish path consumes. `workOffsets[workCapacity]`
is the exact GPU live count.

## 3. Publish paths

### A. Direct (legacy)

The pre-GPU-scene path. CPU culls (gfx filter / `GfxDraw`) and issues indexed
draws directly; no GPU cull chain, no indirect buffers. Reached when
`m_gpuSceneRasterEnabled` is false, or when the GPU-scene cutover is not
eligible (see §4). Benchmark fingerprint: `main draw gpu (off) | cull gpu (off)`
and a large `execute` (the GPU does the rasterization the CPU already culled).

### B. GPU-scene indexed (8-bin whole-submesh)

The Vulkan-1.3 fallback that owns any mesh without cooked meshlet data (the
"native-suffix" subset), and the whole scene when neither task/mesh nor
meshlet-compact engages.

- Producer: `gpu_scene_raster_build` (`rasterProgram`, 1 dispatch,
  `ceil(commandCapacity/128)` groups) writes `VkDrawIndexedIndirectCommand`
  rows into `kRasterPipelineClassCount` **fixed-stride** bins.
- **8 bins**, not 4: `(doubleSided, masked, projectLit)` = `2*2*2` classes
  (`gpu_scene_raster.h`). Each bin is a contiguous `workCapacity`-sized range;
  `firstInstance` stays the original work slot, so the raster VS
  (`work[SV_InstanceID]`) and the temporal identity contract hold.
- Draw: `VkDrawIndexedIndirect` (not `*Count` — the bins are fixed-stride,
  the CPU knows the stride).

### C. GPU-scene task/mesh (canonical)

The default geometry path on mesh-shader hardware. It owns the meshlet subset;
the indexed path (B) still draws the native-suffix remainder via
`rasterPush.meshletBoundary`.

- Producers: `task_group_count` -> reused work scan (block/groups/add) ->
  `task_group_scatter` -> `task_dispatch`. Six dispatches total:
  one count per work row (`ceil(meshClusterCount/32)` task groups each),
  a 3-stage parallel scan, one scatter expanding task-group records, one
  single-thread dispatch that writes 4 identical `VkDrawMeshTasksIndirectCommand`
  (`{x, y, 1}`, x/y sized from `VkPhysicalDeviceMeshShaderPropertiesEXT`).
- `taskOffsets[workCapacity]` = exact task-group total; the task_dispatch
  kernel reads it and the indirect draw consumes it device-side (no CPU readback).
- Four raster classes (coverage/culling only: opaque / double-sided / masked /
  masked-double-sided). ProjectLit work **never** reaches task/mesh — admission
  disables the meshlet backend when any ProjectLit material is present.
- Engagement is all-or-indexed: if task/mesh is enabled but any cutover gate
  fails, the app **hard-aborts** (see §4), it does not silently fall back.

### D. GPU-scene meshlet compact (superseded)

A dense-candidate path: `meshlet_candidate_count/scatter` then the #301 PSO
prefix (`meshlet_pso_count` -> `pso_prefix_scan` -> `meshlet_pso_scatter`) into
4 per-class command bins consumed by `VkDrawIndexedIndirectCount` (the class
counts are read device-side from `classCounts`).

**This path is currently unreachable** — see §4. Its PSO-prefix machinery and
the `PrepareMeshletCompact` / `MeshletCompactPlan` scaffolding are retained in
the tree but are dead code since the task/mesh cutover became canonical.

## 4. Path selection and engagement

Order of preference once the shared chain has produced work
(`gpuSceneRasterMode && workReady`):

1. **task/mesh** if `taskReady` (all cutover gates + `Prepare` succeed).
2. **meshlet compact** if `!taskReady && meshletCompactReady`.
3. **indexed 8-bin** otherwise (`rasterReady && !meshletCompactReady`).

The `gpuSceneRasterMode` gate (`WorldRenderer.cpp`) itself requires the indexed
raster program, GI-variant readiness, no ShaderLOD, no wireframe, a supported
debug view, and whole-world source ownership.

### Why meshlet compact is unreachable

The task/mesh and meshlet-compact paths are coupled through **one** flag:
`GpuSceneCookedMeshletsWanted()` =
`SupportsMeshShaders() && (TOURMALINE_GPU_SCENE_TASK_MESH != 0)`. That flag
drives both:

- `cookedMeshletBackend` -> whether `RegisterWorldMeshes` runs -> whether
  `worstMeshlets > 0` **and** whether `m_gpuSceneTaskGeometryIdsCompatible`
  (`= !nativeOnly`) is true.
- `s_gpuSceneTaskMesh` -> whether the task/mesh path engages (`taskReady`).

The meshlet-compact gate needs `!taskReady && geoIdsCompatible && worstMeshlets>0`:

| `TOURMALINE_GPU_SCENE_TASK_MESH` | cooking / `worstMeshlets` / `geoIdsCompatible` | `taskReady` | compact gate |
|---|---|---|---|
| on (default) | registered -> `>0`, `true` | **true** (else the FATAL below) | `!taskReady` false -> skipped |
| `=0` | not registered -> `0`, `false` | false | `geoIdsCompatible` false -> skipped |

With task/mesh on, a failed `Prepare` is a **hard error**, not a fallback:

```
[GpuSceneTaskMesh] FATAL: task/mesh path enabled but cannot engage: <reason>
  (opt out with TOURMALINE_GPU_SCENE_TASK_MESH=0)
```

So there is no configuration in which `meshletCompactReady` is true: task/mesh
on -> `taskReady` true (or abort); task/mesh off -> no cooked-meshlet
registration, so `worstMeshlets == 0` and `geoIdsCompatible` false. The
meshlet-compact path is legacy scaffolding from before task/mesh became
canonical.

## 5. Candidate E — wave-aggregated atomic append (task/mesh)

Candidate E replaces the task/mesh `count -> scan -> scatter` chain (5
dispatches + 5 barriers) with a **counter clear + one atomic append**
(2 dispatches + 2 barriers):

```
task_group_clear   (zero taskOffsets[workCapacity])  1 dispatch
task_group_append  (per work row, wave-aggregated
                    InterlockedAdd reserve + record
                    write)                            ceil(workCapacity/256) groups
task_dispatch      (unchanged: reads taskOffsets[workCapacity])
```

- `gpu_scene_task_group_append.cs.hlsl`: each work row recomputes its task-group
  count with the **exact** predicate of the retired `task_group_count`, reserves
  its range with one `InterlockedAdd` per active wave on `taskOffsets[workCapacity]`,
  and writes the group records in place. The final counter word **is** the
  `DrawMeshTasksIndirect` total.
- `gpu_scene_task_group_clear.cs.hlsl`: zeroes that word behind an explicit
  compute dependency (the "reset counters with explicit dependencies"
  requirement).
- Opt-in via `TOURMALINE_GPU_SCENE_ATOMIC_APPEND=1`; default stays the
  atomics-free prefix. A missing append/clear kernel keeps the prefix.
- Parity (bistro): `task/mesh drawn tris 1507865` byte-identical prefix vs
  append across runs. No measurable execute win on the bistro — the retired
  chain is tiny (2909 work rows, 12 scan blocks, 6002 groups) next to the 1.5M-tri
  mesh draw — so E is opt-in until a large scene shows the trade-off.

## 6. Trade-offs

| Axis | Prefix (count/scan/scatter) | Atomic append (candidate E) |
|---|---|---|
| Dispatches/barriers | 5 dispatches + 5 intra-pass barriers | 2 dispatches + 2 barriers |
| Global atomics | none (block-local reductions) | one `InterlockedAdd` per active wave |
| Order determinism | fully deterministic (stable block scan) | non-deterministic across waves (stable within a row) |
| Memory traffic | block sums + block bases buffers | counter word only |
| Contention | none | single shared counter word |
| Best for | high work-row counts, deterministic replay | few dispatches, small/medium scenes |

| Axis | Direct | GPU-scene (any publish path) |
|---|---|---|
| Cull authority | CPU (gfx filter) | GPU (compute) |
| CPU load | high (per-draw record + submit) | low (coarse leaf selection only) |
| GPU load | draw-bound | cull + draw |
| Small-scene winner | direct (GPU-scene pays a fixed CPU orchestration tax) | — |
| Large-scene winner | — | GPU-scene (CPU cull is the bottleneck) |

The direct-vs-GPU-scene crossover is scene-size dependent: on the bistro the
direct path still wins end-to-end because the GPU-scene's CPU orchestration tax
outweighs its GPU cull savings; the candidates A/B/C/D/E all attack the **GPU**
side and do not by themselves move that crossover.

## 7. Limits

| Limit | Value | Where |
|---|---|---|
| Raster draw groups (meshes/geometry/aliases) | `kMaxRasterDrawGroups = 524288` | `gpu_scene_raster.h` |
| Raster pipeline classes (indexed) | `kRasterPipelineClassCount = 8` | `gpu_scene_raster.h` |
| PSO prefix classes (task/mesh + meshlet) | `kPsoPrefixClasses = 4` | `gpu_scene_pso_prefix.h` |
| Work rows | `GS_MAX_WORK = 1048576` | `gpu_scene_task_mesh.hlsli` |
| Dense candidate cap | `kMeshletCandidateLimit = 1048576` | `gpu_scene_meshlet_prefix.h` |
| Task-group worst-case bound | `MaxTaskGroups = 16777216` | `gpu_scene_task_mesh_runtime.h` |
| Clusters | `GS_MAX_CLUSTERS = 4194304` | `gpu_scene_task_mesh.hlsli` |
| Instances | `GS_MAX_INSTANCES = 4194304` | `gpu_scene_task_mesh.hlsli` |
| Geometry rows (task/mesh HLSL) | `GS_MAX_GEO = 524288` | `gpu_scene_task_mesh.hlsli` |
| Cluster index words (static monoblock) | `GS_MAX_INDEX_WORDS = 2^29` (2048 MiB) | `gpu_scene_task_mesh.hlsli` |
| Vertices | `GS_MAX_VERTICES = 44739242` | `gpu_scene_task_mesh.hlsli` |
| Clusters per task group | `GS_TASK_THREADS = 32` | `gpu_scene_task_mesh.hlsli` |
| Cluster meshlet triangles | `kMaxClusterTris = 128` (cook-time 96) | `gpu_scene_static.h` |

Note the known stale bound: `gpu_scene_task_group_count.cs.hlsl` still uses
`GS_MAX_MESHES = 65535` for its mesh-id guard while the scatter (and
`kMaxRasterDrawGroups`) allow `524288`. Rows with mesh id >= 65535 are therefore
silently undercounted. It does not affect the bistro (1314 meshes) but is a
latent cap to reconcile before meshes can exceed 65535.

## 8. Environment toggles

| Variable | Path | Default | Effect |
|---|---|---|---|
| `TOURMALINE_GPU_SCENE_RASTER` | cutover | on | `=0` disables the GPU-scene cutover (direct path) |
| `TOURMALINE_GPU_SCENE_TASK_MESH` | task/mesh | on | `=0` opts out of task/mesh **and** cooked-meshlet registration |
| `TOURMALINE_GPU_SCENE_MESHLETS` | meshlet compact | on | `=0` forces the indexed 8-bin path |
| `TOURMALINE_GPU_SCENE_ATOMIC_APPEND` | candidate E | off | `=1` uses clear+append instead of count/scan/scatter |
| `TOURMALINE_GPU_SCENE_SCOPED_BARRIERS` | candidate A | off | `=1` scoped compute barriers |
| `TOURMALINE_GPU_SCENE_FUSED_COUNT` | candidate B | off | `=1` skip compact-count |
| `TOURMALINE_GPU_SCENE_FUSED_WORK_ADD` | candidate C | off | `=1` skip prefix-add |
| `TOURMALINE_HIZ_SPD` | candidate D | off | `=1` single-dispatch SPD MIN pyramid (after verify) |
| `TOURMALINE_HIZ_SPD_VERIFY` | candidate D | off | `=1` compare SPD vs per-mip |

## 9. Validation gates

The merge criteria in `gpu_scene_barrier_hzb_optimization.md` apply: exact
integer/command parity for each opt-in candidate (verdicts, compact counts,
work offsets incl. sentinel, indirect command fields, PSO/class counts,
owner/generation), pixel identity where deterministic, and no missing geometry.
The standard deterministic parity pins are:

```
TOURMALINE_AO=off TOURMALINE_SSR=off TOURMALINE_SSGI=off TOURMALINE_DDGI=off
TOURMALINE_BLOOM_INTENSITY=0 TOURMALINE_VOLUMETRIC=off
TOURMALINE_GPU_SCENE_TEMPORAL=0 TOURMALINE_CONTACT_SHADOWS=off
TOURMALINE_GODRAYS=off
```
