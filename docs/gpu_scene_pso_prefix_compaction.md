# GPU-scene PSO compaction: atomics-free count / prefix / scatter

## Scope

This draft introduces a standalone **four-PSO**, atomics-free GPU
compaction pipeline and a C++ oracle for verifying command destinations.
It does **not** replace the active renderer or switch rendering to
`vkCmdDrawIndexedIndirectCount` yet. No changes to double-sided culling
or the existing raster PSO classes are made.

This is intended to replace the wave-aggregated global-atomic append
experimental shader in draft PR #296 before that branch is enabled.

## Stages

1. `gpu_scene_pso_prefix_count.cs.hlsl` dispatches one 256-thread workgroup
   per chunk of work. Every thread reads at most one work record and checks
   persistent geometry through **master's per-flight alias table**.
   Participating waves count valid work per class and reduce those counts
   to four `uint` entries per block. No global atomic reservations.
2. `gpu_scene_pso_prefix_scan.cs.hlsl` runs four independent
   exclusive scans, each with its own thread, over the block totals.
   It publishes one block base per class and four exact GPU draw counts.
   It also records invalid layout / capacity overflow. The host must
   submit no compact ownership if any overflow is reported.
3. `gpu_scene_pso_prefix_scatter.cs.hlsl` re-validates work, calculates
   local class rank using wave prefix and preceding-wave counts, then
   writes exactly one indirect command at:

   `binOffset[class] + blockBase[block][class] + precedingWaveCount + waveRank`

   `firstInstance` stays equal to the **original GPU work index**,
   preserving the existing vertex shader's generation check.

## Allocation and CPU budgets

The four `binOffsets` and `binCapacities` come from a matching
per-flight **conservative** CPU scene budget (e.g. the bookkeeping
introduced in draft #296). They may point into one contiguous Vulkan
buffer or four disjoint buffers with an adapter; this shader currently
expects one command buffer with compact non-overlapping ranges.

Each block-base scan is exact for this frame. CPU capacities are an
upper bound, not GPU-visible draw counts. Draw-count values come from
the GPU scan and feed `vkCmdDrawIndexedIndirectCount`.

There is no global atomic **append** in any successful path. One
`InterlockedOr` is used by scatter solely on detected overflow/error.
This can be removed if a separate debug buffer is desirable.

## Required runtime integration (not included)

- Wire CPU conservative per-class capacities to the exact acquired
  per-flight snapshot and check CPU publication / source revision.
- Allocate block counts and block bases (max 4096 × 4 entries each),
  GPU class counts / overflow diagnostics, and compact indirect buffer.
- Dispatch count → scan → scatter, with compute memory dependencies
  between passes and compute→draw-indirect/read dependencies afterward.
  Scan and scatter both read `workOffsets[candidateCount]` for the
  current frame. Work/alias/geometry tables must be published first.
- Bind each of the four matching graphics pipelines and draw the
  corresponding region via indirect-count, using its GPU count.
- Convert temporal seed, depth remainder, Hi-Z and history consumers
  from the existing 4× fixed-stride indexing to compact command ranges.
- Define **same-frame no-dropped-draws behavior** for any CPU-budget
  or GPU-overflow failure before enabling the new path. Merely flagging
  overflow and clamping counts is *not* a valid fallback.
- Validate GPU shader compilation, shader reflection / C++ push ABI,
  Vulkan usage flags and barriers, view/flight retirement, and parity.
  Compare total frame time to both fixed 4× and wave-append variants,
  especially opaque-heavy scenes where global append contention is worst.

## Validation

`tests/test_gpu_scene_pso_prefix.cpp` exercises deterministic stable
ordering, 256-item block boundaries, skewed distributions, invalid work,
class-specific capacity headroom, and all-or-nothing CPU oracle failure
when capacity is insufficient. CI must compile the three HLSL kernels
and the C++ oracle. GPU performance correctness is not claimed here.
