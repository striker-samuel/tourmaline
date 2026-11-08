# GPU-scene validation lane: all-zero GPU verdicts on master (device fault pre-#275)

Status: OPEN BUG (found 2026-11-08, post-#275 renderer verification pass).
Tracking note — the repository has issues disabled, so this document is the
record. Remove it when the lane is fixed and re-verified on a real GPU.

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
