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
- **The async pager**: worker reads, coalescing, bounded pools,
  pinning, eviction, cancellation (Phase 5). `OpenFile` + the
  per-page CRCs are its container contract.
- **Compiled static plans**: the private codegen that turns
  slot-keyed plans into compile-time physical reads + the
  fail-closed layout hash (Phase 6).

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
