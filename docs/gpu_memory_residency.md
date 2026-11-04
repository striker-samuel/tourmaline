# GPU memory pressure and suspected spill warnings

Tourmaline already reports VMA heap usage and budget through
`smvk::ResourceManager::GetGpuMemoryInfo()`. The new opt-in polling API
`PollGpuMemoryWarning(elapsedSeconds)` classifies those heap samples
using `GpuMemorySpillDetector`.

## Semantics

- **Healthy:** budget is known and no sustained pressure is observed.
- **Pressure:** >=85% of local budget for at least two seconds.
- **Critical:** >=95% of local budget, immediately.
- **SpillSuspected:** sustained >=85% local pressure plus >=64 MiB
  non-device-local heap growth relative to the observed low-water mark.
- **Unavailable:** no local budget is reported.

The detector applies a five-second, below-80% recovery window. The
state machine is side-effect-free except for its internal counters and does
not allocate GPU objects. It is safe to include in shipping builds;
**poll at approximately 1 Hz**, log on state transitions, and rate-limit
telemetry. An example caller:

```cpp
auto warning = resourceManager.PollGpuMemoryWarning(1.0);
if (warning.changed) {
    // Send warning.state and warning.localRatio to the diagnostic log.
}
```

Keep the detector attached to a single ResourceManager and call it with
elapsed time measured since the previous poll (not frame delta unless
polling every frame). Do not automatically degrade graphics quality based
on this result alone.

**These are heuristic warnings, not confirmed eviction events.** VMA
reports allocation and memory budget estimates, not GPU memory
residency migration. Some Vulkan heaps share physical memory (UMA);
the nonlocal heap distinction is most useful on discrete GPUs.
Host-visible allocations might be intentional; nonlocal growth does
not prove the OS demoted a local allocation. True WDDM residency/
eviction attribution requires separate platform-specific instrumentation.

Run CPU-only checks with:
`python -m unittest discover -s tools/tests -v`

This PR intentionally does not introduce ETW capture or modify residency
policy. It adds the core warning path and testable API without impacting
renderer allocation behavior.
