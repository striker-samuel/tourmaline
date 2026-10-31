# ProjectScenes — the runtime scene-instance layer

`app/src/ProjectScenes.h` is the reviewer-note layer over
[`ProjectRuntime`](../app/src/ProjectRuntime.h) (the apphost's project
façade): **numeric-ID-only scene addressing**, the
`LoadScene` / `LoadSceneAdditive` / `RemoveScene` instance lifecycle,
and **ownership-aware prefetch**. Game code passes
`project::scenes::*` constants; no pak filenames, cooked scene names,
or VFS paths enter from the outside.

## The id→scene join

Scene content ids resolved to their pak, but nothing joined an id to
the COOKED SCENE inside that pak — the appmanifest's `[ids]` scene
rows carried the source `.tour` path while the runtime loads scenes by
their cooked NAME (`ListPakScenes`). The join is now recorded data:

- `[ids]` scene rows take an optional **5th token** — the cooked
  scene's name (`AppManifestAsset::name`, `appmanifest.h`). The build
  executor records it when it publishes the manifest (the same
  `ListPakScenes` result the entry-scene identity uses; one reopen per
  pak, `scene[0]` with a loud multi-scene warning — the ownership model
  keeps one tour scene per pak).
- `ProjectScenes::ResolveSceneTarget(id)` resolves
  id → pak → (name, pak scene index), cached. A recorded name that is
  missing from the pak fails loudly (build/runtime version skew —
  recook). Rows without a name (pre-token manifests) infer from a
  SINGLE-scene pak; a multi-scene pak without a recorded name refuses
  (`recook`), never "whatever sits at index 0".

## Scene instances

```
app::ProjectRuntime rt = *app::ProjectRuntime::Load(tapp, err);
app::ProjectScenes scenes(rt, renderer.ContentVfs());

auto church = scenes.LoadScene(world, ctx, streamer, project::scenes::Church);
auto crypt  = scenes.LoadSceneAdditive(world, ctx, streamer, project::scenes::Crypt);
scenes.RemoveScene(church, world, err);
```

- **`SceneHandle`** — 64-bit generational (generation `<<32 | index`,
  generation starts at 1; all-zero = "no scene"). The same shape as
  the gameplay handles (`MeshEntityHandle` / `AnimatorHandle` /
  `AudioSourceHandle`): gameplay structs compose these by value and
  default them to zero. `RemoveScene` bumps the slot's generation —
  stale copies fail validation forever, a freed slot's replacement can
  never be resolved by accident.
- **`LoadScene`** = REPLACE (unloads every loaded project scene, then
  loads); **`LoadSceneAdditive`** stacks on top. Both deserialize the
  pak blob into a NEW registered cluster (`World::LoadClusterFromPak`
  through the `clusterLoader` seam) and run the apphost's shared
  `PostLoadTail` (anim attach, cooked static flags, the resolve that
  folds the cluster into the merged world scene) — an additive scene
  gets the entry scene's exact treatment.
- **`RemoveScene`** = `World::UnloadCluster` (the merged scene rebuild
  drops the scene's nodes) + the ownership release (below).
- The constructor's **adopt-or-open pass**: every manifest pak is
  adopted from the host's existing VFS mounts when present (a
  `--project` launch already preloaded + mounted the set —
  `ProjectRuntime::AdoptPakBackend` feeds them into the façade's cache
  so its reads serve through the SAME image), else preloaded in
  parallel, then mounted in manifest order at priority 10. One pak
  image per archive in memory, whatever opens it first.
- The pak→cluster step is the public `clusterLoader` seam: the GPU-less
  tests (`tests/test_pak_host.cpp` `TestProjectScenes`) substitute a
  hand-built offline cluster; the default runs the real deserialize
  and refuses a null `ResourceManager` (the engine's GPU-less test
  convention — `EnsureDynamicCluster`'s).

## Ownership-aware prefetch

```
scenes.Prefetch(project::sounds::Bells);   // warm the pak + PIN the id
scenes.Release(project::sounds::Bells);    // drop the pin
```

- `Prefetch(id)` warms the owning pak (the background preload + CRC
  open — the expensive half of a first read) and pins the id; scenes
  additionally resolve + cache their `SceneTarget`. `Release` drops a
  pin. Pins survive scene removal.
- **The ownership registry**: a loaded scene OWNS the assets registered
  against it (`SceneAcquireAsset` — scene-aware layers register at
  their discover points; the audio layer registers each scene's sound
  ids). `IsHeld(id)` is true while ANY scene owns it or a pin exists.
  `RemoveScene`'s `releasedAssets` out-param reports the ids whose LAST
  owner went away and that carry no pin — exactly what decoded-form
  caches (audio clips) evict. "Shared or explicitly prefetched clips
  remain cached."
- v1: pak backends stay open for the runtime's lifetime (the archive
  is the IO channel + mount surface). The ownership registry is the
  machinery a future image-eviction policy would drive; eviction itself
  waits for a measured need.

## Where it runs

Main/render thread only (the world loading it builds on is
load-time-only there). The tests run the whole machinery headless in
CI; every cooked project launch exercises the default cluster loader.

## The entry path

The apphost's `--project` launch (`LoadProjectWorld`) keeps its own
entry flow — it preloads every pak BEFORE Vulkan init (the startup
overlap is a feature) and loads the entry scene by the manifest's
recorded identity. `ProjectScenes` is the layer GAME CODE drives on
the live world (scene switching, additive level stacking, streaming
prefetch) — the entry scene loads through it untouched until a host
opts in.
