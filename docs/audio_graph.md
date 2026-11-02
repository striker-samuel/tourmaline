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

## v1 limits + follow-ups

- **No reverb node** — miniaudio 0.11 ships no reverb; the stock set
  is source/gain/lpf/hpf/bpf/notch/peak/loshelf/hishelf/delay. A
  Schroeder comb+allpass `ma_node` implementation is the natural
  follow-up.
- **No spatialization inside graphs** — graph sources render
  non-spatial stereo in v1; the scene-emitter lane (amplitude+pan per
  frame) is the only spatial surface. A per-voice spatializer bridge
  (the `ma_spatializer` is already in every `ma_sound`) is the
  follow-up.
- **Instance volumes are not sample-accurate** — they land at chunk
  boundaries with 256-frame smoothing (21 ms cadence); fine for beds
  and ambience, not for rhythm-critical one-shots.
- **The doc passes by value into the instance** (the animator's
  clip-by-value rule); a shared doc cache beyond the app lane's is
  unnecessary at graph sizes.
- **No editor surface yet** — the format is authored as text; a
  visual graph editor (the editor's Create/inspector surfaces) is the
  follow-up this format was shaped for.
- Pinned by: `audiograph_tests` (parser + engine), `audiograph.*` in
  project_pipeline_tests, the sample's compile-time pins
  (`samples/04_project_game`), and `project_e2e_tests`.
