# GPU Scene Update System — Implementation & Operations

This is the consolidated reference for Tourmaline's **GPU-driven scene update
pipeline** — the fused multi-view visibility cutover and the memory-bounded
world-partition streaming that feeds it. It documents what is *implemented and
GPU-validated*, not the design goals (see `gpuir_and_gpu_scene.md` and
`gpu_scene_update_design_distillation.md` for the north-star design, and
`gpu_scene_visibility_hierarchy_proposal.md` for the RFC #372 rationale).

---

## 1. Overview

The system has three layers:

1. **Flat leaf visibility** — the CPU publishes a *dense* leaf view of the
   scene database; the GPU classifies every leaf against a set of view
   frustums and emits a fused per-instance verdict stream.
2. **Fused work expansion** — the fused verdict stream drives the existing
   compact → work-select → work-scan → raster chain, replacing the per-view
   CPU coarse selector and cull.
3. **World streaming** — a camera-driven page streamer keeps a
   *memory-bounded* resident set of tiles; geometry residency is gated on the
   static vertex arena, not just a mesh count.

The production draws (Main + shadow cascades) are owned by the fused path
behind `TOURMALINE_GPU_SCENE_VISIBILITY_PROD=1`; the CPU path remains the
fallback and the reference oracle for validation.

---

## 2. Fused multi-view visibility

### Pipeline stages (dispatch order)

| Stage | Shader | Input | Output |
|---|---|---|---|
| classify | `gpu_scene_leaf_classify.cs.hlsl` | dense leaves + packed views | per-leaf view mask |
| compact | `gpu_scene_leaf_compact.cs.hlsl` | leaf masks + counters | active-leaf work queue |
| fused cull | `gpu_scene_fused_cull.cs.hlsl` | work queue + leaf members + instances | fused verdicts |
| expand | `gpu_scene_fused_expand.cs.hlsl` | fused verdicts (view bit + shadow filter) | dense instance-cull verdicts |
| compact | `gpu_scene_compact_{count,prefix,scatter}` | dense verdicts | compacted visible set |
| work select | `gpu_scene_work_select.cs.hlsl` | compacted set + meshes/lods/submeshes | work choices + counts |
| work scan | `gpu_scene_work_prefix_{block,groups,add}` | work counts | parallel prefix (offsets) |
| work scatter | `gpu_scene_work_scatter.cs.hlsl` | choices + offsets | packed `SceneSubmeshWork` |
| raster | `gpu_scene_raster_work_drawbuild.cs.hlsl` | work + indexed geometry | indexed indirect draws |

### Fused verdict stream

One `GpuFusedVerdict` per visible instance: `x = slot, y = generation,
z = per-view mask (bit v = visible in view v), w = flags (bit 0 = CastShadow)`.
This replaces the five independent per-cascade cull passes that read each
instance five times (the "5x verdict-write amplification" the RFC calls out).

### Correctness contracts

- **Fail-open on incomplete bounds**: an instance/mesh with unknown or
  non-finite bounds is treated as visible, never culled (`gpu_scene_fused_cull`
  and the CPU `ReferenceInstanceVisible` mirror each other).
- **Ownership**: only an exact generation match (owner mask + instance
  generation) may emit work — a recycled or legacy-owned slot emits nothing.
- **Fused cull reads each leaf's instances once** and validates generation +
  ownership + CastShadow once, folding per-view visibility into the verdict.

---

## 3. Production cutover

### Main draw

The fused verdict stream drives expand → compact → work → scan → raster, and
the resulting indirect commands become the Main draw source
(`GpuSceneVisibilityProd` pass). The CPU raster path is skipped when the fused
path owns it. The flip is guarded by:

- `PrepareFusedVerdicts` **fails closed** when `instanceCount > 1,048,576`
  (`MaxPassedInstanceCandidates`), so an oversubscribed scene falls back to
  the CPU path instead of overflowing the fused buffers.
- The canonical owner mask (`CanonicalRasterOwnership::BuildGenerationMask`)
  admits only static-opaque-compatible sources; legacy (skinned/layered/
  masked/transparent) sources stay on the legacy path.

### Shadow cascades

Each cascade's expand reads the *same* fused verdicts with its own view bit and
a `CastShadow` filter, then runs its own expand → compact → work → scan →
raster chain (`WorldRendererGpuSceneCascades.inl`). The **three-stage parallel
scan** (`PrepareParallelWorkPrefix`) is required — the single work-prefix pass
caused a `VK_ERROR_DEVICE_LOST` under shadow cascades.

### Owner mask

`docs/gpu_scene_canonical_ownership.md` describes the per-source ownership
table. The fused path uploads the same per-slot generation mask the CPU path
uses; a recycled or legacy-owned slot emits no GPU work.

---

## 4. World streaming (partition → GPU scene)

`ScenePageStreamer` (see `scene_page_streamer.h`) is a camera-driven page
streamer over a partitioned-family world:

- **Loads** pages whose world AABB is within the streaming radius of the
  camera, and **unloads** pages beyond it (the entry scene is pinned).
- Deserialization runs on dedicated **bin threads** (one pool shard each);
  the render thread only folds completed pages.
- Admission is bounded by **two budgets**:
  - `maxLoadedMeshes_` — the mesh-count ceiling (`kMaxMeshes`), so streaming
    never oversubscribes the GPU-driven mesh buffer.
  - `maxLoadedVertices_` — the **resident geometry budget in vertices**
    (configurable, default 85% of the static vertex arena).

### Heap-capacity admission

The static **vertex** arena (not the mesh count) is the binding budget for
dense partitioned worlds: the island's ~162k valid meshes fit under
`kMaxMeshes`, but their vertices overflow the arena. The admission gate:

```
used + InFlightVertexEstimate() >= GeometryBudget()
```

- `GeometryBudget()` = the configured budget, or 85% of the arena capacity.
- `InFlightVertexEstimate()` = the vertex cost of pages still queued/being
  deserialized, estimated as `(dispatched mesh count) × (running per-mesh
  vertex average observed from the arena)`. This reserves the **async carve
  burst** — the bins carve ahead of the render-thread gate, so gating on
  `Used()` alone would let the burst overflow the arena before the next tick.

When the gate trips, admission holds until far pages evict (eviction runs
first each tick and frees their arena ranges via deferred retirement). Pages
that were already in flight and still overflow fail **gracefully** — the
deserialize returns null and the page is skipped, never aborting the world.

---

## 5. Degenerate mesh tolerance

The importer can remove every face of a primitive, leaving a zero-vertex
geometry blob (e.g. `map_docks_part0.geometry#mesh.0/prim.0`). Previously this
failed the whole cluster load. Now:

- The deserializer passes a zero-vertex mesh through (only *insane* extents
  stay fatal).
- `UploadStaticGeometry` emits a **valid empty mesh** (no carved ranges, no
  surface) for a zero-vertex source instead of rolling the batch back.

A degenerate primitive therefore produces an empty handle that draws nothing,
rather than failing the load.

---

## 6. Configurable memory limits

Geometry residency is configurable through environment variables:

| Variable | Unit | Meaning |
|---|---|---|
| `TOURMALINE_MEM_STATIC_VERTEX_MB` | MiB | static vertex arena allocation (pos + shading streams). Binding constraint: 16 B/vertex (shading). |
| `TOURMALINE_MEM_STATIC_INDEX_MB` | MiB | static index arena allocation (4 B/index). |
| `TOURMALINE_MEM_STATIC_LIGHTMAP_MB` | MiB | optional lightmap stream arena (deprecated; usually 0). |
| `TOURMALINE_GEOMETRY_BUDGET_MB` | MiB | **resident geometry budget** (vertices) for the streamer; `0` = auto (85% of the vertex arena). Converted `MiB × 1024² / 16` → vertices. |
| `TOURMALINE_MAP_STREAMING_RADIUS` | metres | page-streamer interest radius (default 512). |
| `TOURMALINE_COARSE_ONLY` | flag | load only the entry scene's coarse LOD; no fine pages stream in. |

The memory arenas are read **at allocation time** from
`mem::config::Runtime()` (`Core/Memory/alloc_config.h`), which the
`TOURMALINE_MEM_*` overrides feed.

---

## 7. Validation

### Parity methodology

`TOURMALINE_GPU_SCENE_VISIBILITY=1` enables the fused pipeline with a CPU
reference oracle (`ReferenceVisibility` in `gpu_visibility_frame_ref.h`). The
runtime readback compares leaf masks and fused verdicts slot-by-slot;
`maskMismatch=0 verdictMismatch=0` means bit-exact parity with the CPU path.

### Gates

- `GpuSceneLeafClassifyTests` — 100 checks pinning the flat classifier's
  no-false-negative contract.
- `WorldSceneInstanceTests` — 65 checks on the direct hierarchy-instantiation
  path.
- `ArchitectureTests` — 12/12 struct-layout ABI checks.
- `test_text_integrity.py` — documentation/comment integrity.

### Results

| Scene | Result |
|---|---|
| `sponza_grid32` | fused path bit-identical (0/2.07M pixels differ) vs CPU; 216 leaves, `maskMismatch=0 verdictMismatch=0`. |
| `docks / ROOT_part0` (121k nodes, 60.5k meshes) | loads after the degenerate-mesh fix; `leaves=101 active=63 maskMismatch=0 verdictMismatch=0`; 669,212 tris drawn (was 32 before the fix). |
| `island_fixed` (197 scenes, 8.7 GB) | streams with heap-capacity admission; gate trips at `used + in-flight ≥ budget` with **0 failed pages** (was a hard `pak load failed … heap cap` abort). |

### Perf (indicative, uncapped `--bench`)

- Docks `ROOT_part0`: fused 29 ms vs non-fused 54 ms (~1.9× faster).
- Island coarse ~7 ms; island streaming ~10 ms.
