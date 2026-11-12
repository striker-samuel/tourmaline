# Milestone: current-master hardware and renderer acceptance

**Status:** validation required; do not assume historical bugs remain or are fixed.

## Work remaining
- [ ] Reproduce #234 near/far packed-grid sky-only report on current master using synchronized complete indirect commands/counters; if absent, archive a fixed capture and close the regression. If present, isolate the cause before proposing code changes.
- [ ] Compare GPU-scene static raster vs legacy fallback across masked A2C, double-sided depth/shadow, G-buffer, Hi-Z, temporal history and material edits.
- [ ] Exercise sparse LOD residency and promotion/eviction, camera motion/hysteresis and multiple frames in flight.
- [ ] Capture real Map/World tile switch and GPU-fence retirement on representative hardware.
- [ ] Capture mesh-enabled and `TOURMALINE_MESH=0` indexed rendering, grid/grid32, with visually complete scene output.
- [ ] Record test GPU/driver, build SHA, native build/CTest/DXC results, validation-layer diagnostics, image comparisons and timestamps.
- [ ] Isolate any observed reproducible bug in a new code-focused PR with a failing regression fixture.

## Acceptance
The evidence matrix either passes or links each reproducible failure to a specific follow-up. No benchmark is accepted if its captured scene is incomplete.

## References
- [Current TODO](../../TODO.md)
- [Far-view investigation](../renderer/gfxfilter_far_view_investigation.md)

This draft contains the acceptance plan only. It does not claim hardware tests were run.