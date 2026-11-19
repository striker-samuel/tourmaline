# Synthetic hardware profiles

Use a rebuilt dedicated Benchmark executable on Windows or Linux:

```powershell
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --hardware-profile ps4 --output out/perf/ps4
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --hardware-profile ps5 --output out/perf/ps5
python tools/perf_harness.py bench --scenario bistro-ssr-stress --exe out/build/x64-Release/Benchmark.exe --hardware-profile rtx4060 --output out/perf/4060
```

| Preset | Logical CPUs | VMA device-local ceiling |
| --- | ---: | ---: |
| native | unrestricted | unrestricted |
| ps4 | 7 | 4096 MiB |
| ps5 | 14 | 12288 MiB |
| rtx4060 | unrestricted | 8192 MiB |

The console values are intentionally approximate game-side resource
budgets, not official SDK memory reservations. The console unified memory
architecture is not reproduced. The 4060 profile leaves CPU unrestricted
because the graphics card does not specify the host CPU.

Override either limit with `--cpu-limit 4` or `--vram-mb 3072`.
The native profile can also be used with either override. The benchmark
applies CPU affinity before host initialization, selecting logical CPUs
from the existing allowed mask (not necessarily separate physical cores).
The VMA heap-size limit restricts allocator-owned device-local memory
blocks; nonlocal heaps and allocations outside VMA are not capped.
An insufficient host or allocation failure can fail the benchmark.
This is not a paging or unified-memory simulator.

Both priming and measured runs use identical limits. The harness verifies
startup log markers, records the effective configuration, and refuses
comparisons with different profiles. No GPU clock, CPU clock, bandwidth,
GPU compute throughput or FPS limit is implied. To experimentally reduce
GPU throughput, use external supported driver clock/power controls on a
dedicated test machine and record them separately; this harness does not
change global GPU settings or require elevated privileges. An FPS limiter
would not emulate weaker rendering hardware.

CPU-only profile tests: `python -m unittest discover -s tools/tests -p test_benchmark_profiles.py -v`.
Real validation requires a rebuilt Benchmark executable and a GPU.
