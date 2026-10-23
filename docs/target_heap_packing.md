# Target-heap packing (designs A + B)

The packed-memory system behind the engine's render targets: what
"packing" is, the two implementations, the contracts every consumer must
hold, and how to read the diagnostics (the `[TargetHeap]` /
`[TransientHeap]` stderr lines) — plus the `03_fuzz` probe that
regression-tests the whole surface.

Everything here lives in `engine/src/Vulkan/impl.cpp` unless noted.

---

## 1. The problem packing solves

A frame's render targets (depth, HZB, HDR, post buffers, the world
renderer's whole chain) are the big DEVICE-LOCAL consumers after the
mesh heaps: a 1080p frame carries tens of MiB of targets, and a target
set that never reuses memory pays that footprint **per target** — a
depth buffer, a separate HZB depth, an HDR buffer, motion vectors,
bloom mips... each a standalone `vkAllocateMemory`-backed image.

Two of those targets are frequently **the same memory**: a
`renderDepth` (written by the depth prepass, read by the tonemapper)
and a `hizDepth` (written by the occluder prepass, consumed by the HZB
pyramid) never live at the same time *within a frame*, and never live
at all *across* frames (transient by definition). Packing lets
same-shaped targets with non-overlapping **use windows** share one
packed memory range, and lets a freed range be reused for the next
tenant — the transient footprint becomes a rolling working set instead
of a sum.

Packing is implemented twice, for the two ownership models the engine
has for targets. Both share the same foundation.

---

## 2. The shared foundation

- **Heap images are created UNBOUND.** `CreateHeapImage` (impl.cpp)
  does `vkCreateImage` with `transientHeap = true` and **no** VMA
  allocation — the image gets memory only when a packed range is bound
  to it (`BindHeapImage` → `vkBindImageMemory2` with the block's
  `VkDeviceMemory` + the range offset). This is why heap images are
  not VMA images: their allocation IS the packed range.

- **Blocks are real device memory with VMA virtual bookkeeping.**
  Each block is one `vkAllocateMemory` (`kTargetHeapBlock` = 64 MiB
  default, `TOURMALINE_MEM_TARGET_BLOCK_MB` to override, halving
  ladder down to the request on allocation failure — the OOM policy is
  a loud abort, never a silent null). The *virtual* block
  (`vmaCreateVirtualBlock`) is pure CPU bookkeeping over that memory:
  `vmaVirtualAllocate` hands out aligned offsets, `vmaVirtualFree`
  returns them for reuse. First-fit placement; a freed offset is
  reusable by the next claim.

- **Shape identity.** Two images may share one range only when EVERY
  creation parameter matches except the debug name — type, extent,
  format, usage, samples, mips, layers, cubemap/concurrent flags. The
  shape is hashed (`RgTransientShapeHash` for design B, the FNV-style
  mix inside `ClaimTargetHeapRange` for design A); mismatched shapes
  never share, regardless of windows.

- **Disjoint windows.** Sharers must not be live at the same time.
  Design A expresses windows as `TargetPhase` ranges (first..last,
  `Shadows`..`Composite` — the frame's pass order); design B derives
  them from the frame's sorted pass list (see below). Two claims with
  the same shape JOIN one slot only when their windows don't overlap;
  otherwise the newcomer packs to a fresh range. The sharing is plain
  memory aliasing: the second tenant's first barrier transitions from
  `UNDEFINED`, discarding the previous contents — correct, because
  the previous tenant is done by construction.

- **Retirement ordering (the FIFO pairing).** Destroys of packed
  images and their ranges go through the deferred-destroy queue
  (`DeferDestroy`) keyed on the graphics timeline point +
  `MAX_FRAMES_IN_FLIGHT` margin — in-flight frames keep their objects
  alive past their last recorded use. The pairing rules that make
  churn safe:
  1. **Views retire before images.** `DestroyImageNow` asserts
     `liveViewCount == 0`; the queue drains FIFO, so a view destroy
     queued before its image's destroy lands first. (Holder locals
     must therefore be declared image-first/view-second — reverse
     destruction order frees view-then-image. `depth.h`'s `Create`
     and `ReleaseTransientPlacement` follow this.)
  2. **The range free lands BEHIND the image destroy.** The last
     user's release queues `vmaVirtualFree` after the destroy task, so
     a future claim can never rebind a range while the previous
     binding's image object still exists.
  3. **The drain runs every ready task, not just a ready prefix**
     (`ProcessDeferredTasks`) — tasks key on different timelines, and
     relative order among drained tasks is preserved (monotonic retire
     values + a stable partition).

---

## 3. Design A — the TargetHeap (`ClaimTargetHeapRange`)

**Consumers**: host-owned long-lived targets that outlive a frame and
are recreated on extent drift — `DepthTarget` (`depth.h`, the
`renderDepth`/`hizDepth` pair that shares one range: the biggest
aliasing win in the set) and the world renderer's per-extent target
chain via the same claim API.

**The claim** (`ClaimTargetHeapRange(image, firstPhase, lastPhase)`):
takes an unbound heap image + its declared use window. It first tries
to JOIN a live slot (same shape + disjoint window) — the join rebinds
the shared range and the sharer carries a null allocation (the slot
owner frees the range once). Otherwise it first-fits a new range into
a compatible block, growing one if none fits. The claim Holder
(`TargetHeapClaimHandle`) is the lease: destruction calls
`ReleaseTargetHeapClaim`, which drops the live-user count; the LAST
user's release queues the deferred `vmaVirtualFree`.

**The declaration-order contract** (see depth.h's `Create`): the
claim Holder is declared FIRST among the locals so it destroys LAST —
the range free must land behind the image's deferred destroy in the
FIFO. Get this wrong and a churn race can rebind a live range.

**Diagnostics**: one stderr line per claim:
`[TargetHeap] claim #N: shape 0x… window [f..l] -> JOIN slot S /
NEW slot S (block B, offset O, X MiB; ranges R MiB, shared A MiB)`.
`JOIN` lines are the aliasing wins. On a healthy host the offsets
CYCLE (freed ranges are reused); **offsets that march monotonically
mean the deferred range frees are not landing** — see §6.

---

## 4. Design B — the TransientHeap (`CompileTransientHeap`)

**Consumers**: graph-MANAGED transients — images created through
`RenderGraph::AddImage`, where the graph owns the whole lifecycle and
targets exist only as frame-scoped packed ranges. The host never
touches these; it declares passes, and the graph packs whatever its
transient images need per frame.

**The pack** runs at the top of `RenderGraph::Execute`:
1. Collect candidates (live transient logical images; `hostTransfer`
   images stay on the plain VMA path — host-copy binding requirements
   are not honored by manual binds).
2. Derive each candidate's use interval `[firstUse, lastUse]` from
   the frame's sorted pass list.
3. Fingerprint the layout (candidate count + pass count + every
   candidate's shape/interval). Unchanged → steady state, zero work.
   Changed → **detonate and repack once**:
   - detonation destroys the placed views (deferred, FIFO-ordered
     before their images), releases every placement (deferred image
     destroy + deferred range free, pairing per §2), then re-resolves
     fresh physical images and binds them into new packed ranges —
     same-shape targets with disjoint intervals share a slot
     (first-fit interval scheduling, "a slot whose occupant finished
     before this image starts is reusable").
4. `CollectPassUsages`/`TopologicalSort` are recomputed after a
   detonation (fresh physical handles).

The key property: a layout change costs exactly ONE repack, not a
per-frame churn — the fingerprint keeps steady-state frames free.

**Diagnostics**: one stderr line per pack:
`[TransientHeap] pack #N: I images / B buckets / K blocks, R MiB
ranges vs U MiB union (S MiB interval-shared)` — `S` (ranges minus
union) is the aliasing win; `(repacked: layout changed)` marks
detonations.

---

## 5. The bind-before-view contract (and the bug that established it)

**A VkImageView over a heap image must be created AFTER the image's
packed-range bind.** Views over unbound images are spec-legal to
*create*, but a render pass touching one faulted the device on its
first submit — deterministically, on the very first pack or repack
(commit 9335a9a7). `RenderGraph::Execute` used to resolve views
before `CompileTransientHeap` bound the transients; the fix moved
`ResolveImageViews` below the pack. The safety argument: the usage
collection and topological sort that feed the pack read only the
logical views' backing **images** (never the resolved view handles),
so nothing above the pack needs views — only the pass recording below
does, and it now sees post-bind views exclusively.

`depth.h` (design A) had this order from the start; commit 9335a9a7
made it the engine-wide order. **Any new heap consumer must bind
before creating views** — this is the contract the `03_fuzz` `graph`
mode regression-tests (it died in ~3 seconds before the fix; it now
survives 600 every-frame repacks).

---

## 6. Reading the diagnostics + the 03_fuzz probe

`samples/03_fuzz` is the minimal reproducer/regression host for this
whole surface — one mode per mechanism:

| mode | exercises |
|------|-----------|
| `rhi` | the inline-target churn pattern: private image + view + bindless + graph-external logical, recreated at a toggling extent through the deferred-retire Holder swap |
| `graph` | design B end-to-end: `AddImage` transients recreated at a toggling extent + a clear pass + Execute — first packs, detonations, repacks, per-frame interval scheduling (the 9335a9a7 regression) |
| `claims` | design A end-to-end: the `DepthTarget` churn pattern verbatim — `CreateHeapImage` + `ClaimTargetHeapRange` + view + bindless, Holder-swap retired per toggle |
| `swap` | swapchain `Recreate` spam (the user-resize path) |
| `all` | everything at once |

Knobs: `FUZZ_FRAMES` (default 600), `FUZZ_EVERY` (the recreate
cadence; 1 = every frame — the hardest), `FUZZ_SWAP_EVERY` (0 = off;
137 is a resize-ish cadence), plus the bisect knobs `FUZZ_GRAPH_ONLY`
/ `FUZZ_ATTACH_RHI` / `FUZZ_HOST_TRANSFER`. Verdict: `[FUZZ] PASS` on
a clean exit; the engine's own loud paths report failure.

**The offsets tell the story.** While a mode churns, watch the
`[TargetHeap] claim` / `[TransientHeap] pack` lines:

- **Cycling offsets / JOIN slots** — the deferred frees land; the
  rolling working set is bounded. Healthy.
- **Marching monotonic offsets** (every claim a NEW slot, the low
  ranges never reused) — the deferred range frees are NOT landing;
  the block(s) grow until something breaks. This is the signature to
  check whenever a host dies around a suspiciously consistent claim
  count.

**Known open item** (tracked here so the signature stays documented):
the *editor's* WYSIWYG world-chain churn still dies around the ~33rd
claim with marching offsets, while `claims` mode passes 620 claims
with cycling offsets in the minimal host — the delta is the editor's
specific frame flow (the world renderer claiming inline between the
editor's two graph rounds), and it is the active investigation. The
probe pair (editor churn on one side, `03_fuzz` `claims` on the other)
bounds it from both sides.

**A host lesson the fuzzer paid for**: a custom `DrawFrame` override
must replicate the base pump's resize handling
(`if (FramebufferResized()) { Recreate(); ClearFramebufferResized();
re-import views; }`) — otherwise the WM's boot-time window settle
rebuilds the swapchain under stale imported swap views, and recording
a destroyed image is a DEVICE_LOST that looks exactly like a packing
bug. The sample carries this fix; keep it when copying the skeleton.
