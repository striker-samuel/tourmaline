# Mesh entities + the Animator component — the runtime entity model

The gameplay-handle model's mesh/animator legs (the design's §4-§11),
on the foundation `entity_handle.h` laid with the audio entities. The
rules: **assets are immutable and shared; components define
capabilities; the World owns mutable state; the renderer consumes
it.**

## Keys vs handles

```
MeshKey asset = project::meshes::Knight;      // which ASSET (project.h)
MeshEntityHandle instance = world.SpawnMeshFromNode(rig, frame, parent);  // which INSTANCE
```

Handles are 64-bit generational (`EntityHandle<Tag>`), zero = invalid,
destroy bumps the generation (stale copies fail validation forever),
and they **survive scene rebuilds** — the resolve remaps the internal
NodeId, never the handle.

## Mesh entities

`World::SpawnMeshFromNode(sourceNode, frame, parent)` instances a
loaded world mesh (+ its material association) onto a fresh entity
node. A skinned mesh is still a mesh — skinning is a capability of the
mesh ASSET, not a separate entity category. The binding is
cross-cluster by construction: the source cluster keeps owning the
mesh pool entry (meshes ship inside their scene's cluster — the v1 pak
boundary), the entity stores the stable **(owner cluster, pool
handle)** pair, and the merged-scene rows are MIRRORS the resolve tail
re-establishes after every rebuild. `MeshEntityMeshIndex(handle)`
resolves the current world index; `DestroyMesh` drops the rows + nodes.

`SpawnMesh(MeshKey)` — the authored API — needs the key→loaded-mesh
join (the mesh's owning scene + a mesh-URI table in the manifest);
that's the app-layer follow-up listed below.

## The Animator

`world.AddAnimator(meshEntity, clip)` attaches per-entity playback
state (the entity's mesh cluster must carry the skeleton — the v1
single-skeleton-per-cluster contract). The `Animator` facade forwards
to World-owned state:

```
anim.Play(clip, loop);
anim.CrossFade(clip, 0.25f);
anim.SetSpeed(1.5f);
anim.Pause(); anim.Resume(); anim.Stop();
```

The engine (`engine/src/App/animator.cpp`): layers replace whole TRS
channels over the REST pose captured at attach (the .tourAnim rule —
untracked joints/channels keep the rest), the crossfade blends the
layered poses (lerp translate/scale, shortest-path nlerp rotate), and
each animator keeps its pose in a PRIVATE joint array — independent
playback state for every entity, exactly.

`World::Update(dt)` (driven by the renderer's frame build with the
frame's dt — bench-deterministic) advances every animator and flushes
the FIRST animator per target cluster into the cluster's scene, where
the existing skinning path consumes it (`Cluster::
poseExternallyDriven` tells the renderer to skip its own clip advance —
no double cursors). Claims reset each Update; a dead animator's
cluster is re-claimed by the next animator.

## Ownership + destruction

```
int Kill(smvk::World* world, Enemy* enemy) {
    world->DestroyAudioSource(enemy->audio);
    world->DestroyMesh(enemy->mesh);   // the ATTACHED animator dies with it
    *enemy = {};
    return 1;
}
```

Explicit destruction of independently-owned handles; automatic cleanup
only for explicitly-owned attachments (the animator is the mesh
entity's attachment slot — `DestroyAnimator` is the independent path).
The World never knows `Enemy` exists.

## v1 limits (documented follow-ups)

- **One rendered pose per cluster**: every animator's STATE is
  independent, but the flush writes one cluster-wide pose — two knight
  entities sharing one cluster render the first animator's pose. The
  per-draw palette table (a palette per draw, not per cluster) is the
  renderer generalization.
- **`SpawnMesh(MeshKey)` + `LoadMesh`/`LoadSkeleton`/`LoadAnimation`**:
  the app-layer asset-manager surface — the key→loaded-mesh join needs
  the mesh's owning-scene + mesh-URI rows in the appmanifest, and the
  `AnimationClipKey`/`SkeletonKey` catalog buckets (0x5xxx+) for
  `.anim`/skeleton identities. Clips currently pass as loaded
  `touranim::Clip` values (`LoadTourAnimText`).
- **One skeleton per cluster** (the importer's first-skin-wins
  contract) — a mesh-keyed skeleton table is the deeper unblock.

Tests: `WorldEntityTests` (`TestMeshEntitiesAndAnimators` — the
resolve re-mirror, the flush/sampling, pause/resume/speed/loop-wrap,
the crossfade blend arithmetic, the attachment cleanup, the
static-mesh refusal) — all GPU-less.
