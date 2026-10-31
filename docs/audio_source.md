# The AudioSource node — authored emitters, sound assets, runtime playback

The AudioSource pipeline in three stages (the authoring → content →
playback split). Design rule throughout: **a sound slot is an ASSET
REFERENCE, never a playing voice** — clips are shared by many emitters;
playback belongs to runtime state.

## 1. Authoring (the node association)

An AudioSource is an ordinary transform node + an `AudioSourceRow`
(`scene.tables.audioSources` — the inline-payload pattern, no pool):
`SceneAudioSource { volume, spatial, playOnLoad, minDistance,
maxDistance, sounds[] }` with `AudioSoundRef { name, assetPath,
soundId, gain, loop }` (`scene_payloads.h`). The emitter POSE is the
node's GLOBAL transform — parent the tower under the church and moving
the church moves the bell. Nodes are transforms; association tables
describe what they do.

`.tour` surface (the camera/exportSettings pattern — payload via
tabulated lines under the create):

```
createNode -at audioSource -p 0 -name "BellTower" -t 0 3 0;
	setAudioAttr -ref 0 -at volume -v 0.8;
	setAudioAttr -ref 0 -at spatial -type "bool" -v on;
	setAudioAttr -ref 0 -at playOnLoad -type "bool" -v on;
	setAudioAttr -ref 0 -at minDistance -v 2;
	setAudioAttr -ref 0 -at maxDistance -v 30;
	addAudioSound -ref 0 -name "Bells" -source "content/audio/bells.wav" -v 1 -loop on;
```

`-ref k` addresses the k-th create (`kCreateAudioSourceRefBase
0x02000000`); the SLOT NAME is the stable slot identifier (reordering
the list never breaks undo/history). Verbs: `setAudioAttr`
(volume/spatial/playOnLoad/minDistance/maxDistance/name/position),
`addAudioSound` (UPSERT by slot name), `removeAudioSound`.

Editor: Create → Audio source, the emitter inspector (volume/distances
drags, spatial/play-on-load, the sounds list), full undo/redo, tour
round-trip. Tests: `TourTests` (`TestAudioSource`), `SceneTableTests`.

## 2. Content (sounds as first-class assets)

- **Ids**: `.wav` sources claim `PlanAssetKind::Sound` in recipes
  (`include content/audio/*.wav`); identity is the `.wav.meta` sidecar
  GUID; ids allocate in the **0x4000 range** (scenes 0x1xxx, textures
  0x2xxx, meshes 0x3xxx, sounds 0x4xxx). The generated `project.h`
  carries `namespace sounds { inline constexpr SoundKey bells{...}; }`
  (typed keys — see below).
- **Pak blobs**: `PakAssetKind::Sound = 11` — raw `.wav` bytes at the
  original VFS path, the SECOND VFS-addressable kind (with textures):
  `PakBackend::TryRead` serves them, cross-pak ownership
  (contentSet/excludeVfs/policing) works identically.
- **The cooked scene** stores **numeric ids, never paths**: the scene
  blob's v18 audio section (`cluster_serializer.cpp`) writes
  `(node, volume, flags, minDist, maxDist, sounds[(u64 soundId, f32
  gain, u32 loop)])`. The pak cooker resolves paths → ids through the
  `--sound-id` map (the executor's pre-stage catalog ensure builds it —
  idempotent, first-build-safe). **A non-empty map with an unresolved
  referenced path FAILS the cook** (never silently omit); an empty map
  (bare cooks) stamps 0 = unresolved with a loud runtime warning.
- **The appmanifest** `[ids]` table carries the sound rows; the runtime
  façade's `ReadAsset(project::sounds::Bells)` returns the raw bytes.

## 3. Playback (the scene-aware layer)

`app/src/AudioRuntime.h` — an `IAppElement` over the mixer:

```
auto church = scenes.LoadScene(world, ctx, stream, project::scenes::Church);
PrefetchSound(project::sounds::Bells);          // warm + PIN the clip
auto voice = PlaySound(world, bellTowerNode, project::sounds::Bells);
SetAudioSourceVolume(world, bellTowerNode, 0.5f);
StopSound(voice);
SetListener(pos, fwd, up);                      // or follow the camera (default)
```

- **Mixer**: per-voice pan (constant-power, `SetVoiceSpatial(vol,
  pan)`) — all spatial reasoning lives ABOVE the mixer.
- **Emitter states** key by the STABLE (owning cluster, cluster-local
  node) pair through `World::clusterMergedNodes` — merged node ids
  rebuild on every resolve; emitters (and their voices) never confuse.
  Per frame: linear min→max distance rolloff + right-axis pan against
  the listener, change-detected mixer commands.
- **Lifecycle**: `playOnLoad` autoplays the first slot once per scene
  life; a RemoveScene's rebuild drops that scene's emitters → their
  voices stop, everyone else keeps playing. The clip cache keeps a
  sound while ANY live emitter references it OR a `PrefetchSound` pin
  exists ("shared or explicitly prefetched clips remain cached").
- **Spawned emitters** (gameplay): `World::SpawnAudioSource(frame,
  parent, def)` → `AudioSourceHandle` (the typed gameplay handle,
  `entity_handle.h`); `World::DestroyAudioSource(handle)` bumps the
  generation — stale copies fail validation forever, the voices stop.
  Your gameplay objects compose the handles:

```
struct Enemy {
    smvk::MeshEntityHandle mesh{};
    smvk::AnimatorHandle animator{};
    smvk::AudioSourceHandle audio{};   // all default-zero
};
int Kill(smvk::World* world, Enemy* enemy) {
    world->DestroyAudioSource(enemy->audio);
    world->DestroyMesh(enemy->mesh);       // (the mesh-entity surface)
    *enemy = {};
    return 1;
}
```

**Typed keys vs handles** (`project.h` + `entity_handle.h`): a KEY
names WHICH ASSET (`SoundKey`/`MeshKey`/... — immutable, shared); a
HANDLE names WHICH RUNTIME INSTANCE (World-owned, generational). The
dogfood: `samples/04_project_game` — the `BellTower` audio source in
`game.tour` rings the shared pak's `bells.wav` (looping, spatialized)
on scene load; run `GameProject.exe` after `GameProject_paks`.

v1 scope: linear rolloff + clamped right-axis pan, omnidirectional
emitters, one autoplay slot, WAV only (SDL decodes; everything
converts at load). Cones/occlusion/streaming/compressed formats are
forward work that does not change the authored node model.
