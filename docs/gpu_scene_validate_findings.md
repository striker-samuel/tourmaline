# GPU-scene validation lane: all-zero GPU verdicts on master (device fault pre-#275)

Status: ROOT CAUSE FOUND + FIXED + GPU-VERIFIED on this branch (found and
fixed 2026-11-08; the eligible-scene raster-parity gate below remains open).
Tracking note — the repository has issues disabled, so this document is the
record. Remove it when the last open gate (an actually eligible
static-opaque scene under `TOURMALINE_GPU_SCENE_RASTER=1`) is covered.

## Summary

The opt-in GPU-scene validation lane (`TOURMALINE_GPU_SCENE_VALIDATE=1`) does
not work on master: the GPU instance-cull verdict readback is **all zeros** on
every scene tested, while the CPU reference is sane. On the pre-#275 merge
base (`bb568cea`) the same lane is worse — it **device-faults** inside the
`GpuSceneValidate` pass. The lane appears to have never produced a working GPU
cull in the configurations tested; PR #275 only changed the failure mode
(device fault → silent all-zero verdicts). This is **not** a #275 regression
— recorded here so it does not get misattributed later.

## Repro A — master @ 27ba8790 (merged #275), silent all-zero verdicts

Cook the bundled sample project and run the viewer with the validation pin
(version-matched cook, built from master):

```powershell
cd out/build/x64-Release
Copy-Item -Recurse samples/04_project_game $env:TEMP\game_proj
.\Project.exe build --project $env:TEMP\game_proj\project --recipe $env:TEMP\game_proj\project\game.tourrecipe --publish $env:TEMP\game_publish
$env:TOURMALINE_GPU_SCENE_VALIDATE='1'; $env:TOURMALINE_AUTO_EXPOSURE='0'; $env:TOURMALINE_AA='off'
.\App.exe --project $env:TEMP\game_publish\game.tapp --disable_control --bench 130
```

Observed (sample project, 1 candidate):

```
[GpuSceneValidate] mismatch candidate=0 gpu=(0,0,0,0) cpu=(0,1,1,0)
[GpuSceneValidate] flight=1 compared=1 mismatches=1 total=1/1
[GpuSceneCompact] visible count gpu=0 cpu=1
[GpuSceneWork] count gpu=0 cpu=1
[GpuSceneIndirect] groups gpu=(0,1,1) cpu=(1,1,1)
```

Same on single-Sponza (299 meshes, the `sponza_single` recipe with the two
authored GI volumes re-baked fresh):

```
[GpuSceneValidate] mismatch candidate=0 gpu=(0,0,0,0) cpu=(0,1,1,0)   (first 4 of 103)
[GpuSceneValidate] flight=1 compared=103 mismatches=103 total=206/206
[GpuSceneCompact] visible count gpu=0 cpu=72
[GpuSceneWork] count gpu=0 cpu=72
[GpuSceneIndirect] groups gpu=(0,1,1) cpu=(2,1,1)
```

Every GPU verdict tuple is `(0,0,0,0)` — the verdict buffer reads back as
never-written. The instance-cull dispatch is recorded and `MarkDispatched`
runs (the comparison fires rather than reporting "skipped"), so the compute
either never writes or writes somewhere the readback does not look (verdict
BDA / buffer binding suspicion). The CPU reference is plausible in both cases
(1/1 and 72/103 visible).

Mixing `TOURMALINE_GPU_SCENE_RASTER=1` with `VALIDATE=1` produces the same
result, as does validation alone.

## Repro B — merge base bb568cea (pre-#275), device fault

Base built in a clean worktree, sample project cooked with the base
`Project.exe`, same pins:

```
[DeviceFault]   stage=1 frame=1 sortedPos=0 pass='GpuSceneValidate'
[DeviceFault]   stage=8192 frame=1 sortedPos=0 pass='GpuSceneValidate'
```

The base run loses the device in the validation pass at frame 1 (marker
captured via the diagnostic checkpoints), so the GPU cull compute was already
broken before #275 landed.

## Environment

- Windows 11, NVIDIA GeForce RTX 5070 Ti, driver 617.42
- Vulkan SDK 1.4.357.0
- master @ 27ba8790 (PR #275 merged) and merge base bb568cea, both
  Release/Ninja, MSVC
- Scene paks cooked fresh with the matching engine build in both cases
  (cross-version cook/render is rejected by the scene-blob gate, so both
  pairs are version-matched)

## Scope / impact

- Production rendering is unaffected: with `TOURMALINE_GPU_SCENE_RASTER=1`
  the conservative whole-world eligibility gate keeps every scene tested
  (Sponza single: cooked LOD chains + masked materials; the sample project)
  on the legacy Main path, and default runs are pixel-identical with/without
  the pin (0.02% dither noise, avg delta 0.11/255).
- The broken lane is the GPU instance-cull verdict compute itself, which the
  #275 raster work-build chain (`PrepareCompaction` /
  `PrepareWorkExpansion` / `PrepareRasterCommands`) builds on — so the
  GPU-driven Main cutover remains unproven on real GPU output until this is
  fixed.

## Suspicion area

`engine/include/Engine/Renderer/gpu_scene_validation.h`
(`CandidateValidator::Prepare` / `BeginFlight` readback, verdict buffer BDA
in `InstanceCullGpuPush`) and the `GpuSceneValidate` compute dispatch in
`engine/src/Renderer/WorldRenderer.cpp` (~11330–11445). All GPU-side push
fields (`push.ranges`, `push.verdicts`, `push.view`, table BDAs) pass the
nonzero checks in `Prepare`, yet nothing lands in `f.verdicts` two flights
later.

## Root cause (confirmed on GPU 2026-11-08): the InstanceCullGpuPush ABI drifted 8 bytes

Instrumented bisection on the RTX 5070 Ti (instrumentation since removed):

1. The pass recording func executes; the dispatch is recorded with the
   correct verdict BDA, and the later readback reads that exact same
   address. Not a graph/registration/ordering problem.
2. An unconditional magic write at the top of `mainCS` LANDS in the
   readback — the kernel runs and the BDA is good.
3. Dumping the shader's own view of its push block showed `counts.y == 0`
   while the CPU pushed 1, and `counts2.y == 1` (correct) — an 8-byte
   shift on `counts` only.

`spirv-dis` on `gpu_scene_instance_cull.cs.hlsl.spv` is authoritative: the
`[[vk::push_constant]] ConstantBuffer<GpuSceneInstanceCullPush>` block packs
the seven `vk::BufferPointer`s at 0..48 and then `counts` at **56**,
`counts2` at **72** (DXC packs a following `uint4` at the next 8-byte
boundary — it does NOT round up to 16). The C++ mirror
`InstanceCullGpuPush` had `alignas(16)` on `counts`, placing it at 64 with
8 bytes of padding the shader never sees. Result: the kernel's
`counts.x/y` read the CPU padding (zeros) — `candidateCount` was 0 — so
every invocation took the bounds-check early-return BEFORE the unconditional
verdict header write, and the entire compact/work/indirect chain starved on
all-zero verdicts. (The pre-#275 device fault is consistent with the same
class: that era's ABI drift pointed the kernel at garbage BDAs instead.)

This also explains why the graph-registration patch below changed nothing:
it was a plausible-discipline fix, not the root cause.

Audit of every GPU-scene kernel push on this branch
(`spirv-dis` offsets vs the C++ mirrors): `InstanceCullGpuPush` was the ONLY
mismatched mirror — every other push either uses plain `uint32`s after the
pointers (tight both sides) or an EVEN pointer run before a `uint4` (the
16-aligned and tight layouts coincide at 32/64).

## Fix (this branch)

`engine/include/Engine/Renderer/gpu_scene_candidates.h`:
`InstanceCullGpuPush` now packs tight — `counts` at 56, `counts2` at 72,
`sizeof == 88` — matching the SPIR-V block exactly, with static_asserts
pinning the offsets and a comment recording the DXC packing rule so the
next mirror does not repeat the drift.

## Earlier candidate on this draft branch (2026-11-08, kept)

The BDA-only GPU-scene verdict buffer previously had **no render-graph
write declaration** on the validation-only pass: the existing graph hints
covered GPU raster indirect/work buffers only when full-scene raster was
active. The compute dispatch referenced the verdict allocation through a raw
device address, so the graph could not track its shader-write stage/access.

The patch registers the real per-flight verdict buffer as an external logical
buffer, updates that registration when the physical buffer is replaced,
and declares a Compute/ShaderWrite hint on `GpuSceneValidate`. If graph
registration fails, validation is not marked dispatched. This was NOT the
root cause of the all-zero readback (see above) but is kept as correct
graph-discipline for a BDA write.

The CPU validation readback also explicitly counts all-zero verdicts. The
GPU kernel must write a sentinel invalid slot (`InvalidIndex`) even for a
rejected candidate, so an all-zero tuple means an unwritten/misaddressed
slot, not an invisible instance. CPU-only regression assertions cover
valid visible, culled, rejected and all-zero cases.

### GPU acceptance checks

1. Compile shaders and the renderer, and run the GPU-scene C++ tests.
2. Run both version-matched repros above on the RTX 5070 Ti with Vulkan
   validation enabled and `TOURMALINE_GPU_SCENE_VALIDATE=1`.
3. Require **zero unwritten verdicts and zero CPU/GPU mismatches** in both
   the single-instance sample and Sponza, across multiple flight reuses.
4. Require matching compact-visible counts, expanded-work counts and
   indirect-dispatch groups. Confirm the absence of device loss and
   validation-layer errors.
5. Repeat under `TOURMALINE_GPU_SCENE_RASTER=1` separately, including
   an actually eligible static-opaque scene; verify visual parity and
   that the opt-in does not hide a failing compute path behind legacy.
6. Only after these gates pass, remove this tracking note, record the
   evidence in the PR and mark it ready for review.

### Acceptance results (RTX 5070 Ti, driver 617.42, 2026-11-08, this branch)

- C++ suites: `gpu_scene_*` 6/6 + `architecture` + `text_integrity` green.
- Sample project (`VALIDATE=1`, bench 130):
  `compared=1 mismatches=0`; compact/work/indirect all agree (1/1).
- Single-Sponza (`VALIDATE=1`, bench 260, two validator samples):
  `compared=103 mismatches=0 total=0/206`; compact-visible 72, work 72,
  indirect groups agree — the GPU cull now matches the CPU oracle exactly
  (72/103 visible). No unwritten verdicts, no device loss.
- Mixed `RASTER=1` + `VALIDATE=1` (Sponza, bench 128, capture 48):
  `mismatches=0`, frame renders, no fault (~9.2 ms vs 8.8 ms baseline —
  the documented diagnostic-readback stall).
- Release builds run without the Vulkan validation layer (it is
  NDEBUG-gated); the layer-enabled pass of gate 2 is still open, as is
  gate 5's actually-eligible static-opaque scene (Sponza keeps the raster
  lane in its conservative legacy fallback: cooked LOD chains + masked
  materials). Until those are covered, keep this PR draft and do not
  promote GPU-driven Main to default.
