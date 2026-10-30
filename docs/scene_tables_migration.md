# Scene Data Tables, Pak V2 & Jolt Physics — migration record

The working document for the typed-scene-tables migration. This file
records the baseline, the inventory that drives the epoch order, and
the design decisions locked in before each phase. Update it at every
epoch boundary (the epoch's commit records the delta here).

## Baseline

- Branch: `feature/scene-tables-jolt` (from `master`)
- Base SHA: `f13ad32fccf920d920e25513e92eac3b27ba6b1b` (master, 972 commits)
- Tests: `out/build/tests` (Ninja, Release, BUILD_TESTING=ON) —
  **44/44 passing** at baseline.
- Windows toolchain: VS 18 Insiders MSVC 14.52, vcvarsall x64.
  Linux verification per AGENTS.md (Ubuntu 24.04 + GCC 14 in WSL).

## Inventory — what `Scene` actually carries today (audited at baseline)

`NodeId = uint32_t`, a DENSE index into parallel `std::vector`s
(`hierarchy`, `localTransform`, `globalTransform`) plus seven dense
per-node flag tables (`staticRoot`, `staticNode`, `noLightmap`,
`lightmap`, `castsVolumetricShadow`, `ditherCloseup`, `hidden`).
The flag tables stay as they are — dense per-node bytes are the right
shape for them; they are NOT part of this migration.

The twelve node-association maps (`engine/include/Engine/Scene/scene.h`),
split by value kind:

| Map | Value | Kind | Serialized in pak scene blob? | Carried by `ResolveAndFixup_clusters`? |
|---|---|---|---|---|
| `meshForNode` | pool/location index | indirection | yes (pak-local asset id) | yes (→ `meshLocations`) |
| `materialForNode` | pool/location index | indirection | yes | yes (dedup) |
| `nameForNode` | string-pool index | indirection | yes | yes |
| `lightForNode` | cluster light pool idx | indirection | yes (v4) | yes (→ `lightLocations`) |
| `cameraForNode` | cluster camera pool idx | indirection | yes (v8) | yes |
| `decalForNode` | cluster decal pool idx | indirection | yes (v11) | yes |
| `giVolumeForNode` | `SceneGiVolume` (has strings) | inline | **NO** | **NO** |
| `cubemapForNode` | `SceneCubemap` (has strings) | inline | **NO** | **NO** |
| `exportForNode` | `SceneExportSettings` | inline | **NO** (editor-only by design) | **NO** |
| `envForNode` | `SceneEnv` (17 floats) | inline | yes (v9) | yes (verbatim) |
| `envVolumeForNode` | `EnvVolume` | inline | yes (v13) | yes |
| `uuidForNode` | `std::string` | inline | **NO** | **NO** |

Two consequences the plan must respect:

1. **`giVolumeForNode` / `cubemapForNode` / `exportForNode` /
   `uuidForNode` have NO serializer today.** Their transport is .tour
   replay (cook-time state harvested by tools). Migrating them to rows
   (Epoch 5) and then adding pak chunks (Epoch 7/8) is a semantic
   upgrade — those authored facts become runtime-loadable for the first
   time — not a reformat. `exportForNode` deliberately stays
   editor/cook-only (the current code excludes it from packed runtime
   content; preserve that).
2. **`mergeScenes` is NOT the merge path.** It deliberately drops
   lights/cameras/decals ("merge via World::ResolveAndFixup_clusters
   instead", Scene.cpp:929). Epoch 6 targets `ResolveAndFixup_clusters`
   (world.cpp) — the wipe-and-rebuild that also explains the dynamic
   cluster (below).

Serialization today: the legacy `saveScene` (unversioned positional
FILE*) and the pak scene blob (trailing-section appends with an
exact-end rejection check; the pak container has NO version field —
**the magic IS the version**, currently `SMP6`/`KPM6`). Epoch 7 bumps
to `SMP7`/`KPM7` per the house no-conversions policy.

## Locked design decisions (pre-Phase-I review)

From the external flat-storage proposal, adopted/adapted/rejected
against this codebase:

**Adopted**
- Rows are trivially copyable POD. Strings inside inline payloads
  (`SceneGiVolume` bake paths/GUIDs, `SceneCubemap` URIs, node UUIDs)
  become `StringRef { uint32_t offset; uint32_t length; }` into ONE
  flat per-scene string arena. Editor mutation is append-and-replace
  with occasional compaction; no clever string allocator.
- Cooked vs mutable storage are DIFFERENT shapes sharing the same row
  definitions: cooked = compact sequential arrays, read-only after
  load, dense cluster-local node references, no free list; live world
  = mutable, creation/deletion. Never force one allocation policy on
  both.
- The pak scene chunks are literal row arrays with a static layout:
  field widths + packing pinned by `static_assert(sizeof(Row))` and a
  layout-pinning test (the push-constant contract pattern), written
  field-group-wise by handwritten writers — never a dump of the
  compiler's in-memory layout, but the two coincide by construction
  and the test proves it stays that way.
- Transforms stay separate from hierarchy (already true) and default
  to identity, never zero (already true — glm).

**Adapted**
- Stable node slots + generation counters: the right IDEA, but NOT in
  Epoch 3. `NodeId` is a dense index baked into every consumer and the
  whole cluster resolve/merge remaps by dense offset; slots+generations
  land when `World::scene` becomes authoritative (Epochs 6b–6c below),
  not before. The cooked format stays dense cluster-local (a cooked
  scene needs no free list).
- The `0 = absent row` convention: adopted in the COOKED per-node
  association table only (a zero-filled region means "no association"
  — free under bulk-zero or future mmap loading). In-memory keeps the
  house `UINT32_MAX` sentinel. The ±1 conversion lives inside the
  handwritten reader/writer pair, nowhere else.

**Rejected**
- Raw `NodeSlot*` + calloc arrays replacing `std::vector`: a vector of
  trivially-copyable rows IS contiguous bulk storage. Hand-rolled
  capacity/growth/cleanup across ~10 tables is exactly the
  infrastructure-over-explicit-code the migration rules forbid, and it
  would bypass the engine's tracked process-memory reporting. Zero-init
  where wanted is `resize`/`assign` (value-init); the only place
  raw-array zero-fill pays is the cooked/read-only side, which is not
  `std::vector` anyway.
- A custom `Mat4 { float m[16]; }`: `glm::mat4` is trivially copyable
  and the repo already memcpys matrices throughout; a parallel type is
  churn without portability gain (the disk layout is defined by the
  writer regardless of the in-memory type).

## Amended epoch roadmap

The external plan's 16 epochs, with the code-audit corrections and the
world-authority work inserted. Commit subjects follow the plan; the
deltas are marked.

- 0 — baseline (this commit).
- 1 — `build: vendor pinned Jolt Physics` (v5.6.0 — verified the
  latest release, published 2026-07-11; `JPH_BUILD_SHARED_LIBS` exists
  now, `JPH_USE_DX12/VK/MTL/CPU_COMPUTE` confirmed as the option
  names; Jolt 5.6 requires CMake ≥ the version pinned in its Build
  tree — bump the root `cmake_minimum_required` if it exceeds 3.17 and
  note it here).
  - **Record**: pin `e77f175` (tag v5.6.0). Root CMake bumped
    3.17 → 3.20 (Jolt's Build minimum; local CMake 4.4.2). Flag audit
    against the built ninja rules: Jolt TUs compile `-std:c++17`,
    `/MD` (dynamic CRT — the `USE_STATIC_MSVC_RUNTIME_LIBRARY OFF`
    pin), NO `/arch:` (the SSE4.2 floor pin worked), with Jolt's own
    `/fp:fast` + `/GR-` contained to its TUs (valid: a no-RTTI
    static lib links into RTTI engine code). All five of Jolt's own
    targets (unit tests / hello world / perf test / samples / viewer,
    ALL default ON) are forced OFF — the external plan's snippet
    missed them entirely. `DEBUG_RENDERER_IN_DEBUG_AND_RELEASE` +
    `PROFILER_IN_DEBUG_AND_RELEASE` forced OFF after the first
    configure audit caught `JPH_DEBUG_RENDERER`/`JPH_PROFILE_ENABLED`
    still defined (the pins were missing, not ineffective).
    `ENABLE_OBJECT_STREAM` OFF. Windows: Release + Debug App/engine
    build clean, `out/build/tests` 44/44. NOTE: `external/box3d` was
    already vendored "for upcoming physics work" (unwired,
    THIRD_PARTY_NOTICES) — superseded by Jolt for the 3D runtime;
    its fate gets decided at Epoch 15 cleanup.
  - **Gates (all closed)**: Windows Release + Debug clean; Linux
    (Ubuntu 24.04 WSL, GCC 14.2, CMake 3.28, ext4 clone provisioned by
    `install_dependencies.sh`) — full tree with Jolt builds, 44/44;
    the subproject-host path verified with a simulated game repo
    (`add_subdirectory` of this tree — `TOURMALINE_IS_TOP_LEVEL`
    FALSE: no tools/editor/tests, engine + Jolt link into the host
    exe through the PRIVATE link-only interface). One note for real
    game hosts: their OWN TUs must compile at C++20 (the engine's
    public headers use inline variables) — the simulation hit that
    first, any host that already links the engine knows it.
- 2 — `test: add Jolt physics smoke test`.
  - **Gates**: Windows 45/45; Linux — the smoke test pulled into the
    WSL clone, built, **all 7 checks pass** on GCC 14 too. Two v5.6
    API spellings pinned by the compile: `JobSystemThreadPool` (not
    JobThreadPool), `Quat::sIdentity` (not RQuat); and one physics
    lesson recorded in the test: the first draft centered the 20 m
    floor box at y=-10, putting its TOP at y=+10 — inside the drop
    volume — and the contact solver ejected the cube UPWARD (the
    step-trace caught it: rise to 10.48, sleep). Floor centered at
    -20 (top at y=0) as intended.
- 2 — `test: add Jolt physics smoke test`.
- 3 — `refactor(scene): introduce typed data tables` (rows +
  `rowsForNode` derived lookup + handwritten Set/Find/Remove; rows
  carry `StringRef`s from the start; `UINT32_MAX` invalid-row
  in-memory).
- 4 — `refactor(scene): migrate mesh and material associations`.
- 5 — `refactor(scene): migrate remaining node associations`
  (all twelve maps; `nameForNode`+`uuidForNode` fold into dense
  `NodeMetadata`; env/envVolume inline rows are already POD —
  giVolume/cubemap/export rows take `StringRef`s).
- 6 — `refactor(world): support table remapping and merging` —
  targets `ResolveAndFixup_clusters`, NOT `mergeScenes`; the four
  never-carried maps start riding the resolve here.
- **6b — `refactor(world): stable node slots and generations`**
  (inserted): handles survive node deletion without renumbering;
  the dense-index remap contract becomes a slot-association contract.
- **6c — `refactor(world): incremental cluster append/remove`**
  (inserted): cluster load/unload stops wiping + rebuilding
  `World::scene`; dynamically created nodes survive streaming. This is
  the precondition for 6d.
- **6d — `refactor(runtime): retire the dynamic cluster`**
  (inserted): `World::scene` becomes the authoritative live scene;
  `EnsureDynamicCluster`/`CreateDecalAt`'s mirror-node pattern and the
  unload-refusal contract dissolve; runtime-created resources move to
  explicit pools. (The dynamic cluster exists today precisely BECAUSE
  resolve wipes the world scene — see `World::CreateDecalAt`, which
  keeps the authoritative copy in the dynamic cluster and a
  throwaway mirror in the merged scene.)
- 7 — `feat(pak): introduce versioned scene table chunks`
  (`SMP7`/`KPM7`; NODS/XFRM/MESH/LGHT/CAMR/DECL/ENVS/RBOD/COLL/META +
  STRS string-arena chunk; per-node association table uses 0=absent;
  giVolume/cubemap/uuid serialized for the first time; export rows
  stay cook-side only).
- 8 — `refactor(cook): serialize and load typed scene tables`.
- 9 — `refactor(editor): migrate typed authoring and tour commands`
  (the main-architecture gate; .tour verbs keep working, implemented
  against the tables).
- 10–14 — physics epochs as planned (authored RBOD/COLL rows →
  runtime `PhysicsWorld` behind Tourmaline-owned handles → fixed-step
  + transform sync (respecting `staticRoot`: a dynamic body never
  lives inside a frozen subtree) → editor preview (temp sim world,
  authored data never overwritten) → cooked collision geometry).
- 15 — `refactor: remove legacy scene and pak paths`.
- 16 — `test: validate scene-to-physics pipeline`.

Rule stays: one epoch, one independently testable result; no permanent
parallel representations (the maps are authoritative only until their
consumers migrate, then they die in that same epoch or 15 at the
latest).
