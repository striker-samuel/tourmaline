# Particle source graph and brush-authored emitters

This is an initial **typed, open source graph** for particle *sources*, separate
from the GPU emit/sim/draw graph. It uses the same domain-neutral
`tour::graph::Document` as the placement and gameplay graph foundation;
the graph contains stable node IDs, named typed plugs, validated links,
and node attributes. Source evaluation is CPU-side and produces either existing
`smvk::ParticleEmitterGpu` rows or `smvk::MeshParticleEmitterGpu` rows,
depending on the sink rendering mode. The GPU's particle state, spawn scheduling,
simulation and rendering are unchanged.

## Nodes and plugs

| Node type | Input | Output | Owned attributes |
|---|---|---|---|
| `particle.source.painted` | — | `points: particle.points` | stroke selector |
| `particle.source.heightfield` | — | `points: particle.points` | terrain-group selector |
| `particle.source.placement` | — | `points: particle.points` | current placement result selector (reserved) |
| `particle.filter.height` | `points` | `points` | minimum/maximum world Y |
| `particle.emitter` | `sources: particle.points` (multi) | — | rate, lifetime, size, wind, tint, billboard/mesh mode, mesh bin |

Source plugs are *positions and normals*, not GPU emitters. The emitter
sink maps representative positions to world-space GPU emitter rows. Each
sink's rate is a **total** budget distributed across its resolved points
rather than multiplied by every dab. A finite row budget is enforced;
extra source points are deterministically downsampled and the caller
receives a warning.

External source kinds can use any `particle.source.<provider>` type name.
`Graph::AddSource` creates the standard typed `points` output, and
`Evaluate(graph, provider, capacity)` resolves each source through the
caller's `SourceProvider` callback. Providers can sample mesh surfaces,
spline paths, terrain windows, particle event streams, existing placement
points, or simulation/gameplay data without teaching the particle core about
their storage types.

This is a *source graph*, **not** an unrestricted custom shader or GPU
simulation graph. Expanding it to a full VFX graph should layer additional
typed particle-operation nodes and a GPU IR over this boundary.

## Editor workflow

1. Choose **Create > Surface brush (points / spline)** and paint points on
   visible meshes. The stored points follow parent object UUID + local
   transform, so moving the source surface moves the particles.
2. In **Surface Brush > Particle source graph**, the initial graph contains
   `painted.points -> emitter.sources`. Add a painted, terrain or placement
   source; source nodes connect to the emitter sink.
3. Optionally insert a world-height filter between a selected source and
   the emitter. Select the emitter node to tune its rate, tint, lifetime,
   wind and cloud sizes. The graph is evaluated for the inline live renderer.
4. Continue to use **Use cloud in Placement Graph** to create the existing
   Placement `PointCloud` domain. The same surface strokes feed either
   application, without duplicating their stored geometry.

The graph and surface strokes live in `.tour` **editorSettings**:
`particle.source.node.<id>` and `particle.source.link.<id>`.
Attributes serialize by versioned schema and node IDs; links use named ports.
Invalid saved topology is discarded in favor of the safe default.
This preserves the project distinction: editorSettings are NOT cooked
runtime authored emitters. Games can evaluate the graph with their own
`SourceProvider` and feed the resulting rows to
`WorldRenderer::SetExternalParticleEmitters`.

### Integration points

- **Surface brush:** parented stroke points are resolved live by
  `EditorApp::BuildPlacementPointCloud`, reused for particle inputs.
- **Terrain:** the editor reads the same live, painted `HeightfieldData`
  as the terrain and PlacementGraph system, and samples a bounded 4x4
  source grid per selected group. World-space height is sampled from that
  heightfield, never a separate analytic reconstruction.
- **Placement:** the current accepted PlacementGraph preview candidates
  become source points. The preview must be valid and fresh.
- **WorldRenderer:** external emitter rows are copied into a stable
  slot range after demo rows and before transient bursts. Source updates
  are retained by the renderer across frames; the GPU emitter buffer
  remains per-flight with GPU-owned fractional spawn carry.
- **Dust rendering:** the alpha-blended ground dust mode comes from the
  preceding ground-dust PR on which this change is stacked.

## Mesh emitter compilation (backend integration pending)

The emitter sink supports a `renderMode` of `kBillboardRender` (default)
or `kMeshRender`, with a `meshId` in the eight-bin GPU mesh-particle table.
`Evaluate` returns billboard rows in `Result::emitters` and mesh rows in
`Result::meshEmitters`. Both count toward the supplied evaluation capacity.
Mesh rows retain rate distribution, life, size envelope, tint, source point,
and a stable per-row seed. Attribute serialization uses `v2` and reads
existing `v1` documents as billboard emitters.

The editor exposes **Particle render type** (Billboard or Mesh instances)
and a **Mesh prototype bin** selector on the emitter sink. Mesh emitter rows
are forwarded to `WorldRenderer::SetExternalMeshParticleEmitters`, alongside
the original billboard rows. The renderer creates its own `MeshParticles`
pool, runs GPU emission and simulation, culls and bins particles against the
main RenderView, builds indirect draws, and renders spinning instances with
`meshparticles_draw.vert.hlsl` and the placement diagnostic unlit fragment
shader. Main's GPU read hints synchronize compute outputs and indirect draws.

**Geometry contract:** mesh IDs map to the indexed prototype bins registered
via `AttachGpuPlacementRuntime` (up to eight). Invalid or unseeded bins are
skipped; no arbitrary asset lookup is performed by the emitter. Meshes use
placement's static vertex/index heap. A project without registered placement
prototype geometry can still author mesh particle settings, but it will not
produce mesh geometry until the host registers valid prototypes. Per-bin
bounds currently use a conservative fallback sphere; cooked bounds and lit
material selection remain follow-up improvements.

The graph stores **editor settings**, not a cooked runtime graph asset. Runtime
applications can call the external mesh emitter API with compiled rows.

## Current limitations / follow-up work

- The source graph is shown as editable node/plugs/attribute controls in
  the Surface Brush panel, not yet as a full draggable canvas. The shared
  graph topology makes a dedicated canvas possible without changing
  evaluation.
- Max GPU emitters is 16, shared with demo and transient sources; the
  editor reserves 11 external rows. There is no per-emitter stable-ID
  rebinding across topology edits, and no indirect GPU emitter allocation.
- Surface strokes and the particle graph are editor-only until a future
  cooked graph asset + world-component schema is introduced. Runtime
  applications can already inject their own evaluated emitter rows.
- Heightfield source sampling is a 4x4 representative grid; dense field
  sources should use tiled GPU emission/placement buffers rather than
  allocating thousands of CPU rows.
- No general terrain-clamping simulation, soft depth-fade or sorted
  transparency is added here; see the ground-dust PR.
- Editing/undo for graph topology currently persists through Autosave
  but has no integrated transaction-based graph undo history.
- The live renderer preview is GPU-dependent; `particles_tests` verifies
  typed graph validation, filtering, serialization and emitter compilation
  without graphics.

The intent is to keep sources **open** and domain-independent while
allowing future GPU placement kernels, terrain tiles, ecotope weights,
mesh sampling and runtime scene graph nodes to supply the *same*
`particle.points` plug type.
