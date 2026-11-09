# Authored audio graphs (docs/audio_graph.md)

The audio system's GRAPH layer — the direct parallel of the skeletal
animation stack:

| Animation | Audio |
|---|---|
| Skeleton (`.tourAnim`'s joint table) | **AudioGraphDoc** (the authored node topology) |
| Joint | node (source / gain / filter / delay) |
| Animation clip | **cue** (a named timeline) |
| Animator | **audio instance** (`tour::audio::AudioInstance`) |
| CrossFade | `crossFade` (the two-layer weight model) |
| Evaluated pose | the rendered PCM signal |

**An authored graph is ONE asset** (a `.audiograph` text file, a
0x7xxx `AudioGraphKey`): the topology, its exposed parameters, and ALL
of its cues. Cues ride the graph the way clips ride a `.tourAnim`
file — there is no separate cue asset kind; the generated project.h
additionally emits **per-cue constants** (`project::audiocues::
<graph>_<cue>`) bundling (graph id, cue ordinal).

## The rendering core: miniaudio, device-less

`external/miniaudio` (vendored 0.11.26, MIT-0/public domain; impl TU
`engine/src/Audio/miniaudio_impl.cpp`, feature macros in
`engine/src/Audio/miniaudio_config.h`) is **the DSP + graph execution
core** — and nothing else:

- **SDL3 keeps the device.** The `ma_engine` ALWAYS runs in
  `noDevice` mode; the mixer thread renders each chunk through
  `ma_engine_read_pcm_frames` and pushes it into the same
  `SDL_AudioStream` as always (same watermark pacing, same core
  pinning).
- **Paks + the content registry keep the content.**
  `MA_NO_RESOURCE_MANAGER` / `MA_NO_DECODING` — sounds are created
  from in-memory `ma_audio_buffer`s over the bytes SDL decoded from
  the pak's Sound-kind blob (ONE copy of the bytes, no second decode).
- **miniaudio owns the graph machinery**: the per-voice
  source/pan/pitch/resample path, the biquad + delay effect nodes,
  and the sound-group submix buses.

The legacy voice lane (`Play`/`Stop`/`SetVoiceSpatial` — the
scene-aware layer's surface in docs/audio_source.md) is unchanged at
the API level; each voice is now an `ma_sound` attached to the
endpoint. The scene layer's spatial reasoning still lives entirely
ABOVE the mixer.

## The authored format (`.audiograph`)

```
version 1

node -name "wind"  -type source -sound "audio/wind.wav" -gain 0.8 -loop on
node -name "lpf"   -type lpf    -cutoff 900 -order 2
node -name "env"   -type gain   -gain 0.0
node -name "master" -type gain  -gain 1.0

connect -from "wind" -to "lpf"        # fan-in + fan-out allowed
connect -from "lpf"   -to "env"       # every node must reach "output"
connect -from "env"   -to "master"
connect -from "master" -to "output"   # the reserved graph exit

param -name "wind" -node "env" -field gain   # the exposed surface

cue -name "calm"                          # the FIRST cue is the default
start -node "wind" -t 0
ramp  -param "wind" -t 0 -value 0.0
ramp  -param "wind" -t 6 -value 0.7

cue -name "storm"
start -node "wind" -t 0
set   -param "wind" -t 0 -value 1.0
```

**Node types**: `source` (a sound asset by VFS path + authored level +
loop), `gain` (a submix bus; the master shape), `lpf`/`hpf`/`bpf`
(cutoff + order), `notch`/`peak`/`loshelf`/`hishelf` (frequency, q,
gainDb), `delay` (delay seconds + decay — feedback lives INSIDE the
node, never in the edge topology: graph cycles are a parse error).

**Param fields** (the `-field` vocabulary): `gain`, `cutoff`,
`frequency`, `q`, `gainDb`, `decay`. A param's field must be valid
for its node's type. Params are the ONLY automated/controllable
surface — raw node addressing never leaves the asset.

**Cue events**: `start`/`stop -node <source> -t <sec>` (edge-fired)
and `set`/`ramp -param <name> -t <sec> -value <v>` (keyframes: set =
step, ramp = linear to the next key). A track OWNS its param from cue
entry (before the first key it holds the BASELINE captured at entry)
through its LAST keyframe; past the last key the param RELEASES to
manual control (cues sustain indefinitely — a forever-hold would
starve gameplay `set`). Keyframe times are strictly ascending. A cue
has no duration: started sources keep sounding until stopped,
replaced, or the instance stops.

**Strict parse** (`tour::asset::ParseAudioGraphText`,
engine/Engine/Asset/audiograph.h): every malformation — unknown verb,
dangling edge, node that never reaches `output`, a cycle, an invalid
param binding, non-ascending keys — fails with the LINE NUMBER. The
SAME parse runs at THREE gates: the catalog's ids pass (a malformed
graph never reaches a project.h), the pak cooker's content-set lane
(corrupt-refused, like the shader magic gate), and the runtime read.

## Instances (the animator parallel)

The ASSET is immutable + shared; the INSTANCE owns its state. The
engine's `AudioEngine` owns the registry (not the World — audio
instances are not scene-space objects in v1):

```cpp
auto& audio = ctx.audio();                       // app::Context
auto forest = audio.create(project::audiographs::forest);
forest.play();                                   // the default cue
forest.set("wind", 0.75f);                       // an exposed param
forest.crossFade(project::audiocues::forest_storm.cue, 2.0f);
forest.stop();
```

- `smvk::AudioInstanceHandle` (entity_handle.h): 64-bit generational,
  the same shape as `AnimatorHandle`. A destroyed instance fails every
  facade call silently; stale copies fail forever.
- `tour::audio::AudioInstance` (Engine/Audio/audio.h): the facade —
  `play`/`playCue`/`crossFade`/`stop`/`set`/`setVolume`/`playing`,
  by cue/param ordinal or NAME.
- Clip resolution runs on the CALLER thread before the create command
  (`AudioClipResolver` — the app lane reads Sound-kind bytes by VFS
  path through the session and decodes once, cached per path). Pak
  reads + WAV decode NEVER touch the mixer thread.
- `app::AudioGraphs` (app/src/AudioGraphs.h): the app-side service
  behind `ctx.audio()` — the typed-key create, the parsed-doc cache
  (per graph id) and the decoded-clip cache (per sound path).

### The audio-buffer-boundary contract

All graph mutations + parameter scheduling happen at CHUNK boundaries
on the mixer thread, and steady-state mutation is **allocation-free**:

- **Create** (one sanctioned allocation burst): the mixer thread
  builds the WHOLE node set at the next boundary — every authored
  node (groups/biquads/delay + splitters for fan-out) and every
  **(source, cue) voice**, pre-created STOPPED at volume 0.
- **Cue starts / crossfades / param sets**: pure
  seek/start/stop/volume/reconfig ops — no allocation, no blocking.
- **Automation** is evaluated ON the mixer thread at the chunk cadence
  (21 ms at the 1024/48k default) — immune to frame stalls, and
  deterministic in the offline lane.
- Crossfade = the `AnimatorCrossFade` two-layer weight model: the
  current cue's voices + automation ramp out (1-w) as the target's
  ramp in (w); params both cues automate BLEND by the weight; at
  completion the fade layer becomes current with its own cursor and
  baseline. A stacked fade snaps the previous one (v1).
- Biquad param changes go through miniaudio's `*_node_reinit`, which
  RESETS the filter state — coarse cutoff automation can click
  slightly (a smoothed param node is the documented follow-up).
  Volume changes ride the engine's 256-frame gain smoothing.

### The offline lane (tests + future bounce)

`Config::deviceless = true` + `PumpOffline(frames, out)` render
deterministically on the caller's thread: no device, no mixer thread,
no wall clock. `audiograph_tests` (tests/test_audiograph.cpp) drives
the whole instance engine this way — automation ramps, crossfades,
immediate sets, teardown — with RMS assertions over rendered chunks.

## The content pipeline

The kind's full ladder (the shaders/gi pattern):

- **ABI kind byte** `kCatalogKindAudioGraph = 6` (appmanifest.h;
  kinds append, never renumber) + the `"audiograph"` string join.
- **Pak kind** `PakAssetKind::AudioGraph = 16` — the authored text
  VERBATIM, the FOURTH VFS-addressable kind (PakBackend
  Exists/TryRead/ListDir/Stat serve it by path; cross-pak graphs
  resolve like sounds).
- **Content ids** 0x7000..0x7FFF (`IdRangeForKind`); the identity is
  the file's `.meta` sidecar GUID.
- **Plan kind** `PlanAssetKind::AudioGraph`; the recipe resolver
  classifies `.audiograph`.
- **The catalog**: the `audiographs` bucket (id-sorted, fingerprinted
  in emission order), the parsed **cue table** (catalog-time
  validation), the `AudioGraphKey`/`AudioCueKey` typed keys, the
  `project::audiographs` + `project::audiocues` namespaces, the
  `AssetKind::AudioGraph = 6` enum row, the flat `Assets` table and
  the `AssetTraits<AudioGraphKey>` query binding.
- **The cook**: the content-set lane validates the parse (corrupt
  never ships) and embeds the blob; the per-pak `#ids` identity table
  carries the kind byte; the ownership policing + `excludeVfs` treat
  it like every other VFS-addressable kind.
- **Sound references stay PATHS** — the graph's source nodes name
  `.wav` VFS paths, resolved at instance create through the same
  registry (a missing sound fails the create LOUDLY with the named
  path). No numeric id stamping (the cross-pak path form is the
  stable identity here, unlike cooked scenes which stamp ids).

## The Schroeder reverb (v1 DSP)

`engine/src/Audio/schroeder_reverb.{h,cpp}` — a custom miniaudio
`ma_node` (the ONE custom node; everything else is stock miniaudio):
four staggered feedback combs in parallel, two all-pass diffusers in
series, per channel, with distinct left/right delay tunings
(Freeverb's reference values at 44.1 kHz, scaled to the engine rate)
and a one-pole high-frequency damper inside each comb's feedback path.

- **Two modes, one implementation**: MIX (the authored-graph inline
  form: `out = (1-wet)*dry + wet*processed`; wet 0 is an EXACT
  bypass — the callback short-circuits to a copy) and WET-ONLY (the
  environmental send form: the node emits only the tail; the dry path
  rides a separate splitter branch, so the send gain never attenuates
  the dry signal).
- **The audio-thread contract**: all heap allocation at init (the
  sanctioned burst), freed at uninit; the callback never allocates,
  locks, logs or reinitializes; delay line LENGTHS are fixed for the
  node's lifetime; coefficients (comb feedback from RT60 via
  `fb = 10^(-3*D/rt60)`, the damping pole, the wet mix) smooth toward
  their targets inside the callback (~30 ms glide).
- `MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT`:
  tails drain through input silence (a stopped source never truncates
  its reverb). Non-finite input reads as zero; comb filter states
  zero-snap below 1e-20 (denormal protection); feedback clamped
  strictly below unity.

Pinned by `ag.reverb.*` (audiograph_tests): byte-exact wet-0 bypass,
the tail outliving a 50 ms source at rt60 3, decay ordering (rt60 0.25
dead vs rt60 3.0 ringing at the same instant), stereo decorrelation,
finite output, and mid-flight RT60 retargets.

## Authored graph `reverb` nodes

`node -type reverb -wet 0.28 -rt60 1.8 -damping 0.60` — the inline
form (`AudioNodeType::Reverb = 11`, appended; the param fields
`wet`/`rt60`/`damping` appended to `AudioParamField`). The bounds gate
at all three parse points (catalog ids, cooker, runtime):
wet/damping 0..1, rt60 0.15..8 s. Like `delay`, the reverb's feedback
lives INSIDE the node — it authorizes no graph cycles.

## Audio rooms (the environmental reverb volumes)

**`SceneAudioRoom`** (scene_payloads.h) — a node-local payload:
`enabled`, `priority` (audio's own ordering, INDEPENDENT of GI
lighting priority), `insideFade`/`outsideFade` (per-face metres),
`wet`, `rt60`, `damping`, `transitionSeconds`. The room SHAPE is the
node's transform — the GI-volume placement convention, decoded at the
listener by `Engine/Scene/spatial_volume.h` (the ONE shared OBB
utility: center = global translation, axes = normalized transform
columns, half-extents = HALF the column lengths; per-face
smoothstep fades; min-of-six-faces; parity with the GI volume's own
plane math pinned by scene_table_tests). **Independent of GI by
design**: a node may carry BOTH a `GiVolumeRow` and an `AudioRoomRow`
(one placement, two associations), or only a room — nothing here
reads `.givolume` artifacts, probe textures or the renderer's GI
budget.

- **Authoring**: `.tour` verbs — `createNode -at audioRoom` (the
  givolume-lane create: `-t` position, `-s` FULL extents),
  `attachAudioRoom`/`removeAudioRoom` (the association verbs; a room
  on a created GI volume rides `-ref k -owner givolume`), and
  `setAudioRoomAttr` (the property lines + the position/rotation/
  size shape channels; post-creation transform edits lower to them,
  the givolume lane). `kCreateAudioRoomRefBase = 0x01000000`.
- **Editor**: Create → Audio room; the inspector's Audio room section
  (the Enable button attaches a default row to ANY transform node;
  every field drags with coalesced undo); teal X-ray box gizmos
  (View toggle) + pick; full CmdToTour/TourToCmd round-trip,
  duplicate/delete capture, and the family-ref drop guards.
- **Cooked scene**: the blob's **v20 audio-room section** (appended
  after v19 schemaAssocs; the sanctioned break — old readers reject
  at the trailing-bytes check): `u32 rowCount` + per node-sorted row
  `(denseNode, flags, priority, insideFade, outsideFade, wet, rt60,
  damping, transitionSeconds)`. Values sanitize on read (the last
  gate). The world merge copies rows per-cluster through the node map,
  exactly like the audio sources.

### The environmental world-SFX bus

```
spatial SceneAudioSource voice ──┐
                                 ├─> worldSfxGroup ─> splitter ─┬─ dry ─> endpoint
PlaySound (spatial emitters) ───┘                              └─ wet ─> SchroederReverbNode (wet-only)
                                                                        └─> wetGain ─> endpoint

2D / UI / music voices ────────────────> endpoint (Direct, the default)
.audiograph instance buses ────────────> endpoint (unchanged by default)
```

`Play(..., AudioRoute route)` gains the route parameter (default
`Direct` — every pre-room caller keeps its exact old behavior); the
scene-aware layer routes `spatial == true` emitters to `WorldSfx`. A
failed bus build degrades WorldSfx to the direct attach (logged once,
never fatal). The reverb is a **send**: the dry path is never
attenuated; the listener-room weight drives the wet gain.

**Listener-side evaluation** (AudioRuntime, after ResolveListener):
walk the durable pak clusters' `audioRooms` rows (the emitter-walk's
shape — stable keys, merged transforms through `clusterMergedNodes`,
never cached merged ids), sort by descending audio priority (stable
cluster/node tie-breaks), blend by **ordered coverage**: each room
contributes `remaining * weight(room)` of its wet/rt60/damping, the
leftover coverage stays at the OUTDOOR baseline (wet 0, rt60 0.35 s,
damping 0.5). ONE change-detected `SetEnvironmentReverb` command per
material change; all smoothing is mixer-thread-side (the wet gainer's
256-frame ramp + the reverb's ~30 ms coefficient glide) — immune to
frame stalls, no zipper.

**MVP rule** (the honest scope): room geometry governs
listener-side environmental reverb only — no openings, no
source-to-listener transmission, occlusion, diffraction or per-room
impulse responses. It is not geometric acoustics.

### The per-voice cursor (the shared-buffer trap)

**A clip's decoded bytes are wrapped by PER-VOICE
`ma_audio_buffer_ref` cursors — never a shared `ma_audio_buffer`.**
One `ma_audio_buffer` carries ONE cursor: the first voice to play a
clip exhausts it, and every later voice wrapping the same buffer
reads silence; two simultaneous voices would fight over the position.
The instance lane's seek-to-0 masked this for sequential instances;
overlapping same-clip instances still raced. Pinned by
`ag.bus.raw-cursor-trap`/`-fix` (the raw demonstration) and
`ag.bus.second-play-rings`/`ag.bus.world-*-rings` (the engine-level
regression pins).

## v1 limits + follow-ups

- ~~**No reverb node**~~ — **LANDED**: the Schroeder reverb
  (`engine/src/Audio/schroeder_reverb.{h,cpp}`) serves both the
  authored-graph `reverb` node and the environmental world-SFX send.
  Follow-ups: separate early reflections, dual crossfaded reverb
  processors for sharp room changes, real room-material estimates.
- **No spatialization inside graphs** — graph sources render
  non-spatial stereo in v1; the scene-emitter lane (amplitude+pan per
  frame) plus the listener-driven environmental send are the spatial
  surfaces. A per-voice spatializer bridge (the `ma_spatializer` is
  already in every `ma_sound`) is the follow-up.
- **Instance volumes are not sample-accurate** — they land at chunk
  boundaries with 256-frame smoothing (21 ms cadence); fine for beds
  and ambience, not for rhythm-critical one-shots.
- **The doc passes by value into the instance** (the animator's
  clip-by-value rule); a shared doc cache beyond the app lane's is
  unnecessary at graph sizes.
- **No graph editor yet** — the `.audiograph` format is authored as
  text; AUDIO ROOMS have their full editor surface (create, inspector,
  gizmo, undo, tour round-trip) — a visual graph editor remains the
  follow-up the format was shaped for.
- **Room audio v1 follow-ups**: per-source room sends, doorway/portal
  topology, real material absorption, the per-room transition glide
  (the field cooks + round-trips; the runtime uses the engine's fixed
  ~30 ms coefficient glide).
- Pinned by: `audiograph_tests` (parser + engine + reverb + world
  bus), `audiograph.*` in project_pipeline_tests, `room.*` in asset
  tests (the v20 section), `room *` + `volume *` pins in
  scene_table_tests + tour_tests, the sample's compile-time pins
  (`samples/04_project_game`, local-only), and `project_e2e_tests`.
