# AGENTS.md — instructions for AI coding agents and human collaborators.

## Text encoding integrity

Repository text is UTF-8. Damage from encoding-unsafe local edits
(replacement characters, mojibake, non-ASCII dashes in PowerShell
switches) is caught automatically - no agent-facing checklist:

- `tests/test_text_integrity.py` is the regression suite AND the
  enforcement: it covers the fixtures and scans the whole tracked tree
  (vendored `external/` and the checker's own fixture files are
  exempted), so `python -m unittest discover -s tests -p
  "test_text_integrity.py" -v` is the single command.
- The `Text integrity` CI workflow runs exactly that suite on every
  push and pull request.
- When scripting file writes, prefer structured edits; if you must
  rewrite via PowerShell, write UTF-8 without BOM. Unicode em/en dashes
  and non-English text are legitimate content - never globally replace
  dashes.

## Agent command-line tooling manual

**Snapshot recording and cross-agent verification:** Read
[docs/agent_snapshot_cli.md](docs/agent_snapshot_cli.md) first. The supported
GPU/CPU entry point is `python tools/toursnapshot.py record|inspect|verify|extract|compare|compare-sequence|input-tape`.
Use `record --dry-run --json` to inspect a reproducible plan, then
`verify --json` (with `--pixels` when Pillow is available) before comparing
captured engine-observable world states. Return code **2** is a semantic
difference, **1** is invalid/corrupt/failed work, **64** is CLI misuse.
The snapshot includes no user game code; compatible gameplay rules and assets
are still required for actual re-simulation.


**Native C++ consumers:** [docs/tooling_api.md](docs/tooling_api.md) documents the `tourmaline::tooling` STATIC CMake target and its project init, SMP8 snapshot/diff and capture/pixel helper APIs; the `tourinit` executable is a thin frontend over this library. Existing Python CLIs remain supported and are not yet bindings to the native API.

**Start with [docs/agent_cli_tools.md](docs/agent_cli_tools.md)** for headless package inspection (`tools/tourinfo.py`), native-backed named project scaffolding (`tools/project_init.py`), stable-ID release comparisons (`tools/tourdiff.py`), repeatable fresh-frame renderer capture (`tools/tourcapture.py`), Unix pipe / JSONL / glob / regex workflows, and decoded-pixel screenshot comparisons (`tools/pixel_compare.py`), golden-capture regression gates, benchmark comparison, and HZB diagnostics. This manual is the canonical discoverable entry point for agent-facing CLI commands. Prefer it to invoking a GUI for artifact inspection; keep existing content IDs and SMP8 compatibility rules intact.


## Benchmarking and performance regression workflow

- Build `Benchmark` explicitly with the same CMake build directory and shader configuration used for the test: `cmake --build out/build/x64-Release --target Benchmark` (Linux uses `Benchmark`, Windows `Benchmark.exe`). It is a separate cooked-pak host with **no gameplay/audio sample elements** and requires a positive `--bench` budget; an interactive test belongs in `App`.
- For reproducible workload capture, launch `App` with a cooked pak or `.tapp`, navigate to an expensive view, then use **TAB → Benchmarks → Export for benchmark** (press **I** to enable the UI cursor if needed). Review and commit `benchmarks/scenarios.json`. IDs derive from the label, with collision suffixes; saved `environment`/`app_args` capture supported startup pins and the consolidated `--with`/`--without` pass graph, not every live-only editor/viewer slider.
- Run `python tools/perf_harness.py list` to inspect the catalog and `python tools/perf_harness.py bench --scenario <id> --exe <build-dir>/Benchmark[.exe] --repeat 5 --output out/perf/<label>` from the repository root. The executable's build directory is the renderer CWD; saved scene paths are relative to **that** directory. The sample `bistro-ssr-stress` needs a cooked Bistro pak not included in the Sponza release.
- Scenario replay scrubs inherited `TOURMALINE_*` environment variables; explicit `--env` pins are recorded. `compare` rejects mismatched scenarios, scene args, app args, frame counts or environment, and compares medians of **per-run average frame times**. Nsight GPU Trace and graphics captures are **instrumented diagnostics**, not FPS baselines; use a GPU-capable Windows/NVIDIA setup and a separate optimized shader-debug build for source correlation.
- CI `.github/workflows/perf_scenarios.yml` validates the Python harness and catalog with no GPU. Locally run `python -m unittest discover -s tools/tests -v` and `python tools/perf_harness.py list`. More detail: `benchmarks/README.md`, `docs/perf_harness.md`, `docs/headless_bench.md`.

## Package inspection and editor regression surfaces

- `tools/pak_browser.py --cli-dump <pak-or-tapp>` reports SMP8 TOC metadata and per-pak `#ids`; `--cli-validate` verifies each stored page's CRC as well as manifest/TOC integrity, without PyQt6 or GPU. The GUI needs PyQt6. Appmanifest v2 `.tapp` files contain only entry information and an ordered `[paks]` pointer; identity records live in the paks. See `docs/pak_vnext.md` and CPU-only `tests/test_pak_browser.py`.
- The editor routes F8 baked irradiance through an inline `WorldRenderer` (one live viewport: active F8, other F8, then viewport 0 Lit), and propagates selection bounds there. `editor_asset_inspect.h` supports typed inspector routing and a 64-KiB read-only UTF-8 fallback for generic files; `editor_asset_browser_nav.h` hides internal cubemap/GI-cook tiles **only in the UI**. See `docs/editor_tour_and_context.md` and corresponding editor regression tests.

## Runtime layers (app/src — the project façade stack)

- **THE .TOUR LOGICAL TARGET LAYER (landed)**: the generic node verbs
  (rename/setAttr/assignMaterial/the flag verbs/parent) and the
  cluster-id form of the typed attr verbs accept `-uuid <hex>` /
  `-name "<name>"` alongside or instead of the bare `-n <cluster id>`
  — replay resolves the Delete chain (uuid exact → unique full name →
  DAG path suffix → the numeric id as the disambiguator), so node
  commands survive re-imports that shift cluster ids. HARD keys: a
  carried key that resolves to nothing FAILS the strict replay (a
  wrong key is an error, never a silent numeric apply); reference
  replay (strict=false) warn-skips. Attr-verb lines take -uuid only
  (their -name token is the payload). Excluded: Delete (own path),
  SetUuid (ASSIGNS the identity), the create verbs (the symbolic -p
  parentRef lane), the -ref create-family forms. Editor leg: CmdToTour
  records the uuid; TourToCmd resolves keys against the live world
  scene; keyless lines keep the numeric path bit-identical. Tests:
  TestLogicalNodeTargets (test_tour.cpp).

- **THE CONTENT REGISTRY** (`engine/include/Engine/Asset/
  content_registry.h`, `tour::asset::ContentRegistry`): the ONE
  content-resolution authority — "an asset has ONE identity, ONE
  effective provider, possibly many available versions". The
  CONVERGENCE CONTRACT: `Read(id)` and `Read(kind, path)` resolve
  through the SAME provider selection (overrides registered first
  win; base = manifest paks in order), so a mod/patch/loose override
  serving a record's path wins for the compiled-game id form AND the
  serialized-path form alike — never the old split where the [ids]
  join read the manifest-named pak while the VFS overlay read
  whichever mount preceded. Byte lanes: an ADOPTED backend's resident
  image first (one copy of the bytes), else the SMP8 pager (light
  mount + one positional page read — never a whole-image preload on
  the read path). Providers toggle by name with a generation bump
  (between-launches mods; loaded resources never mutate). Pinned by
  test_content_registry.cpp (the base+mod+loose acceptance ladder).

- **ProjectRuntime** (the session façade over the registry): owns the
  release pointer (the appmanifest 2 `.tapp` — `entry` + `[paks]`,
  NOTHING else: the identity lives in the paks) + ONE ContentRegistry.
  **ProjectRuntime::Load parses the pointer + DISCOVERS every listed
  pak through its OWN "#ids" identity table** (strict — a listed pak
  without its table fails the launch, recook). `Asset(id)` returns the
  canonical ContentRecord (kind byte + owner pak + logical path + the
  scene-name join); `ReadAsset(id)` / `ReadAssetByPath(kind, path)` /
  `ReadAssetPath(path)` are the convergent byte forms (override-aware);
  `OpenPak`/`AdoptPakBackend` adopt whole-image backends into the
  registry (releasing the provider's pager); `OpenPakPager` is the
  explicit pager accessor. `AddPakOverride`/`AddLooseOverride` register
  mod/dev providers. `AttachCompiledCatalog` (the compiled catalog's
  rows + fingerprint vs the REGISTRY's `RecordFingerprint()` — the
  manifest's `[ids]` mirror + `catalog` line died with appmanifest 2)
  is the compiled-catalog gate project hosts run. **THE DISCOVERY
  LANE**: `OpenDiscovered(pakPaths)` builds the session from PAKS
  ALONE (a legacy blob-less pak serves paths only); `PakNames()` (the
  registry's base providers, both lanes) replaces manifest-paks
  iteration (ProjectScenes' adopt-or-open walks it). RunPakApp's
  `--project` lane AND the viewer's MULTI-PAK lane (several `--pak`,
  or `--pak + --scene <name>` — entry by cooked identity,
  DeployCheck-gated) both build ONE shared session and load through
  the same LoadProjectWorld path; the loose `content/` tree registers
  as an override unless `--no-vfs`. The game element hook receives the
  session (the audio runtime borrows it — no second package
  universe). The build appends the per-pak "#ids" blob post-cook
  (stable slots preserved; the publish digest covers it). The game
  bootstrap contract: `app::AttachCompiledCatalog` (the typed helper —
  layout static_asserts + the pointer cast in ONE audited place, never
  a reinterpret_cast in game code) installs through
  `PakAppConfig::compiledCatalogGate`, which RunPakApp runs FATAL at
  bootstrap (a catalog/deployment mismatch refuses the launch before
  any graphics work); `PakAppConfig::extraElements` (a LIST, in attach
  order, reverse detach) replaced the single `extraElement` slot —
  game elements compose standalone, never nested lifecycle-forwarding
  wrappers (Sample 04: the audio element + the ProjectLit demo beside
  it). **app::RunGame** (`app/src/Runtime.h`, in the apphost lib):
  the application-facing runtime — `app::Context` (the session + the
  world + the scene service), `app::GameBehaviour` (onStart/onUpdate),
  `RunGame(game, argc, argv, catalog...)` (the packaged-manifest
  default entry, RunConfig parse + log, the FATAL typed gate, the
  services under the game element; a thin adapter over RunPakApp —
  the bring-your-own-loop path stays open). **The viewer serves
  authored scene audio** (App.exe installs the audio service on every
  lane; a sound living in an unmounted pak refuses loudly by name).
  **THE OVERLAY IS NO LONGER A CONTENT SOURCE on any runtime
  path**: the anim-clip fallback (the last overlay read —
  AttachClips/PostLoadTail ride `ReadAssetPath` +
  `ContentRegistry::ListPathsUnder`, the ONE enumeration surface:
  the provider union under a prefix, one level, read-only), the
  legacy single-`--pak` lane (its own session via lenient discovery
  + adoption), and `LoadProjectWorld`'s cross-pak resolver all read
  through the session. The world-source callback's vfs stays as the
  ENGINE's protocol surface only (the renderer hands the callback a
  vfs; hosts mount their paks into it — nothing READS it anymore).

- **DeployCheck** (`app/src/DeployCheck.h`): the deployment validation
  gate, pre-graphics in the `--project` launch — every manifest-listed
  pak EXISTS + opens (PakReader magic/CRC) + its CONTENT-ADDRESSED name
  matches the file's ACTUAL bytes, and an ENGINE SHADER SOURCE exists
  (`smvk::shaders::HasAnySource`). A broken deployment fails loudly
  before any Vulkan/window work. Unit-tested in test_pak_host.cpp.

- **ProjectScenes** (docs/project_scenes.md): the scene-INSTANCE layer —
  `LoadScene`/`LoadSceneAdditive`/`RemoveScene` by NUMERIC id over a
  live World (SceneHandle = 64-bit generational), the id→cooked-scene
  join (the paks' identity-table scene rows carry the cooked NAME),
  and the OWNERSHIP-AWARE prefetch (Prefetch/Release pins + the
  per-scene asset registry; `RemoveScene`'s `releasedAssets` reports
  the dropped ids). GPU-less tests drive it through the public
  `clusterLoader` seam.

- **AudioRuntime** (docs/audio_source.md): the scene-aware audio layer
  (an IAppElement) — clip cache keyed by sound id, emitter states keyed
  by the STABLE (cluster, cluster-node) pair through
  `World::clusterMergedNodes`, per-frame distance attenuation +
  constant-power pan, autoplay, scene/spawn lifecycle. The imperative surface
  (PrefetchSound/PlaySound/StopSound/SetAudioSourceVolume/SetListener)
  forwards through its singleton.

- **THE AUDIO RENDERING CORE IS MINIAUDIO, DEVICE-LESS** (vendored
  0.11.26 at `external/miniaudio`; impl TU
  `engine/src/Audio/miniaudio_impl.cpp`, feature macros in
  `engine/src/Audio/miniaudio_config.h` — MA_NO_DEVICE_IO /
  MA_NO_RESOURCE_MANAGER / MA_NO_DECODING): ONE `ma_engine` hosts every
  voice, bus and effect node; the mixer thread renders each chunk via
  `ma_engine_read_pcm_frames` into the SAME SDL3 stream (SDL keeps the
  device, paks keep the content, miniaudio owns the DSP). The legacy
  voice lane (`Play`/`Stop`/`SetVoiceSpatial`) is API-unchanged — each
  voice an `ma_sound` on the endpoint. `Config::deviceless` +
  `PumpOffline` is the deterministic test/bounce lane.

- **AUTHORED AUDIO GRAPHS** (docs/audio_graph.md — the animator
  parallel): a `.audiograph` text asset (tour::asset::AudioGraphDoc —
  node topology + exposed params + ALL of its cues; STRICT parse with
  line numbers, gated at catalog ids, cooker AND runtime) is a
  first-class kind (`PakAssetKind::AudioGraph` = 16, catalog byte 6,
  0x7xxx ids, the FOURTH VFS-addressable kind — authored text
  verbatim). `PakAssetKind`/catalog kinds APPEND, never renumber.
  Instances are ENGINE-owned (`tour::audio::AudioEngine` registries +
  generational `smvk::AudioInstanceHandle` + the
  `tour::audio::AudioInstance` facade: play/playCue/crossFade/stop/
  set/setVolume/playing); gameplay reaches them through
  `app::Context::audio()` (`app::AudioGraphs` — the typed-key create +
  the doc/clip caches over the shared session). Clip resolution is
  CALLER-thread (sounds resolve by VFS path through the registry;
  pak reads + WAV decode never touch the mixer thread). ALL graph
  mutations + param scheduling happen at CHUNK boundaries on the
  mixer thread; the whole node set + every (source, cue) voice is
  pre-built at create (the one allocation burst — steady state is
  alloc-free); automation evaluates AT the chunk cadence (immune to
  frame stalls). The generated project.h emits
  `project::audiographs::*` AudioGraphKeys PLUS per-cue constants
  (`project::audiocues::<graph>_<cue>` bundling (graph id, cue
  ordinal), outside the catalog ABI). v1: no in-graph spatialization,
  no graph editor yet.
  Pinned by audiograph_tests + `audiograph.*` in
  project_pipeline_tests + the sample + project_e2e_tests.

- **THE SCHROEDER REVERB** (`engine/src/Audio/schroeder_reverb.
  {h,cpp}` — the ONE custom miniaudio `ma_node`): four damped
  feedback combs + two all-pass diffusers per channel, stereo
  decorrelated, MIX mode (the authored-graph `reverb` node —
  `AudioNodeType::Reverb = 11`, params wet/rt60/damping appended,
  wet 0 = an EXACT bypass) and WET-ONLY mode (the environmental
  send). Continuous processing drains tails through input silence;
  coefficients (RT60 feedback, damping, wet) smooth INSIDE the
  callback; delay lengths fixed for the node's lifetime; bounded
  rt60 0.15..8 s at every gate.

- **AUDIO ROOMS — the environmental reverb volumes** (docs/
  audio_graph.md): `SceneAudioRoom` (a node-local payload:
  priority/fades/wet/rt60/damping/transition; INDEPENDENT of GI — a
  node may carry BOTH a GiVolumeRow and an AudioRoomRow, or a
  room only) + the shared OBB utility `Engine/Scene/
  spatial_volume.h` (the GI placement convention: center =
  translation, half-extents = HALF the column lengths; parity with
  the GI planes pinned by tests). The `.tour` lane: `createNode -at
  audioRoom`, `attachAudioRoom`/`removeAudioRoom` (`-ref k -owner
  givolume` targets a created GI volume — one placement, two
  associations), `setAudioRoomAttr` (properties + the givolume-lane
  position/rotation/size shape channels);
  `kCreateAudioRoomRefBase = 0x01000000`. The EDITOR lane is full
  (Create menu, the inspector's Enable button + field drags, teal
  box gizmos + pick, undo/redo, duplicate/delete capture, the
  family-ref drop guards). The COOKED lane: the scene blob's v20
  audio-room section (appended after v19 — the sanctioned break).
  The RUNTIME lane: the ENVIRONMENTAL WORLD-SFX BUS (worldSfxGroup →
  splitter → {dry → endpoint, wet-only Schroeder → wetGain →
  endpoint}); `Play(..., AudioRoute)` routes spatial scene emitters
  through it (default Direct — the compat contract; a failed bus
  build degrades to direct, logged, never fatal); AudioRuntime
  evaluates the listener's ordered-coverage blend over the durable
  cluster rows each frame (descending audio priority, the outdoor
  baseline for the leftover coverage) and pushes ONE
  change-detected `SetEnvironmentReverb` — all smoothing is
  mixer-thread-side. MVP: listener-side reverb ONLY — no occlusion,
  portals or per-room IRs.

- **THE PER-VOICE CURSOR RULE** (the shared-buffer trap,
  docs/audio_graph.md): a clip's decoded bytes are wrapped by
  PER-VOICE `ma_audio_buffer_ref` cursors — NEVER a shared
  `ma_audio_buffer` (one buffer = one cursor: the first voice to
  play a clip exhausts it and every later voice reads silence;
  simultaneous voices race the position). Pinned by
  `ag.bus.raw-cursor-trap`/`-fix` + the `ag.bus.*-rings`
  regression pins.

- **Sound assets are a first-class kind**: `PakAssetKind::Sound` (raw
  .wav at the VFS path), 0x4xxx content ids (scenes 0x1xxx / textures
  0x2xxx / meshes 0x3xxx / sounds 0x4xxx / audiographs 0x7xxx), and
  the cooked scene's v18 audio section stores NUMERIC sound ids (the
  pak cooker's `--sound-id` map; a non-empty map with an unresolved
  referenced path FAILS the cook). The kinds gate on the SHARED
  catalog kind bytes.

- **THE PAK CONTAINER IS SMP8** (the paged break — docs/pak_vnext.md is
  the format reference): per-entry `contentCrc` verified LAZILY at
  `Payload()` fetch (a corrupt page throws AT the page, neighbors stay
  servable), the footer CRC covers ONLY the TOC (`VerifyAll()` is the
  explicit full policy), STABLE per-entry `slot` ordinals (the
  compiled-plan addressing), `PakReader::OpenFile` (header/footer/TOC-
  only open with positional page reads — the async pager's primitive),
  and the cooked MESH PLAN (`PakAssetKind::Plan` blob at `"#meshplan"`:
  the family→member join; the runtime join is a plan lookup, NEVER a
  TOC string scan — a missing plan/family = version skew = loud
  recook failure). Every SMP7-or-older pak fails closed at the magic
  (content recooks from zero — standing policy, no compat reader).
  Also: the per-pak `"#schemas"` registry blob, and
  Cluster::PromoteDeferredMeshLodFromPage reads promote bytes from the
  entry's own file range (see docs/pak_vnext.md).

- **THE ASYNC PAK PAGER** (`engine/Asset/pak_pager.h` — docs/pak_vnext.md):
  ONE worker thread over a persistent handle, positional page reads
  outside the pool lock, demand-reads-jump-prefetch (prefetch is NEVER a
  guarantee — the later Read is the contract), in-flight/resident DEDUP,
  bounded-byte-budget LRU pool evicting UNPINNED pages only, loud
  terminal per-page failures. THE SELECTIVE RUNTIME LANE:
  `ProjectRuntime::ReadAsset` serves through the pager whenever the pak
  has no backend in the cache (one copy of the bytes whatever opened
  first). The SCENE lane stays on the whole-image preload (the
  direct-addressed mesh upload needs a contiguous base) — documented in
  pak_vnext.md.

- **THE COMPILED STATIC PLANS** (`engine/Asset/asset_plan.h` —
  docs/pak_vnext.md): the shipping hot path loads meshes with NO runtime
  name search, path normalization, or TOC walk — and FAILS CLOSED on
  layout mismatch. `smvk::MeshPlanRef` (pak mount index + family base
  pak slot + .geometry identity); the PRIVATE generated
  `project_plans.h` (never the game-facing project.h); the LAYOUT HASH
  (ONE shared engine fold, CONTENT EXCLUDED — a payload-only redeploy
  needs no recompile); the gate (`AttachCompiledPlans`; count or fold
  mismatch = LOUD abort, never a dynamic fallback); the plan load lane
  (`LoadMeshByPlan` — slot validated through the O(1)
  `PakReader::FindBySlot` index).

- **GEOMETRY STAGE 6 — THE PAGER LOD PROMOTE** (docs/pak_vnext.md):
  `Cluster::PromoteDeferredMeshLodFromPage` promotes deferred siblings
  from their ENTRY'S OWN BYTES through the pager (every range must FIT
  the page) with the promote tail SHARED with the image-pin lane.
  `smvk::SetLodPageSource` (geometry_streamer.h) is the host-installed
  seam — `World::PromoteLodLevel` consults it FIRST. Verified on sponza
  with budget-driven churn. Stage 6 (disk-range streaming) remains the
  noted TODO in geometry_streamer.h.

- **PROJECT-DEFINED NODE SCHEMAS** (`engine/Scene/node_schema.h`):
  typed DATA associations on regular scene transform nodes, authored as
  `.nodeschema` text files; the field identity is the DECLARATION
  ORDINAL ("COMPILE TYPES; COOK VALUES": the engine stores cooked value
  bytes; schema layouts ride the per-pak registry; the GENERATED
  `project::types` PODs own the decode — generated C++ layout EQUALS
  cooked layout, static_asserts on every offset; str fields are
  layout-TERMINAL). An edit/rename/reorder is a generation event
  (renames are identity-free, NOT recompile-free). Pinned across
  test_assets.cpp / test_world_entities.cpp / test_project_pipeline.cpp
  (`schema.*`/`schemafold.*`/`schemagen.*`).

- **The gameplay-handle model** (engine Engine/Scene/entity_handle.h):
  NO generic game-entity registry — gameplay structs compose TYPED
  64-bit generational handles (MeshEntityHandle/AnimatorHandle/
  AudioSourceHandle); the generated project.h carries TYPED KEYS
  (SceneKey/TextureKey/MeshKey/SoundKey — which ASSET; handles name
  which INSTANCE). The full mesh-entity + animator surface has landed
  (docs/entities_and_animators.md): World::SpawnMeshFromNode/
  DestroyMesh, World::AddAnimator + the Animator facade, the
  World::Update flush through Cluster::poseExternallyDriven (driven by
  the frame's dt, NEVER a second caller). Renderer limit: one POSE per
  cluster (independent STATE for all animators); the per-draw palette
  table + the LoadSkeleton/LoadAnimation asset surface are the
  documented follow-ups; the typed-key MESH load/spawn lane is LANDED
  (AssetManager::LoadMesh + SpawnMesh).

## The namespace boundary invariant (tour:: engine / smvk:: RHI)

**The ownership rule (todo item 8, the standing architecture target):**

- **`tour::`** — the ENGINE API: app, scene/world/cluster, assets, paks,
  audio, GI, placement, camera, mem, profiler, renderers' engine-side
  policy, the shader library, the streamers.
- **`smvk::`** — the RHI ONLY: resources (Texture/Image/Buffer handles,
  Holder/Handle), command buffers, swapchain, rendergraph recording,
  synchronization, descriptor limits.
- **`smvk::detail::vulkan`** — the private Vulkan implementation
  (public headers never include it).

The engine is ALLOWED to call `smvk::` RHI operations — spell them
qualified (`smvk::TextureHandle`, `smvk::ResourceManager&`). The
VIOLATION is the reverse ownership direction: engine policy, engine
assets or engine services declared inside `namespace smvk`, or the RHI
including/referencing engine-domain headers.

**Rules for new + touched code:**

1. New engine-owned types go in a `tour::` namespace (flat `tour::` for
   the core families, `tour::shaders` / `tour::gfx` / `tour::asset` /
   `tour::gi` / `tour::placement` / `tour::mem` / `tour::audio` style
   for subsystems). NEVER declare engine concepts in `namespace smvk`.
2. NEVER add `using namespace ::smvk;` to a public header (R6 bans it
   with an EMPTY exception list - zero broad imports remain). In moved
   headers, QUALIFY the RHI types — the engine's public headers are all
   fully qualified now (the recorded lessons: a broad import's
   transitive lookup also collides same-named types — the
   `tour::gfx::Cluster` vs `tour::Cluster` ambiguity that decided
   gfx_filter.h; and a directive in one placement header silently
   covers every sibling in the same enclosing namespace — the
   placement_pass.h lean). TU-LOCAL using-directives in .cpp files
   (impl.cpp, pak.cpp, mesh.cpp, gfx_filter.cpp) are the contained
   escape hatch while a moved block still spells RHI names
   unqualified.
3. Migrations follow the landed pattern: declare in `tour::`, add the
   `smvk::` compat re-export (`using ::tour::X;`, or a namespace alias
   `namespace X = ::tour::Y;` for whole families), keep consumers
   compiling, migrate call sites opportunistically, then RETIRE the
   alias. Adding a compat re-export = a deliberate R7 baseline row in
   the same commit.
4. Legacy `smvk::` spellings of MIGRATED types (`smvk::World`,
   `smvk::shaders::*`, `smvk::TextureStreamer`, `smvk::gfx::*`,
   `smvk::MaterialValues`, ...) keep resolving through the R7-baselined
   compat aliases — they are TRANSITIONAL, not the canonical surface.
   New code writes the `tour::` spelling.

**The CI leak alarm — `tests/test_architecture.cpp` (R1–R9):** the
namespace/RHI boundary scanner, GPU-free + source-tree-only, runs in
every ctest/CI run. R1–R5: no Vulkan includes / private-impl includes /
Vk types / upward RHI includes (direct + transitive, quoted forms
included). R6–R9 are the SHRINK-ONLY BASELINE gates that make the audit
findings impossible to grow silently:

- **R6** — NO broad `using namespace ::smvk` import in ANY public header,
  with an EMPTY exception list (item 1 of the remaining-changes list
  landed: the last ten migration-stage imports were removed, their RHI
  names explicitly qualified; the impl TUs that still spell RHI names
  unqualified carry the documented TU-LOCAL using-directive escape
  hatch — impl.cpp, pak.cpp). Any new public-header import fails
  outright.
- **R7** — the `smvk::`-compat re-export blocks are confined to exact
  per-file counts: a new compat alias anywhere, or a grown one, FAILS.
- **R8** — RHI implementation-state exposure in engine-facing headers
  (`GetResourceManager`/`GetSwapchain`/`GetRendergraph`,
  `LinearPtr<Swapchain>` members, `ElementFrame`'s `Framebuffer*`,
  `gfx::State`, raw `ResourceManager*`/`Swapchain*`/`RenderGraph*`/
  `Framebuffer*` pointer types) is frozen per (file, token).
- **R9** — the GLOBAL-NAMESPACE gate (the global-namespace cleanup is
  complete: no engine type is declared at global scope anymore —
  NodeId/scene/Camera/Input/BVH/DXC all live in tour::(scene|dxc|...))
  with the transitional GLOBAL compat re-export bridges (`using
  tour::scene::Scene;` etc. in input.h/scene.h/scene_tables.h/
  camera.h/bvh.h...) confined to exact per-file counts, and any NEW
  global struct/class/enum declaration failing outright.

The baselines are EXACT in both directions: growth or a new site fails
the suite, and RETIREMENT also fails until the baseline row is deleted
in the same commit — the exception list can only shrink, explicitly.
When you retire compat surface, shrink the matching row; when you
believe you need new exposure or a new alias, add the row with a
comment and let the review happen at the gate. The full AST
signature-walking scanner remains the follow-up (todo 8.8); the regex
battery is the enforceable v1.

## Build system

**Always configure and build with the Ninja generator.**

```powershell
# Configure (Release)
cmake -B out/build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=Release .

# Build
cmake --build out/build/x64-Release --target App

# Configure (Debug)
cmake -B out/build/x64-Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug .
cmake --build out/build/x64-Debug --target App
```

Never use the Visual Studio generator — the build directories in
`out/build/` are Ninja-configured, and mixing generators fails with
"generator : Ninja does not match the generator used previously". If a
`CMakeCache.txt` mismatch ever appears, delete the offending
`out/build/<config>` directory and reconfigure with Ninja.

MSVC environment: build from the "Developer PowerShell for Visual
Studio" (already initialized), or use `build.bat` (reuses an initialized
shell, else finds VS via vswhere and calls its vcvarsall). From any
other plain shell, call YOUR install's vcvarsall first — no path is
universal, adjust the version/edition to the machine:

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul && cmake --build out/build/x64-Release --target App"
```

## Systems and seams (extend via the seams, not the god objects)

God objects are INTENTIONAL (owner decision): `WorldRenderer.cpp`
(~14k lines) and `Vulkan/impl.cpp` (~12k lines) stay whole. Do not
propose splitting them again.

| System | Key files | Seams / contracts |
|---|---|---|
| App shell | `engine/App/app.h`, `app/src/Host.h+PakApp*.cpp` | `WorldRendererConfig::All/Minimal/FilterOnly` + `--with/--without` CSV; `SetWorldSource` injects content (engine never knows paks) |
| Frame renderer | `engine/Renderer/WorldRenderer.h` | `Configure(passes, run)` (parse-once `RunConfig`); `IFrameRenderer::AppendPasses(AppFrameContext)` |
| Run config | `engine/App/RunConfig.h+.cpp` | `Parse()` once in `main`, `Log()`, `WarnUnknown()`; new knobs go here first |
| Rendergraph | `engine/Rendergraph/*.h`, impl in `engine/src/Vulkan/impl.cpp` | `Add*Hints` barrier discipline; `RendergraphContractTests` |
| GPU scene | `engine/App/world.h+cluster.h`, `engine/Scene/scene.h` | `ResolveAndFixup_clusters` rebuilds `drawItems/flagLists/lodChains` |
| Tour | `engine/Asset/tour.h+.cpp`, `editor/src/editor_commands.cpp` | `CmdToTour`/`TourToCmd`/`ApplyTourToCluster`/`ReplayTourDoc`; `TestTourFuzz` |
| Memory | `engine/Memory/memory.h+linear_vector.h` | `FrameVector` = FrameScratch (reset/frame); `UploadInFlightVector` = monotonic (never reset while live); `ProcessVector` = process (ThreadPool ring) |
| GPU-driven filter | `engine/Renderer/gfx_filter.h` | filtered main view ALWAYS ON (no runtime off; the direct leg stays as the unfilterable/LOD-far fallback); `m_useGfxMesh`/`GpuCullEnabled` stay setup-time; `MeshDrawEligible` truth table in contract tests |
| Goldens | `tools/golden_capture.py`, `tools/golden_hashes.txt` | `--update` records, a mismatch fails the run — LOCAL/dev-GPU tooling (per-GPU baselines; CI never runs it) |

Data flow per frame: `DrawFrame` fills `AppFrameContext` → `GfxRenderer`
(seed/HZB/filter) → `ForwardPlus` (prepass+cull) → shadow atlases
(CSM/spot/EVSM/deferred) → `Main` (mesh leg first, indexed skips
eligible ranges: exactly-once) → motion/DoF/SSR/volumetric/exposure/
bloom/tonemap/AA/UI → swapchain.

## The generated project.h (content as code)

- **Project identity (persistent, never name/path-derived)**: the
  `project.tourproj` marker carries a `guid=` row (format version 2)
  minted ONCE at project creation; `project::ProjectId` derives from
  that GUID. Current-format policy — NO v1 marker reader; a marker
  without a valid guid is rejected by `Project::Open`, the editor's
  `OpenProject` and the launcher's scaffold; `Project.exe init` on an
  existing project is the ONE explicit adopt-and-mint recovery path.
- `project.h` (`cmake/TourmalineProject.cmake`) defaults to the
  project SOURCE directory, is registered under the CMake target's
  `Generated Catalogs` group with GENERATED / HEADER_FILE_ONLY, and
  can be committed to Git. Never hand-edit it: `<Name>_ids` runs on
  every game build and overwrites modifications before compilation,
  keeping mtime intact when no change is required. Do not set an OS
  read-only attribute: it would obstruct Git checkout and generation.
  The private sibling `project_plans.h`
  stays ignored; explicit `HEADER_DIR` can select build-only output.
  For navigable game includes, prefer a source-relative spelling like
  `#include "project/project.h"` (sample's `main.cpp`), not bare
  `#include "project.h"`: `tourmaline_attach_project` adds both the
  header folder and its parent to the consuming target's include path
  so Visual Studio F12 and the compiler resolve the same file.
  The header is a GENERATION-STAMPED,
  self-contained C++ catalog: `project::Generation` / `GeneratedAt` /
  `ContentHash` constexpr plus an EOF machine-readable COMMENT section
  (`TOURMALINE-METADATA`) carrying per-asset/per-pak fingerprints
  (mtime+size+fnv1a64 + pak cook-state). Never code against the EOF
  section; it is tool data. The fingerprints are VERIFIED CONTENT
  HASHES recomputed from bytes — matching size + mtime is NEVER proof
  of unchanged content.
- The stamp moves ONLY on a generation EVENT (any change to the emitted
  text, or a forced refresh); an unchanged generation re-emits
  byte-identical text. Tabular output (`Paks`, `Assets`) is
  width-aligned by the longest emitted cell in each column, as
  are `AssetKind` enumerators; typed
  namespaces, audio cues and retired declarations align their value
  braces. One-column collections need no alignment. Never reorder
  rows or alter literals in a formatting-only change.
  `<Name>_stamp` (`--stamp`) forces a new
  generation every run; `<Name>_paks` cooks content WITHOUT building
  C++ and re-stamps the header (its pak records go `state=verified`; a
  catalog-only refresh can never upgrade a state, only preserve or
  demote). ContentHash covers catalog SEMANTICS only. The three
  operations are independent: `--target <Name>_ids` / `_stamp` / `_paks`
  / `<Game>`. Unit-tested in test_project_pipeline.cpp (`stamp.*`,
  `identity.*`, `proj.*`).
- **`Project.exe explain`**: the provenance/stale-causes CLI — one
  asset's identity, ownership (the pak + declared dependencies in the
  topological plan order; cycles/missing deps fail loud), freshness
  (VERIFIED source hash vs the last EOF record) and deployment (the
  published content-addressed file + cook state). Retired identities
  report the tombstone reservation + restore path. Smoke-tested
  (`catalog.explain-*`).
- **The retired asset-ID/tombstone contract**: deleted assets TOMBSTONE
  in assets.db (active=0 — id+symbol stay reserved FOREVER, never
  reused; a new asset at the same path gets a fresh id +
  collision-suffixed symbol). The generated header emits the RETIRED
  table — historical TYPED symbols with values preserved +
  `IsRetired`/`IsActive` compile-time membership queries; retired
  entries are excluded from the active namespaces/Assets array/
  published manifests and a runtime load of a retired id fails
  explicitly. RESTORING the original asset GUID re-activates the SAME
  id+symbol (the identity is the GUID, never the path). Pinned by
  `catalog.retired-*` + `catalog.reactivate-*`.

## Source identity + the file watcher (the watched-resource pipeline)

- **The mesh-source formats (the import lane)**:
  `LoadModelFromSourceFile` (gltf_import) is the ONE loader for every
  model source: .gltf (ASCII) and .glb (detected by MAGIC, never
  extension) through tinygltf, .obj through the tinyobjloader adapter
  (`obj_import.cpp`, builds the tinygltf interchange form in memory;
  its .mtl libraries are digest/staging dependencies via extraDeps).
  EMBEDDED textures (GLB bufferView images, glTF data: URIs) EXTRACT
  to `<stem>_<srcext>_img<N>.<ext>` sidecar files beside the source
  (write-if-changed; a failed write fails the import LOUDLY) — the
  whole downstream (texture sources, .material TexRefs, project
  staging, the content digest) addresses them like authored files.
  The shared extension gate is `source_formats.h`
  (`IsMeshSourceFile` / `IsMeshSourceMetaSidecar` — the CLI, the
  project pipelines, the catalog/recipe resolver and the editor's
  open/drop/reference/watch surfaces all funnel through it; .fbx/.dae/
  .usd have NO adapter yet). Pinned by `import.*` in
  test_source_import.cpp.
- **The glTF identity ledger**: the `.gltf.meta` manifest is the
  family's identity LEDGER, separate from source-byte fingerprints.
  Family guid comes from the caller's explicit resolution, the previous
  manifest (identity CARRIES across content edits), or the
  deterministic mint. Subasset identities (materials/anims/geometry)
  carry BY NAME — a DCC re-export that reorders glTF arrays keeps every
  GUID; a name-group COUNT change is AMBIGUOUS and the import FAILS
  (removed names retire). A manifest that exists but does not parse
  refuses the derive — identity repair is explicit. Pinned by
  `identity.*` in test_project_pipeline.cpp.
- **The project-scoped native filesystem watcher**
  (`Engine/Utils/file_watcher.h`): recursive dirs + single files,
  ReadDirectoryChangesW/IOCP on Windows, inotify on Linux. POLICY
  (ProjectDefault) excludes generated artifacts/intermediates/published
  products whatever the directory (`.geometry/.material/.anim`, `.spv`,
  `.pak/.tapp`, staging/`out`/`build`/`paks` trees, generated
  `project.h`, atomic-save temps) — only authoritative sources produce
  events (`.meta` sidecars INCLUDING the `.gltf.meta` ledger ARE
  watched). Changes settle after a quiet window + readability probe;
  `Overflowed()` reports lost OS events; poll() once per frame — never
  a per-frame scan/hash of the project. Unit-tested in
  test_file_watcher.cpp.
- **The editor's watched selective refresh**: `EditorApp` arms the
  watcher at `OpenProject` (`ArmProjectWatcher`; `TOURMALINE_EDITOR_WATCH=0`
  disables) and drains settled changes at the frame boundary
  (`UpdateWysiwygBoundary`), routing through EXISTING selective
  funnels — `.gltf/.glb` → ImportMeshArtifactFor + the GEOMETRY RE-OPEN
  FLOW (ReloadReferencesViaTemp rebinds through the temp-tour snapshot:
  unsaved edits, cameras, selection + dirty flag survive; a referenced
  model keeps the reference-editor reload), `.meta` → asset-db rescan,
  `.tour/.tourrecipe` → LOG ONLY (external edits never silently
  overwrite unsaved editor work — reload to apply), an image BESIDE its
  `.texture` asset → the LIVE TEXTURE REFRESH (force lane lands fresh
  pixels under the SAME uri; old pool row destroyed with the image
  release DEFERRED behind in-flight frames; `RewireTextureSlots` moves
  every material slot off the old GLOBAL RHI index; NO reload, NO
  relaunch), other kinds → the log. The routing table is the pure
  classifier in `editor/src/editor_watch_routes.h` (pinned by
  `watchroute.*`); the force lane by `texforce.*`.

## The content-as-code query + loading + GI-blend API

**docs/content_as_code.md** is the system reference.

- **THE COMPILE-TIME QUERY** (`engine/include/Engine/Asset/asset_query.h`
  + the generated project.h binding): `project::MatchAssets<Key,
  "regex">()` — CONSTEVAL regex over the catalog's SYMBOL names (or
  content PATHS via `MatchField::Path`), kind-filtered by
  `AssetTraits<Key>`, returning a precisely sized `std::array<Key, N>`
  in catalog order. FULL-MATCH semantics; malformed pattern = a
  COMPILE-TIME error; bounded-AST Thompson/Pike evaluation, linear, no
  backtracking. The generated metadata sits OUTSIDE the runtime catalog
  ABI (CatalogFingerprint unchanged). Pinned by test_asset_query.cpp
  (`qrx.*`) + the emitter shapes.
- **THE GI CATALOG LANE**: `GiKey` (0x6xxx; a `.givolumemanifest` = a
  baked GI SET naming its `.givolume` payload rows). The kind byte is
  APPENDED (the ABI never renumbers); spans resolver→IdRangeForKind→
  kind gate/strings→fingerprint folds→emitted namespace (`project::gi::*`)
  →the paks' identity tables→`ReadAsset`. Pinned by `catalog.gi-*`.
- **THE ASSET MANAGER FACADE** (`app/src/AssetManager.h/.cpp`):
  `LoadMesh(MeshKey)`/`LoadMeshes(ptr, count)` (raw pointer + count;
  nullptr valid ONLY with count 0; per-key dedup; per-pak grouping
  acquires each owning carrier once; `MeshWorldIndex` resolves a
  handle's CURRENT world row FRESH per call, UINT32_MAX the honest
  no-mapping answer) + `LoadGI(GiKey)` + `SetGI`/`SetGIBrand(from, to,
  alpha)`/`UnloadGI` (generational handles; multiple GI sets stay
  resident). The GAME owns all blend timing — NO engine-side
  durations/easing/timers by design. Pinned by `am.*` in
  test_pak_host.cpp.
- **THE ASSET STORE** (`app/src/AssetStore.h/.cpp` — the shared-resource
  OWNERSHIP layer, docs/content_as_code.md): the found-or-loaded
  mesh-family carrier acquire with ASSET-SIDE refs
  (`Cluster::assetRefs` + `World::RetainCluster`). Scene removal never
  destroys an independently held asset: `World::UnloadCluster` PARKS a
  carrier with live refs (alive in `retainedClusters` until the last
  `ReleaseCluster`); a re-load shares the parked carrier. `keepResident`
  is the sticky SESSION ref released at `UnloadAllMeshes`/teardown. The
  ownership is the STORE's — ProjectScenes knows nothing about who holds
  assets. Teardown order: the store dies BEFORE its world. Pinned by
  `am.mesh-*` + `wretain.*`.
- **THE SHARED GEOMETRY UPLOAD SERVICE**
  (`engine/include/Engine/Renderer/geometry_upload.h/.cpp`): the carve +
  all-or-nothing rollback + table-driven bulk-copy core (extracted from
  `Cluster::UploadStaticMeshesDirect` — that keeps the
  cluster-bookkeeping side). `UploadStaticGeometry` returns CALLER-OWNED
  arena tokens + pool-independent `StaticMesh` values — the ONE upload
  seam the shared mesh store + pager submit into, never a duplicate
  uploader.
- **THE RENDERER GI-BLEND SEAM**: `WorldRenderer::BuildGiVolumeSetFromPaths`
  (transactional build core over an explicit path list),
  `AdoptGiVolumeSet`/`AdoptGiVolumeBlend`/`SetGiBlendAlpha`/
  `ClearGiBlend` (the hot-swap publish pattern; a scene load resets any
  external set). FrameUniforms carries the B-set tables in an
  APPEND-ONLY tail (sizes pinned by static_asserts + the HLSL mirror in
  common.hlsli). The shader twins evaluate BOTH sets independently and
  interpolate the RESULT — the A-set hot paths pay zero cost when
  blending is off. The B set carries no tile masks (the cull hoist walks
  set A) and no probe spheres.
- Documented follow-ups: the subset/pak-grouped mesh fast path (the
  per-mesh blob direct upload), the page-pager + permanent shared-mesh
  store, the dedicated two-bake-set GPU rendering regression.

## Submodules

Unmodified upstream deps are pinned git submodules under `external/`
(nanosvg, volk, SPIRV-Reflect, glm, meshoptimizer, DirectXTex, json,
JoltPhysics). After cloning or pulling:
`git submodule update --init --recursive` (or clone with `--recursive`).
The rest of `external/` is vendored content (trimmed/patched trees,
binary packages — see `THIRD_PARTY_NOTICES.md`).

## Shaders

- Source: `engine/shaders/` (HLSL, compiled by DXC via
  `cmake/CompileShaders.cmake`); Output:
  `out/build/<config>/content/shaders/*.spv`, synced next to `App.exe`.
  Editing a shader: rebuild the `App` target — the EngineScene_shaders
  dependency recompiles changed `.hlsl` and syncs the `.spv`.
- HLSL entry points: `mainVS`/`mainPS`/`mainCS` by file extension
  (`.vert.hlsl`/`.frag.hlsl`/`.cs.hlsl`).
- Debug builds compile shaders `-Od`; files listed in
  `HLSL_FORCE_O3_FILES` (`cmake/CompileShaders.cmake`, currently
  `gfxfilter.cs.hlsl`) are pinned to `-O3` even in Debug.

### Shaders as packaged assets (the engine shader library)

- `PakAssetKind::Shader` (kind 12) stores compiled bytecode VERBATIM as
  a blob keyed by its VFS path (`content/shaders/<file>.spv`); the THIRD
  VFS-addressable kind.
- `tour::shaders` (`Engine/Utils/shader_library.h`; `smvk::shaders` is
  the R7-baselined compat alias) is THE unified
  shader reader: one process-wide overlay mounting (in precedence) loose
  DEV trees (the exe-relative `content/shaders/` the build syncs) over
  SHADER PACKAGES (`MountPak` — the production source is the
  exe-relative `engine-shaders.pak`). Every read VALIDATES the bytecode
  magic (SPIR-V on the Vulkan backend) — a corrupt blob fails at READ
  time with a named provider, never inside a driver. `Read(path)` is a
  drop-in for the old `readFile(...)` reads. Hosts call
  `tour::shaders::Init()` before graphics initialization.
- `engine-shaders.pak` (the ENGINE-OWNED package) is built by
  `EngineScene_shaderpak` (`tools/pak_cooker/CMakeLists.txt`) from the
  AUTHORITATIVE DECLARED compiled-output list (`engine-shaders.list`)
  — every configured variant, whatever the build produced; a retired
  variant's stale `.spv` in an old build tree NEVER ships. Hosts deploy
  the package next to their exe (`HostSetup`) — a MANDATORY runtime
  dependency of every graphics host, built independent of any scene or
  project recipe.
- Regression-tested in test_shader_library.cpp.
- **PROJECT-AUTHORED shader assets** are a separate identity lane: the
  recipe resolver classifies `.spv` under the project's own content tree
  as `PlanAssetKind::Shader`; asset-only paks embed them VERBATIM as
  Shader-kind blobs (SPIR-V magic validated at pack time); the content-id
  range is **0x5xxx**; the generated project.h carries
  `project::shaders::*` typed `ShaderKey` constants + `AssetKind::Shader`
  = 4 (the ABI kind byte — appended, nothing renumbered); the paks'
  identity tables carry the kind byte; `ProjectRuntime::ReadAsset`
  serves the raw bytecode. The ENGINE-NATIVE set is build-owned —
  recipes never control its inclusion. Unit-tested in
  test_project_pipeline.cpp (`shaders.*`).

### The watched shader hot-reload service (dev)

- `smvk::shader_reload` (`Engine/Utils/shader_hot_reload.h`) is the
  source→bytes leg: a FileWatcher over `engine/shaders/` (*.hlsl +
  *.hlsli), settled edits recompiled IN-PROCESS through the DXC
  integration (captured diagnostics), replacement bytecode VALIDATED,
  `.spv` written ATOMICALLY into the dev override tree.
  LAST-KNOWN-GOOD contract: a failed compile NEVER touches the previous
  `.spv`; byte-identical recompile skips the write (no mtime churn).
  Invalidation: a .hlsl edit recompiles that file's BASE output + its
  VARIANT TWINS (`VariantsOf`); a .hlsli edit recompiles EVERYTHING.
  Arming: `TOURMALINE_SHADER_RELOAD=1` (App + Editor, dev only;
  `TOURMALINE_SHADER_RELOAD_SRC` overrides the source tree).
- **THE SWAP CONTRACT (the consumer leg, LANDED — both legs)**: Vulkan
  modules are immutable — a consumer rebuilds its module+pipeline set
  at the frame boundary (between submits, the GPU-safe point) and
  retires old handles behind the in-flight frames
  (`VulkanPipelineCache::RetireModule` evicts cached entries + defers
  destroys behind in-flight frames). The LIT-FAMILY leg —
  `WorldRenderer::LiveSwapCovers` (default.vert + every default_lit
  permutation + the ProjectLit family + the GPU-driven legs) runs
  `ReloadLitShaders` (fresh-first re-reads through the shader library,
  per-module last-known-good, release-into-retire; programs re-forged,
  derived twins invalidated). The PER-BUILDER leg —
  `ReloadBuilderShaders`, UNCONDITIONAL on every change set — swaps
  every rendergraph builder's modules via `ShaderSwap.h` helpers
  (bloom, tonemap, forwardplus, shadows, motion, DOF, SSR, SSGI,
  volumetric, exposure, godrays, contact shadows, particles, SMAA, UI
  composite, skydome, terrain, skinning, probe visuals + pre-mix).
  STILL last-known-good (the documented gap): the gfx compute chain
  (gfxfilter/gfxbatchcull/hzb) + WorldRenderer's auxiliary FS sets.
  Frame-safety regression: `TOURMALINE_SHADER_SWAP=<frame>` forces the
  FULL swap mid-bench (verified: differing pixels 0.00% vs the no-swap
  reference). Pinned by `swap.cover-*`.

### The lit-hook seam + the 32-bit userdata lane (Phase A landed)

docs/shader_library_design.md is the design record.

- **The seam**: `engine/shaders/lit_hooks_api.hlsli` (typed
  `TMLitInput`/`TMSurface` + prototypes) + `lit_hooks_default.hlsli`
  (the StandardLit no-ops). The three lit entry wrappers include the
  hook family BEFORE `default_lit_body.hlsli`; the body calls
  `TM_ModifySurface` after decals (the wetness home; metallic/roughness
  re-clamp + non-finite guard as CONTRACT) and `TM_ModifyLitColor` after
  fog (the damage-flash home; outAoTerm/outEnvSpec/alpha untouched).
  Unlit + debug early returns deliberately skip the hooks. A wrapper
  shipping no hook implementation FAILS the DXC compile loudly.
- **The value**: ONE resolved `uint32_t` per draw — the material
  INSTANCE override or the base material default. World owns the lane:
  `SetMaterialUserData` (base defaults), material instances
  (`CreateMaterialInstance` / `SetMeshMaterialInstance` /
  `SetMaterialInstanceUserData` / `ClearMaterialInstanceUserDataOverride`
  / `DestroyMaterialInstance` — generational handles; the shared base
  material row/textures/batching NEVER touched), and
  `ResolvedItemUserData(item)` for the draw producers. Value changes
  are plain CPU pool writes: no recompile, no reupload, no batch split
  (the material stays the sort key — pinned by
  `gfx.userdata.no-batch-split`).
- **The transport** (all three lit producer paths, pinned at VS location
  9): the gfx Draw ring (96→112 bytes, `Draw::userData` / HLSL
  `GfxDraw.misc3.x`) → gfxfiltered.vert + gfxfilter.mesh forward it
  flat; the direct legs push it at push-constant byte 244
  (`padEnd_[1]`, the always-per-draw discipline — the filtered motion
  pass owns 240..256 as its own tail overlay, so the two never
  conflict) → default.vert forwards it. The shadow/seed legs copy the
  word for ring consistency but never read it (the hooks are
  color-only in v1 — NO coverage parity, no dissolve hooks).
- **drawItems linkage**: `RenderItem::materialInstance` carries the
  bound instance's HANDLE VALUE (generation-checked at resolve —
  destroyed/slot-reused instances fall back to the base default). The
  entity mirror loop runs BEFORE the flags/drawItems build (pinned by
  `matinst.entity-items-in-drawitems`).
- Tests: `gfx.userdata.*` (test_gfxfilter.cpp), `matinst.*`
  (test_world_entities.cpp), the stride/tail/padEnd pins
  (test_rendergraph_contract.cpp).

### The ProjectLit family (Phase B landed)

The project-authored lit FS family — a project's hook implementation
compiled against the SAME shared lit body and selected per base
material. The seam + transport + instances above are the family-neutral
base; this is the family layer.

- **The authoring shape**: a project owns ONE hook file
  (`lit_hooks_project.hlsli` + its `effects/` includes — see
  `samples/04_project_game/project/content/shaders`) plus THREE-LINE
  entry wrappers (`project_lit.frag.hlsl` / `project_lit_a2c.frag.hlsl`
  / `gfxmesh_project_lit.frag.hlsl`) over the engine's extracted glue:
  `engine/shaders/lit_dependencies.hlsli` (the 7 shared includes) +
  `engine/shaders/gfxmesh_lit_decl.hlsli` (the mesh FS PSin/linkage
  block). The compile puts the project's shader dir FIRST on `-I` so
  its hook file wins; `HEADER_DIRS` tracks .hlsli dependencies ACROSS
  the include boundary; `tourmaline_project_lit_variants()` carries the
  engine-mirrored variant matrix (a project cannot drift from the
  engine's compiled set). Outputs are the project's OWN namespace
  (`project_lit.*.spv`), NEVER engine-shaders.pak; served through the
  shader library's dev tree (a project pak via `MountPak` is the
  production lane).
- **The runtime state**: `MaterialShaderFamily` (mesh.h —
  StandardLit/ProjectLit, CPU pipeline-selection state, NEVER in
  MaterialValues) with `World::SetMaterialShaderFamily` /
  `ShaderFamilyOf` — re-keyed through the same (owner, handle)
  capture. Family is BASE-MATERIAL granularity (instances inherit; only
  userdata is per-instance) so it can never split a batch.
- **The renderer consults**: `WorldRenderer::MaterialUsesProjectLit`
  (family + programs-loaded; ONE loud fallback report per session when
  the modules are missing — never a silent effect drop) feeds the
  transparent leg's family twins, the direct opaque pick (monolith-level
  parity with that leg's stock shape) and its A2C pick (the family A2C
  twin ONLY — shaderCount 0 keeps Masked family draws on the family's
  discard variant); the filtered ranges consult
  `GfxRenderer::MaterialProjectFamily` (the `ConfigureMaterialFamily`
  lazy resolver) → the family filtered programs; the mesh leg
  (`MeshProgramFor(..., projectLit)`) picks the family mesh twins and
  returns NULL when the family mesh base is missing — the range is NOT
  mesh-eligible and the indexed family program draws it (design §9
  fallback, never a stock mesh FS).
- **LOD**: ProjectLit materials opt out of BOTH far-shading mechanisms
  (`World::FarShadingAt` returns default{}) — the cheap vertex-lit FS
  never runs the hooks — so the effect survives to the farthest cooked
  LOD (the documented v1 cost).
- **The demo** (`samples/04_project_game`): the `ProjectLitDemo` element
  (composed with the audio runtime — RunPakApp has ONE extraElement
  slot) pulses the damage + wetness bits through `SetMaterialUserData`
  on the scene's first opaque material. Headless A/B:
  `TOURMALINE_EXTRA_ELEMENT_BENCH=1 TOURMALINE_CAPTURE=12 --bench 16`.
- Tests: `family.*` (test_world_entities.cpp) + `gfx.family-*`
  (test_gfxfilter.cpp).

## Running

`App.exe` runs from the build directory (`out/build/x64-Release`),
loading `content/paks/*.pak` and `content/shaders/*.spv` relative to
CWD. Redirect `*.log` output to `out/logs/` (root is for sources, not
logs). `--bench <n>` runs 10 warmup + n measured frames, then exits.

Deterministic capture (auto-exits after the bench budget; **+
docs/headless_bench.md** is the FULL pin/detail reference):

```powershell
$env:TOURMALINE_SSR='1'; $env:TOURMALINE_AA='off'; $env:TOURMALINE_AUTO_EXPOSURE='0'
$env:TOURMALINE_CAM_POS='24.26,1.82,-9.52'; $env:TOURMALINE_CAM_FWD='-0.947,-0.022,-0.321'
$env:TOURMALINE_CAPTURE='48'
& .\App.exe --pak content/paks/bistro.pak --disable_control --bench 64
```

Pins (every one: `TOURMALINE_*` lives in `engine/App/RunConfig.h`):

- `TOURMALINE_CAPTURE=<frame>` — exports `exports/screenshot_*.png`;
  the frame must land INSIDE the bench budget (capture < bench)
- `TOURMALINE_SSR_DUMP=<frame>` — SSR stage stats + PNGs to `exports/`
- `TOURMALINE_AO=off|ssao|sao` — AO mode headless (default sao);
  with `TOURMALINE_SSR`/`TOURMALINE_EVSM`/`TOURMALINE_TRANSPARENCY`
  this spans the full A/B matrix
- `TOURMALINE_GI_INTENSITY=<0..2>` — GI intensity (0 direct-sun-only,
  1 authored, 2 doubled)
- `TOURMALINE_NORMAL_FIX=0|1` (+ `_DEBUG=1`) — JHFAA shading-normal fix
  (experimental; docs/jhfaa_normal_fix.md)
- `TOURMALINE_VOLUMETRIC[_STEPS][_GI_VOLUMES][_DENSITY/_SUN]` +
  `TOURMALINE_FOG_GI_DEBUG` — fog/fog-GI pins
- `TOURMALINE_FOG_CSM=0|1|2` — the fog sun-occlusion feed (0 off /
  1 the raw main CSM tap, PCF mode only / 2 the dedicated coarse atlas
  + full R32F SPD MIN pyramid, PCF+EVSM); `TOURMALINE_FOG_SHADOW_MARK_ALL=1`
  is the headless test rig that marks every opaque surface a volumetric
  caster (docs/headless_bench.md "Fog sun-shadow lane")
- `TOURMALINE_PROBE_PREMIX=0|1` — Remember Me probe pre-mix A/B
  (0.00% differing pixels is the contract)
- `TOURMALINE_FP_MASKS=0|1` — Forward+ hierarchical tile-light culling
  (NO-MISSING-CONTRIBUTOR contract); under DS the F+ cull keeps the
  linear scan
- `TOURMALINE_HZB_SPD[_VERIFY]=0|1` / `TOURMALINE_HIZ_SPD[_VERIFY]=0|1` —
  SPD pyramid builds + byte-compare harnesses; extents > 4096 fall
  back per-mip
- `TOURMALINE_CONTACT_SHADOWS[_STEPS/_DIST/_THICKNESS/_RES/_DUMP]` +
  `TOURMALINE_DEBUGVIEW=contact` — sun contact-shadow pins
- `TOURMALINE_SSGI=1` + `TOURMALINE_DEBUGVIEW=ssgi_*` (+
  `TOURMALINE_SSGI_DEBUG_EV`) — SSGI compose diagnostic views
- `TOURMALINE_DEBUGVIEW=pbrhealth|pbrbase|pbrdiffuse|pbrrough|
  pbrmetal|pbrf0|sss|hair|water` — PBR Material Validation Debugger views
- `TOURMALINE_DEBUGVIEW=clipping|clipdark|clipbright` (+ `TOURMALINE_CLIP_DARK/_BRIGHT`) —
  image clipping diagnostics
- `TOURMALINE_GI_POINTS=1`, `TOURMALINE_GI_PROBE_MODE=0|1|2`,
  `TOURMALINE_GI_HOTSWAP=<frame>` — GI probe spheres/modes/hot-swap
  frame-safety regression
- `TOURMALINE_GEOMETRY_STREAMING=1` (+ `_BUDGET_MB`, `_EVICT_IDLE`) —
  geometry LOD streaming; BC6H ("GV6H") is the canonical GI-volume
  format, a hard engine requirement (no flag)
- MATERIAL_RECEIVE_CONTACT_SHADOWS (the emissiveFactor.w flags word,
  mesh.h kMaterialFlag*) — the per-material contact-shadow receive
  gate (OPT-IN; paks cooked before the flag read w=0 — recook)

## Tests

Tests are plain executables (no gtest), gated behind CMake's standard
`BUILD_TESTING` switch (`include(CTest)` in the root CMakeLists — ON by
default). A dedicated tests tree keeps them separable from working
configs:

```powershell
cmake -B out/build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON .
cmake --build out/build/tests
ctest --test-dir out/build/tests --output-on-failure
```

The normal configs (out/build/x64-Release etc.) build the test targets
by default too — `ctest --test-dir out/build/x64-Release
--output-on-failure` keeps working as-is. Pass `-DBUILD_TESTING=OFF` to
exclude the test targets from a configure entirely (that is what CI's
BUILD VERIFICATION does; the Tests workflow runs the suite — the five
GPU-window tests excluded on hosted runners: no display / Vulkan ICD).

`project_e2e_tests` is the BUNDLED-PROJECT ACCEPTANCE GATE: the
checked-in samples/04_project_game project copied to a temp dir, cooked
and published through the real `Project.exe build` CLI (the subprocess
stage adapter — Cook/GiBaker/Pak spawn as on a contributor's machine),
then the deployment halves headless: DeployCheck over every published
pak, the compiled catalog vs the registry's record fold, every
byte-servable record read through its pak (the .tapp-free discovery
lane proved against the release-pointer session), and a scene load
through the public clusterLoader seam. Compiling the sample's game
exe is the build configs' job; its GPU launch stays a dev-side golden
run.

`architecture_tests` is the NAMESPACE/RHI BOUNDARY LEAK ALARM (R1–R9,
see "The namespace boundary invariant" above): every ctest run — local
or CI — fails on NEW boundary leakage (broad smvk imports, new/grown
compat re-export blocks — smvk-side AND global-scope — new global
namespace declarations, new/grown RHI implementation-state exposure,
upward RHI includes incl. quoted forms). Its baselines are exact both
ways: growth fails, and retiring surface fails until you delete the
baseline row in the same commit. If a boundary check fails your build,
do not "fix the test" — fix the leak, or add the row with a comment
when the exposure is deliberate.

## Conventions

- Push-constant structs: C++ (`engine/include/Engine/Rendergraph/*.h`)
  and HLSL mirrors must stay layout-identical — check the
  `static_assert(sizeof(...))` and the HLSL comment when editing either
  side; run `codegen.py --check`; never add sub-192 fields (FS-only
  extras go in `FrameUniforms::frameHot`).
- Frame renderer plumbing: `engine::AppFrameContext`
  (engine/include/Engine/Rendergraph/FrameContext.h) carries per-frame
  handles; renderers snapshot what they need at Append time.
- Target-heap packing (the packed render-target memory):
  `docs/target_heap_packing.md` is the system reference (the
  bind-before-view contract, the retirement FIFO pairing, how to read
  the `[TargetHeap]`/`[TransientHeap]` stderr offsets, and `03_fuzz` —
  the regression host). Heap image work must follow the bind-before-view
  contract and the Holder declaration orders documented there.
- Heavy explanatory comments are the repo norm — keep the reasoning
  auditable.
- New `TOURMALINE_*` knobs: declare in `RunConfig.h` + parse in
  `RunConfig.cpp` + extend `WarnUnknown` + cover in
  `test_runconfig.cpp`. Never scatter `getenv`.
- New scratch vectors: `FrameVector` (dies in scope) vs
  `UploadInFlightVector` (spans frames) — wrong choice is UAF or leak.
- New shaders: include `push_constants.hlsli` directly when possible
  (fine-grained deps); `common.hlsli` umbrella still works.
- New tests: plain-exe + `add_test` + `tourmaline_stage_sdl3`,
  hand-rolled `Check()` runner, `checks=/failures=` summary, exit
  nonzero on failure.
- Bake albedo: the CUBEMAP captures bake TEXTURED (per-hit baseColor —
  `GiTraceScene::AlbedoAt`); the GI fields (.sggi octree + GI volumes)
  bake white-world 0.7 by convention (the 9-lobe SG fit cannot
  represent per-material chroma); `--uniform-albedo off`(GiBaker) opts
  into the textured transport for the volume bakes — the invalidation
  hashes fold the albedo MODE + value, so the flip re-traces.
- Bake transport is FULL PBR (`kBakeAlgorithmRevision` 2): every vertex
  traces the whole glTF metallic-roughness BSDF — the Lambertian
  diffuse NEE PLUS the GGX specular NEE toward the sun + static spots
  and one-sample-MIS lobe sampling for the bounce hop. A diffuse-only
  transport treats METAL as black (Sponza's gold trim ate the sun);
  white-world runs the SAME machinery with uniform colors. Pinned by
  TestGiPbrTransport + the exact analytic re-pin in TestGiPrimarySunIncluded.
  Emissive stays runtime-only (never baked).
- GI firefly clamp (GiBaker `--clamp 8|off`, both volume bakers): the
  two-pass per-probe cap at clampRatio × the probe's own finite-sample
  mean luminance, pre-SG-fit. The clamp is ALSO a systematic energy
  loss on spiky distributions, so every bake log carries the
  discarded-energy fraction and `--clamp off` is the ready A/B (the
  invalidation hashes fold the ratio). Known transport limitation
  (deferred): skylight is evaluated ON MISS ONLY — partially-enclosed
  scenes need deep paths to find openings.
- Bake ray-tracing backend: EMBREE is THE backend (the GiBaker default
  and the engine's GiSceneBuildOptions default). The builtin
  binned-SAH BVH TRANSPORT is DEPRECATED and marked for deletion. Bakes
  are reproducible per backend; the two backends agree within
  Monte-Carlo noise.
- Ambient closure (`--cubemap-ambient-closure`, default OFF): captures
  + the GI stack both carrying the ambient double-counts every
  mirror-like surface. ON only for stack-less cooks (bistro's bat).
  Dead captures (mean luma ≤ 20) drop from the manifest.
- Weave cloth transmission (`--mask-transmission on`, Cook + GiBaker —
  sponza's bat): MASK cutout cloth keeps its full lit albedo AND passes
  light tinted per texel by its baseColor texture. Default OFF — MASK
  stays the opaque occluder (bistro foliage).
- LOD sibling errors stay MESH-LOCAL in the artifact (scale-free) — the
  renderer applies the instance's world scale at pick time
  (`World::SelectLodIndex`). Never bake a scene transform into the
  serialized error.
- GPU-counter buffers (anything an `InterlockedAdd`/append reads-
  modifies-writes from its first use) MUST be creation-zero-filled via
  `UploadDataToBuffer`: DEVICE_LOCAL fresh allocations recycle freed
  pages, so a churn-recreated counter can start from a garbage count
  and the consuming compute shader MMU-faults (the editor's drag-burst
  DEVICE_LOST, pinned via `VK_NV_device_diagnostic_checkpoints`;
  `TOURMALINE_CHECKPOINTS=0` kills the marker emission; `x64-Release-
  ShaderDebug` + `TOURMALINE_SHADER_DEBUG=ON` for Aftermath line tables).
- Don't use F12 for any binding since it can trigger debugbreak.
- Don't ever bump format versions or add conversions to keep old data
  working. Bumping magic to break old files is ok. No important assets
  to keep.
- The editor/launcher split: `Editor.exe` REQUIRES `--project <dir>`
  (no project, no launch). `EditorLauncher.exe` (editor/launcher/+,
  console v1) owns project SELECTION — the shared editor.prefs recents
  + a typed path (scaffolding when the `project.tourproj` marker is
  missing) — and spawns the editor with its pick. Smoke/editor runs
  pass `--project F:/projects/game` (see the temp scripts).
- Engine limits are OWNERSHIP-SCOPED (the old global MAX_* block in
  smvk.h is gone). The shared CPU/HLSL layout-contract values
  (MAX_MATERIALS / MAX_SIMULTANEOUS_LIGHTS / MAX_TEXTURES /
  MAX_VOLUME_TEXTURES / MAX_SAMPLERS) live in ONE file —
  `engine/shaders/engine_limits.hlsli` — whose ONLY C++ includer is
  `Engine/Core/Limits.h` (converts the macros to constexpr and #undefs
  them; never include the .hlsli from C++ anywhere else). Surface:
  `engine::renderer::limits` (`Renderer/RendererLimits.h` — the GPU
  material table cap + MaxSimultaneousLights, the RUNTIME how-many-
  lights-may-be-BOUND-AT-ONCE-per-frame budget, NOT a per-scene cap)
  and `smvk::limits` (`RHI/DescriptorLimits.h` bindless budgets +
  `RHI/FrameConfig.h` frame scheduling/window defaults). Editing
  engine_limits.hlsli recompiles every shader AND every engine TU on
  purpose — a layout contract must never ship half-applied. The
  pool↔descriptor budget static_assert lives at the INTEGRATION site
  (vulkan_impl.h), never in Memory/pool.h.
- Public APIs take RAW POINTERS + COUNT/SIZE, never container types:
  `f(const Thing* items, size_t itemCount)` / `f(void* out, size_t
  outBytes)` — NOT `std::vector`/`std::span`/`std::array` parameters.
  The caller owns the memory (no hidden allocation/copy, no allocator
  coupling, trivial C ABI/export + FFI later, "nothing" is just
  `{nullptr, 0}`), and count is explicit at the boundary. Containers
  are fine as INTERNAL storage and as return values when convenient.
  Batch/getter pairs use the same shape: `Things() const` +
  `ThingCount() const` (pointer + count), not a vector return.
  Historical span-taking RHI overloads exist (BulkCopyBuffer, the async
  readbacks) — don't propagate them to new code; when touching one,
  prefer adding a pointer+count overload.

## Strict resources (`--strict` / `TOURMALINE_STRICT=1`) + the RHI error contract

**Intention**: resource-pool exhaustion (bindless descriptor budget,
image/view pools) must never hide — but the RHI never decides policy.
**The Vulkan RHI returns error signals (empty handles) and the
responsibility to handle them is at call sites**: the default is
graceful (skip/fall back — editing sessions survive budget pressure),
strict turns exhaustion into a LOUD abort at the call site so silent
skips cannot hide leaks or budget-size bugs. Asserts INSIDE the RHI are
for invariants only (programmer errors) — never for operational
conditions like pool exhaustion.

- `Editor --strict` (editor/src/main.cpp) and `TOURMALINE_STRICT=1`
  (any host, any build) gate `smvk::StrictResources()`
  (engine/include/Engine/smvk.h, implemented in engine/src/Vulkan/impl.cpp).
- **Call-site pattern** (the RHI has no abort paths):
  `if (handle.empty()) { smvk::OnResourceExhaustion("site"); return; }`
  — OnResourceExhaustion is a no-op unless strict, in which case it
  aborts with the diagnostic (`tourmaline_fatal.log` + stderr). Debug
  builds also keep the pools' own `assert`s — strict is the
  RELEASE-visible form.
- **Work is NOT yet done across the whole project**: the gate currently
  lives at the element churn sites in WorldRenderer (the demonstrated
  exhaustion points). The other creation sites still handle gracefully
  without the strict report. Spreading the gate is in-progress work;
  **using the pattern in new code is OK** (and expected at any new
  exhaustion-prone creation site).

## Linux compatibility

The tree builds and passes ctest on Linux (Ubuntu 24.04 + GCC 14
verified in WSL; the same test suite as Windows, passing on both).
Conventions:

- MSVC ACCEPTS but GCC REJECTS a redundant namespace qualification on
  an out-of-line function definition inside its owning namespace block
  (`namespace tour { glm::mat4 tour::Foo(); }` — "explicit
  qualification in declaration"): the mechanical
  namespace-migration passes qualify CALLERS, never definitions inside
  the moved block. The camera.cpp Linux-CI failure (green locally on
  MSVC) was exactly this. A `wsl -d Ubuntu-24.04 -e bash -lc "g++
  -fsyntax-only -std=c++20 -Iengine/include -Iexternal/glm <file>"`
  sweep over the touched files catches the class before the push.
- `install_dependencies.sh` is the Linux installer: it carries the
  platform-specific pins (SDL3 3.4.16 SOURCE zip — the root CMakeLists
  builds it as a subproject when no system SDL3 exists; the DXC
  official linux package; Vulkan-Headers 1.4.357 — distro
  libvulkan-dev headers are too old; DirectX-Headers + DirectXMath +
  the dotnet-PAL sal.h for DirectXTex's non-Win32 path).
- Toolchain: **GCC 14** (required for the verified build). GCC 13 ICEs
  in `gimplify` on the engine's array-NSDMI aggregates (see the NOTE at
  `struct ShaderProgram` in engine/include/Engine/smvk.h) — never write
  `x = ShaderProgram{}` or `ShaderProgram p{};` for that type; use the
  paren/default forms.
- The engine PCH is MSVC/Clang-only; GCC force-includes pch.h as a
  plain `-include`. Do not move TUs off pch.h's transitive includes
  without adding their own includes.
- `find_package(Ktx CONFIG)` — capital K (KTX 4.3+ ships KtxConfig.cmake;
  lowercase only matches on case-insensitive filesystems).
- `<ktxvulkan.h>` — KTX 4.3+ renamed the headers lowercase; on Windows
  the old spelling worked via case-insensitivity. Always use the
  lowercase includes.
- DXC staging + every runtime .so stage at the BUILD ROOT
  (`${TOURMALINE_BINARY_DIR}`), not the static lib's dir; Linux exes
  carry a `$ORIGIN` rpath (cmake/HostSetup.cmake + editor/CMakeLists.txt).
- Streamline/DLSS is Windows-only by design (engine/src/Renderer/dlss.cpp
  compiles stubs on Linux); never add Linux loads of the sl.* runtime.
- Runtime note: boots + loads + pipeline-compiles are verified on the
  mesa software rasterizer (lavapipe), but its compute-pipeline
  compiler crashes on the BDA-heavy engine shaders — the render loop
  needs a real GPU. Do not "fix" lavapipe crashes as port bugs without
  a GPU-verified repro.
