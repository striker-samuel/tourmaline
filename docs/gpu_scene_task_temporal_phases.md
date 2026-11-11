# GPUScene task/mesh temporal seed phases — staging PR

## Scope
This change implements the **task-stage shader phase predicate**, CPU ABI and
device-free parity oracle for splitting a task-group stream by previously
visible persistent instance (slot,generation) identities.

**This does not activate task/mesh temporal visibility in WorldRenderer.**
The default phase is 0 (draw everything), preserving the existing raster path.
It must remain draft until the host records the correct seed/remainder passes.

## Phase contract
- Phase 0: current task groups, unchanged. No visibility-history reads.
- Phase 1: previously visible current task groups only. Seed raster uses
  no Hi-Z test: it is the source of this frame's occluder pyramid.
- Phase 2: current groups not in phase 1. These are tested against the
  seed-only Hi-Z, after the pyramid has been generated.
- Both phases use the current work row's stable instance slot + generation.
  Meshlet/work indices and draw order are not visibility identities.
- Host requires a *completed prior submission* snapshot and enough
  history slots for every current instance. If unavailable, run phase 0.
- Do not mix phase 1 and phase 2 in a single full draw: that duplicates
  raster work. Record an explicit seed-depth pass, Hi-Z build, remainder
  pass and complete Main draw with the proper depth load semantics.
- A phase split alone is not a temporal raster cutover. In particular,
  the existing task mesh parameter buffer is still produced once per
  flight; the future pass integration must create/bind separate phase
  parameter snapshots and synchronize them against GPU reads.

## Other limitations
- History remains instance-granular, **not** meshlet granular.
- The existing raster history GPU producer only runs in indexed mode.
  Task mesh needs its own compatible history producer, with conservative
  generation invalidation, at the end of the task-path visibility chain.
- This PR does not reduce the task-count/prefix/scatter overhead.
- No GPU capture, DXC reflection, or Vulkan parity measurement is claimed.
- No feature flag is exposed; WorldRenderer continues using mode 0.

## Validation
`GpuSceneTaskMeshTests` now covers previous-visible seeding, new work,
generation reuse, invalid history slots and exact seed/remainder partition.
Run these plus shader compilation and Vulkan scene parity before enabling
phase 1/2 in production.
