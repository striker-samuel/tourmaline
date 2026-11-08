# PagerBench: synthetic hardware baseline

A content-free, standalone C++20 CLI to measure the local machine's baseline
memory and transfer behavior before designing a general streaming pager.
It links to the Vulkan loader, but **does not** initialize Tourmaline's engine,
open any .pak, or require the viewer. No external benchmark library is needed.

Build (from the repository root, with the normal Tourmaline prerequisites):

```sh
cmake -S . -B out/build/pager-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/build/pager-release --target PagerBench
./out/build/pager-release/PagerBench --help
./out/build/pager-release/PagerBench --mode all --sizes-mib 1,4,16,64 --warmup 8 --iterations 100 --json > pager-baseline.jsonl
```

On Windows, run `out/build/pager-release/PagerBench.exe`.
Run on the target machine, with a Release build, on AC power and with minimal
background load. Compare repeated independent runs, not just one number.

## Tests

| Name | Operation | What the timer includes |
| --- | --- | --- |
| `cpu_memcpy` | CPU buffer to CPU buffer | memcpy call and CPU wall duration |
| `disk_write` | Write synthetic file and flush C++ stream | buffered OS write, **not** durable fsync |
| `disk_read` | Read a synthetic file | buffered OS IO, **may be entirely page-cache hits** |
| `gpu_upload` | Host-visible coherent staging buffer → device-local VkBuffer | CPU command recording, queue submit, fence wait |
| `gpu_readback` | Device-local VkBuffer → host-visible coherent readback buffer | CPU command recording, queue submit, fence wait |

Every sample uses a fresh wall-clock measurement; warmups are excluded.
Outputs include bytes, iterations, mean, p50, p95, min/max milliseconds
and effective MiB/s based on the **arithmetic mean sample duration**.
The JSONL format has `schema: 1` and includes `timing` to prevent
mistaking end-to-end fence latency for pure GPU execution time.
Readback contents are checked for basic correctness.

Flags:
- `--mode cpu|disk|gpu|all` (default: all)
- `--sizes-mib 1,4,16,64` (default, each size 1..1024 MiB)
- `--warmup 4`, `--iterations 24`
- `--json` emits one JSON object per test and size (JSON Lines)
- `--disk-path FILE` uses the chosen path; default uses a uniquely named
  auto-removed file in the OS temporary directory. Explicit paths are
  **overwritten** and deliberately left in place.

### Caveats / pager design implications

These are **synthetic component microbenchmarks**, not measurements of
Tourmaline's actual asynchronous streaming pipeline. GPU upload/readback
values include a submission/fence round trip, so small-transfer latency is
especially sensitive to CPU overhead. The chosen Vulkan physical device is
the first transfer-capable one and the memory allocation is DEVICE_LOCAL,
which on integrated/UMA systems can be host memory. Dedicated transfer queue
selection, PCIe isolation, calibrated GPU timestamps and multi-GPU choice
are future extensions.

CPU memcpy results depend on cache capacity and CPU frequency. Buffered
disk reads may hit the filesystem cache and cannot be called cold-storage
bandwidth. Disk writes do not enforce physical durability and cannot be
called sustained device write throughput. Do not use these figures as raw
NVMe bandwidth or guaranteed VRAM ceilings.

For paging decisions, sweep small pages (1 MiB and below in a future
fine-grained extension) and large batches separately. Choose a page size
and in-flight limit from **p95 latency**, throughput plateaus and observed
VRAM headroom rather than simply maximizing headline MiB/s.

This CLI intentionally keeps production integration (resource leases,
asynchronous IO, GPU timestamps, cancellation, per-page staging reuse,
dependency scheduling) outside the synthetic hardware baseline.
