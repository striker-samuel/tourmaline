# Pak vNext: SMP8 (the paged break)

**Status:** landed (the container layer of the consolidated content
architecture). The async pager (Phase 5) and the compiled static
plans (Phase 6) build on the primitives defined here.

## The format

SMP8/KPM8 (the magic IS the version — no version field, no compat
reader; every SMP7-or-older pak fails closed at the magic and content
recooks from zero — the standing policy, now exercised: the sponza
pak died exactly this way and recooked clean).

```
Header  (32 B): magic "SMP8", reserved, flags, tocOffset, tocCount, reserved
Blobs   : payloads, 16-byte aligned. Each blob is a PAGE: an
          independently verifiable, independently readable extent.
TOC     : per entry { u64 pathHash, u32 kind, u32 flags,
                      u64 offset, u64 size, u32 slot,
                      u32 contentCrc, u32 pathLen, path }
Footer  (24 B): tocOffset, tocCount, tocCrc, magic "KPM8"
```

The four SMP8 deltas over SMP7:

1. **Per-page integrity (`contentCrc`)** — the CRC-32 of each entry's
   STORED bytes (the LZ4 wrapper included when compressed). Verified
   at FETCH time (`Payload()` — once per reader, cached), never at
   mount. A corrupt page throws AT THE PAGE with its path; its
   neighbors stay servable (selective integrity).
2. **The small mount** — the footer CRC covers ONLY the TOC bytes.
   `Open` validates magic/header/footer agreement + the TOC CRC +
   the structural range checks: no multi-GB walk at boot.
   `VerifyAll()` is the EXPLICIT whole-image policy — the deployment
   gate (`DeployCheck`) calls it, the runtime never does.
3. **Stable slots** — every entry carries a `slot` ordinal assigned
   at cook and preserved across `AppendPakBlobs` grows (a superseded
   entry's replacement KEEPS the old slot). The compiled-plan
   addressing of Phase 6 keys pages by slot, never by TOC position.
4. **The cooked mesh plan** — one `PakAssetKind::Plan` blob per pak
   at the reserved path `"#meshplan"` (`kPakMeshPlanPath`): the
   family -> member join the COOK resolved (which Mesh blobs make up
   each .geometry family, with LOD indices). Emitted by
   `SerializeClusters` (grouping by the URI base before `#`, the LOD
   index parsed from the `/lodN` suffix). The runtime join
   (`AssetStore::FamilyMembers` -> `PakReader::MeshPlanFamily`) is a
   plan lookup — **never a TOC string-prefix scan** ("resolve the
   ambiguity while cooking; no runtime name search"). A missing
   plan/family is a version skew: loud failure, recook.

## The file-backed open (`PakReader::OpenFile`)

The container primitive the async pager builds on: reads ONLY the
header + footer + TOC from the file; every `Payload()` fetch is a
positional page read (verify + cache, lifetime-stable spans like the
image-backed form). A file-backed reader has NO resident image
(`HasImage()` false, `ImageData()` null) — image-relative consumers
(the direct-addressed mesh fast path) use the image-backed `Open`
until the pager replaces the preload path.

## What is deliberately NOT here yet

- **Shared physical pages** (one 64/256 KiB page holding data from
  several assets): the entry IS the page unit this generation. The
  packing-policy work (scene locality, LOD grouping, mip tails) is
  gated on the A/B/C profile the design requires — measure before
  finalizing the packing.
- **The scene lane on the pager**: cluster loading still uses the
  whole-image preload (the direct-addressed mesh fast path needs a
  contiguous image base). Moving it is the subset fast path /
  permanent shared-mesh store work (Phase 7 residue) — the pager
  already serves every VFS-addressed asset read (see below).

## The compiled static plans (Phase 6 — landed)

`Engine/Asset/asset_plan.h` + the private generated
`project_plans.h` + the fail-closed runtime gate:

- **`smvk::MeshPlanRef`** — the compiled plan type: the pak MOUNT
  INDEX + the family's BASE pak SLOT + the .geometry identity. The
  SMP8 slots ARE the stable page addresses; the plan never carries a
  TOC position, a runtime path join, or the mesh-plan join itself.
- **The private `project_plans.h`** (`EmitPlansHeader`, wired into
  the ids pass + the build re-stamp — the strict mode's
  cook-precedes-plan-generation order): one `MeshPlanRef` constant
  per mesh asset + `PlansPakCount` + `PlansLayoutHash`. PRIVATE by
  design — never the game-facing `project.h`; the game includes it
  only at plan-driven call sites. Empty form on fresh trees (the
  gate refuses any deployed pak against `PlansPakCount 0`);
  inconsistent publishes and unknown families fail the generation
  loudly.
- **The layout hash** — ONE shared fold (`PakLayoutFingerprint` +
  `FoldPakLayouts`, engine-side so the generator and the gate can
  never drift): the pak list (mount order + names) + each pak's slot
  table (slot, kind, codec flags, length, path hash — SLOT order) +
  the cooked mesh-plan join verbatim. **Content bytes are excluded**
  (the per-page contentCrc owns them): a payload-only redeploy with
  an unchanged layout needs NO recompile — the acceptance contract.
  Length/slot/kind/join changes move it — rebuild the game.
- **The fail-closed gate** — `ProjectRuntime::AttachCompiledPlans(
  PlansPakCount, PlansLayoutHash)`: opens every manifest pak through
  the light mount, folds, compares. A count or fold mismatch is a
  LOUD abort — redeploy or rebuild; NEVER a silent fallback to the
  dynamic resolver.
- **The plan load lane** — `AssetManager::LoadMeshByPlan` /
  `AssetStore::AcquireFamilyClusterByPlan`: the pak by MOUNT INDEX
  (no name lookup), the slot validated against the deployed TOC
  through the O(1) slot index (`PakReader::FindBySlot` — no walk),
  per-asset fail-closed on kind/family mismatch, then the SAME
  ownership machinery as the catalog lane (shared, never duplicated).
  Dedup keys by the plan identity (pakIndex, baseSlot).

Pinned by `planfp.*` (test_assets.cpp: fold determinism,
content-exclusion, length/plan sensitivity, the list fold),
`plansgen.*` (test_project_pipeline.cpp: the emission, the
hash-matches-gate-fold identity, content-only-keeps-hash,
layout-change-moves-hash, the refusals) + `planlane.*`
(test_pak_host.cpp: the gate's three outcomes, the plan load +
dedup + spawn, the wrong-slot and bad-index fail-closed refusals).

## The async pager (`smvk::PakPager`, Phase 5 — landed)

`engine/include/Engine/Asset/pak_pager.h` — the scheduling + residency
layers over the SMP8 container contract:

- ONE background worker over a persistent file handle; every fetch is
  a positional page read (verify + LZ4-decode) performed OUTSIDE the
  pool lock — a slow read never blocks a poll or a pin release.
- Demand reads jump queued PREFETCH entries (prefetch is early work,
  demand owns latency; **prefetch is never a guarantee** — the later
  `Read` is the contract).
- In-flight/resident pages are DEDUPLICATED by slot — concurrent
  requests attach to one disk read.
- A BOUNDED byte-budget pool (default 64 MiB) with LRU eviction of
  UNPINNED pages only; a `PinnedPage` holds its page for its
  lifetime (the pool never invalidates a live span). The pager
  outlives its pins (the standard owner rule).
- Cancellation for QUEUED prefetches (never in-flight, never demand);
  a demand read on a cancelled page un-cancels it; `Acquire` of a
  dead/evicted record re-enqueues on demand.
- Per-page failures are loud + terminal (the SMP8 lazy verification
  surfaced: a corrupt page fails AT the page, its neighbors stay
  servable).

**The selective runtime lane (landed):** `ProjectRuntime::ReadAsset`
reads through the pager whenever the pak has no backend in the cache
(a scene-loaded pak serves from its already-resident image — one copy
of the bytes whatever opened first). An asset read on a pak nobody
preloaded does ONE positional page read: no backend is created, no
whole-image preload runs, no blocking preload future — the
small-loads acceptance contract, pinned by `prt.pager-*` in
test_pak_host.cpp (`diskReads == 1`, `bytesRead < fileSize`,
`HasPak == false`).

Pinned by `pager.*` in test_assets.cpp: payload parity with the
image-backed reader (LZ4 + plain), the dedup, the pin/eviction
lifecycle, prefetch-satisfies-demand, the cancel contract's
deterministic sides, the corrupt-page failure + clean sibling, the
missing/garbage refusals, and 8-thread concurrent demand (every page
read exactly once).

## Pinned by

- `smp8.*` (test_assets.cpp): the plan codec round trip + malformed
  refusals, slots/`FindBySlot`, the file-backed open (payload parity,
  no-image contract, missing-file refusal), the lazy page contract
  (mount tolerates a corrupt page; the fetch throws; `VerifyAll`
  names it; the sibling page stays clean), the SMP7 dead-epoch
  rejection (image + file forms).
- `pak.corrupt.*` / `paklz4.corrupt.*` (test_assets.cpp): the
  corruption tests moved from "Open rejects" to the page contract
  (mount-tolerant + fetch-throws + VerifyAll).
- `pak.bounds.*` (test_assets.cpp): the forged-image fuzz corpus on
  the 44-byte SMP8 entry layout, including the deliberate tightening
  (the TOC must end EXACTLY — slack bytes are rejected).
- `cook.pak.plan*` (test_assets.cpp): the cook emits the plan;
  unknown families refuse loudly.
- `hfield.pak-smp7-rejected` (test_terrain_hfield.cpp): SMP7 joins
  the dead-epoch forgery battery.
- `pakcorruption.torn-*` + `am.mesh-no-plan-refused`
  (test_pak_host.cpp): the torn page through the backend; the
  plan-less family refuses through the whole facade stack.
- Every cooked project launch (the fresh sponza pak: 408 entries =
  407 + the plan, verify OK, renders byte-comparable, boot cost
  unchanged).
