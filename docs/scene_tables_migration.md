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
**the magic IS the version**). **The magic bumped to `SMP7`/`KPM7` AT
EPOCH 4** (the owner's rule: no old-pak support, no versioning, no
compatibility layers — ONE canonical path; the reader accepts exactly
one magic and the SMP6-era legacy-terrain acceptance hack is deleted).
Epoch 7's chunked scene redesign bumps again whenever it changes the
container/blob contract — bumps are free, conversions are forbidden.

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
    REMOVED from the installers/README/notices ahead of schedule (the
    owner's call right after Epoch 6 — the fetch entry, the tracked
    .tourmaline-fetched marker, the notices row and the .gitignore
    lines are gone; the Epoch 15 cleanup item is closed).
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
- 3 — `refactor(scene): introduce typed data tables` (rows +
  `rowsForNode` derived lookup + handwritten Set/Find/Remove;
  `UINT32_MAX` invalid-row in-memory).
  - **Record**: `scene_node_id.h` extracted (NodeId was inline in
    scene.h; the tables header needs it without a cycle — and it is
    the ONE definition the stable-slot epochs 6b/6c will change).
    Four kinds this epoch (mesh/light/camera/decal — the pool-index
    indirections); swap-and-pop + lookup repair chosen as the
    deletion policy; `RebuildNodeRows` does an EXACT resize (grows
    AND shrinks — the first test run caught the grow-only gap) and
    compacts out dead rows (node out of range after a compacting
    deletion). Node-id remapping on compaction stays Epoch 6's
    contract. `NodeRows.body` reserved for Epoch 10. The maps remain
    authoritative — nothing populates the tables yet except tests.
- 4 — `refactor(scene): migrate mesh and material associations`.
  - **Record**: the epoch's open question answered — material
    associations exist WITHOUT meshes (tour assignMaterial on bare
    nodes, pinned by test_tour's "replay assign"; gltf instancing
    sets mesh-without-material) → MaterialRow is a SEPARATE table
    (MeshRow lost its material field; one owner per fact). The
    migration is semantics-preserving: every meshForNode/
    materialForNode site (111 + 101 across engine/tools/editor/app/
    tests) now goes through tables.meshes/tables.materials +
    Find/Set/Remove, and the MAPS ARE DELETED from Scene.
    Serialization went CANONICAL, not compatible (the owner's rule —
    no old-pak support, no compat layers, one canonical path): the pak
    magic bumped SMP6/KPM6 → **SMP7/KPM7** (the scene-tables break),
    the reader accepts EXACTLY ONE magic, the SMP6-era legacy-terrain
    acceptance hack is DELETED (terrain_hfield now pins forged
    SMP6/SMP5 images failing closed), and the legacy .scene
    association sections went row-native (u32 rowCount + pairs — no
    saveMap emulation). The pak scene blob's link-section layout is
    itself unchanged this epoch (rows sorted by node = the old
    sorted-pairs order); Epoch 7's chunk redesign replaces it. The
    resolve (ResolveAndFixup) remaps rows into the world scene through
    the mutators; deleteSceneNodes remaps row nodes through the
    compaction index (shiftMeshRowNodes/shiftMaterialRowNodes —
    order-preserving) and rebuilds the derived index once;
    ParseSceneBlob rebuilds it after the bulk row push. Validation:
    Windows tests 46/46 (tour replay, pak round-trips, world entity
    resolve/merge all table-based; the old-pak gate pinned by the
    forged-magic checks). All existing paks are stale by design —
    recook from zero (the owner recooks).
- 5 — `refactor(scene): migrate remaining node associations`
  (all twelve maps; `nameForNode`+`uuidForNode` fold into dense
  `NodeMetadata`; env/envVolume inline rows are already POD —
  giVolume/cubemap/export rows keep the authoring structs' strings;
  the COOKED StringRef arena is Epoch 7's).
  - **Record**: ALL TEN maps deleted (light/camera/decal/
    giVolume/cubemap/export/env/envVolume → typed tables with
    inline payloads; nameForNode+uuidForNode → dense NodeMetadata
    array). scene_payloads.h extracted (the payload structs moved
    out of scene.h so the rows can reference them without a
    cycle). Inline-payload rows are copyable-not-trivial (strings
    live in the authoring structs; the COOKED StringRef layout is
    Epoch 7's). findNodeByName/getNodeName/setNodeName re-targeted
    to metadata; mergeScenes clears all tables with the fresh-root
    reset and appends metadata with name-offset remap; the resolve
    remaps the Epoch-5 kinds (the four never-carried kinds —
    giVolume/cubemap/export/uuid — ride the resolve from EPOCH 6,
    not this one); deleteSceneNodes shifts every
    table + metadata (shiftNodeMetadata). Pak writers sorted by
    node (byte-stable); readers bulk-push + one RebuildNodeRows.
    Two migration-war stories recorded for posterity: the
    envvol-priority default needs the derived index LIVE between
    passes (the env bulk push became SetEnv calls — FindEnv in the
    envvol pass missed index-less rows); and a nested-regex
    collision mangled asset_export's name-index ternary (empty
    asset names — every namespaced-reference test caught it).
    Validation: Windows tests 46/46 (incl. tour replay, env
    volumes, world entities); editor smoke clean. The mechanical
    bulk migration was scripted (regex transforms + a
    compiler-error-driven fix loop) — 500+ sites, zero remaining
    live references.
    - **Commit-review findings** (the 10/30 review of 3e6bea6 +
      7a8a1858): CRITICAL — the resolve's metadata append wrote
      names/uuids PAST the appended nodes (EnsureNodeMetadata had
      already grown to the new node count, so `base = size()` was
      past them and the copy landed in a phantom second region —
      double-sized allocation, empty names/uuids at the real nodes).
      It survived every test because the uuid check searched the
      whole metadata array instead of the row's own node. Fixed by
      6b's nodeMap metadata copy (SetNodeNameIndex/SetNodeUuidRaw at
      nodeMap[s] — no offset arithmetic) and pinned per-node in
      TestResolveCarriesInlineTables. Also noted: NodeMetadata's
      mixed name-index + std::string uuid footprint is a cache-
      locality concern — deferred to Epoch 7's string arena (the
      locked plan) unless profiling says otherwise; the payload
      setters/removers now move rows instead of copying (the
      string-owning kinds).

- 6 — `refactor(world): support table remapping and merging` —
  targets `ResolveAndFixup_clusters`, NOT `mergeScenes`; the four
  never-carried maps start riding the resolve here.
- **6b — `refactor(world): stable node slots and generations`**
  (inserted): handles survive node deletion without renumbering;
  the dense-index remap contract becomes a slot-association contract.
  - **Record**: NodeId became a STABLE SLOT INDEX (the epoch's
      representation change lives in scene_node_id.h + Scene's two new
      members: nodeGeneration — bit31 alive + 12-bit generation per
      slot, may lag hierarchy.size() with untracked slots reading live
      — and nodeFreeList, LIFO). PACKING THE GENERATION INTO NodeId
      ITSELF was rejected against the audit: ~500 sites index the
      parallel arrays by NodeId directly (predicates, gizmos, resolve
      internals) and a packed handle would silently corrupt every site
      that doesn't decode — instead NodeId stays directly indexable,
      arrays grow HOLES, and generations are a PARALLEL guard exposed
      as the packed NodeHandle vocabulary (20-bit slot | 12-bit gen,
      scene_node_id.h — the slot ceiling 2^20 matches the pak reader's
      node-count guard; 4,096 lives per slot before wrap). Handles are
      the LONG-LIVED-reference type: MakeNodeHandle/NodeHandleValid,
      unit-pinned for the ABA case (a stale handle stays invalid across
      slot reuse). 6c adopts them at the editor surfaces where cluster
      unloads make mid-session retirement real; 6b's own retirement
      sites (static_merge's cook delete, editor uncreate) are covered
      by slot stability alone. DELETION RETIRES: deleteSceneNodes
      collects the subtree, unlinks roots from SURVIVING parents
      (UnlinkNodeFromParent — firstChild/nextSibling/lastSibling
      surgery), and RetireNodeSlot frees each slot (drop rows via
      RemoveNodeRows — the rows-never-outlive-nodes discipline — clear
      flags/metadata/transforms, reset the hierarchy entry with
      level=-1, bump the generation, push the freelist). The whole
      pre-6b compaction machinery DIED with it: the newIndices remap,
      findLastNonDeletedItem, eraseSelected over the parallel arrays,
      shiftNodes, and the ten shift*RowNodes + shiftNodeMetadata
      helpers (deleteSceneNodes was their only caller; eraseSelected
      survives as an unused scene_utils utility). addNode pops the
      freelist (resetting EVERY parallel array entry for the reused
      slot — the touranim "addNode keeps every array in sync" invariant
      extended to reuse, parent re-asserted LIVE not just in range) or
      appends sequentially (a never-deleted scene is bit-identical to
      the dense era). THE REMAP CONTRACT: ResolveAndFixup_clusters and
      mergeScenes swapped +nodeOffset arithmetic for explicit
      nodeMap[clusterSlot] -> worldSlot tables (numerically identical
      on dense scenes, correct on holed ones — the resolve allocates
      world slots via addNode per live node, maps links/rows/metadata/
      uuids/defaultCameraNode/activeEnvironment through the map; the
      cluster root is the FIRST LIVE slot). COOKED STAYS DENSE (the
      locked decision): both serializers compact ascending-live-slot
      through BuildSlotToDenseMap — byte-identical output to the pre-6b
      writers for never-deleted scenes (ascending-slot == the old
      stable_partition order), holes never serialize; the pak writer
      FAILS loudly on any row/joint still pointing at a retired slot
      (invariant violation), the .scene loaders drop such rows (the
      lenient-format convention); readers expand dense files to fresh
      gen-0 slots (the pak reader already built via addNode on a fresh
      scene — naturally compatible; loadScene resets the slot arrays).
      SCENE_TABLES: NodeInRange gained liveness (SetX/FindX reject
      retired slots — a row can never point at a hole), RebuildNodeRows
      compacts rows of dead slots as the discipline backstop. FLAT
      ITERATION gained alive checks where the per-node predicates
      couldn't reject holes: MarkAllRootsChanged (parent==-1 defaults
      would masquerade as roots), recalculateGlobalTransforms(+Dynamic)
      (dirty-list entries can outlive a mid-frame retire),
      forEachNode, the editor locator loops (a bare-hole reads as a
      locator without the guard), dumpSceneToDot. EDITOR: the six
      uncreate sites (light/camera/givolume/cubemap/export/transform)
      dropped the tail-pop/tombstone split for a shared
      RetireCreatedNode (unlink + retire + World draw-cache scrub
      flags/drawItems/itemFlags + selection scrub) — physical
      retirement now works ANYWHERE (mid-graph uncreates no longer
      leave hidden zombie nodes), the childless gate kept only where
      children live under the node (tombstone). Editor delete STAYS
      tombstone (already stable-id semantics; dissolves with 6d).
      static_merge's post-delete URI-lookup repair DELETED (ids no
      longer shift — the dangling-id pain this epoch removes).
      Terrain uncreate was already tombstone-only — untouched.
      PINS: SceneTableTests +22 (retirement keeps slot count/live
      count, survivor rows verbatim, sibling repair, handle ABA across
      reuse, clean reuse, mutators reject dead slots, rebuild drops
      dead rows), WorldEntityTests +7 (TestResolveHoledCluster — the
      holed-cluster resolve through the map), TourTests' flag-table
      pin REWRITTEN from "the survivor's flag slid with the compacted
      index" to "the survivor KEPT its id" + slot-reuse + the holed
      save/load compaction round-trip (409/409). Validation: Windows
      Release 46/46 + full tree clean; Debug full tree clean with
      camera_tests' 3 mouselook dt-invariance failures PRE-EXISTING
      (verified by stash-rebuild on the pre-6b tree — /Od float
      determinism, no scene involvement); editor smoke clean
      (--project F:/projects/game boots to ready, no faults). Linux
      CLOSED (the pending Epoch 5/6 gate with it — WSL recovered):
      Ubuntu 24.04 + GCC 14, the linux_epoch4.sh pull-build-ctest
      cycle over the branch at d21fb435 — full tree clean, 46/46
      (tour/scene-table/world-entity/gi/pak-host all green;
      camera_tests passes on GCC, pinning the Windows-Debug mouselook
      failures as an MSVC-/Od-only artifact).

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
  (the CHUNKED scene payload — NODS/XFRM/MESH/LGHT/CAMR/DECL/ENVS/
  RBOD/COLL/META + STRS string-arena chunk; per-node association
  table uses 0=absent; giVolume/cubemap/uuid serialized for the first
  time; export rows stay cook-side only). The container magic is
  already `SMP7` (bumped at Epoch 4); another bump only if the
  chunk redesign changes the container contract.
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
