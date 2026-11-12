# Tourmaline Nsight build profiles

Create **separate out-of-source Ninja build directories**, one per profile:

```sh
cmake -S . -B out/build/x64-Shipping -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_BUILD_PROFILE=shipping
cmake -S . -B out/build/x64-Dev -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_BUILD_PROFILE=dev
cmake -S . -B out/build/x64-Diagnostics -G Ninja -DCMAKE_BUILD_TYPE=Release -DTOURMALINE_BUILD_PROFILE=diagnostics
```

| Profile | Optimized code | Optick runtime | Nsight Vulkan pass labels | Shader debug info | Distribution |
| --- | --- | --- | --- | --- | --- |
| shipping | yes | no | no | no | allowed after artifact checks |
| dev | yes | no | yes | no | internal |
| diagnostics | yes | yes | yes | yes | **private only** |
| custom | existing controls | existing controls | opt-in | existing controls | audit manually |

`dev` retains fast iteration and annotated command buffers without shader
debug line maps. `diagnostics` uses existing `-O3 -Zi -fspv-debug=vulkan`
shader compilation and is only for internal investigation. In Nsight Graphics,
open a GPU trace or frame capture using `tools/perf_harness.py` and the
`Benchmark` target. The labels use `VK_EXT_debug_utils` when available,
including when Vulkan validation is disabled.

## Shipping security boundary

Ship binaries, packed assets and compiled shaders **only** from the shipping
build tree using an allowlist. Do not ship PDB / split DWARF symbols, `.nvdbg`,
`.nv-gpudmp`, `.opt`, GPU traces, crash logs, source HLSL, CMake caches, or
private debugging SPIR-V. Check **both** loose SPIR-V and `engine-shaders.pak`.
Keep internal symbol files separate from public CI artifacts. Windows builds
should inspect release PE debug-directory references and Linux builds should
strip/copy debug symbols as a private packaging step.

Stripping shader debug metadata reduces exposed source names and line mapping,
but SPIR-V bytecode itself is reverse-engineerable. For particularly sensitive
algorithms, client-side shaders cannot provide complete source secrecy.

## One-command NsightTrace.exe (Windows, dev/diagnostics only)

The `NsightTrace` target is a small CLI **executable**, independent of the
renderer and NVIDIA SDK. It launches the existing stdlib-only
`tools/perf_harness.py` (Python 3 required), preserving the
source-controlled `benchmarks/scenarios.json` setup and safety checks.
The executable is **not built** for `TOURMALINE_BUILD_PROFILE=shipping`.

```powershell
cmake --build out/build/x64-Dev --target NsightTrace
# First save your preferred disk locations in your local user profile:
out/build/x64-Dev/NsightTrace.exe --save-paths --capture-dir "D:/GPUCaptures" --scratch-dir "E:/NsightScratch"
# Review saved preferences:
out/build/x64-Dev/NsightTrace.exe --show-paths
# Dry-run prints exact ngfx invocation without starting GPU tracing:
out/build/x64-Dev/NsightTrace.exe --scenario bistro-ssr-stress --dry-run
# Trace a saved scenario: ten warmup frames, ten trace frames and an exit margin:
out/build/x64-Dev/NsightTrace.exe --scenario bistro-ssr-stress --ngfx "C:/NVIDIA/Nsight Graphics/host/windows-desktop-nomad-x64/ngfx.exe"
# Override the paths for ONE capture without modifying saved settings:
out/build/x64-Dev/NsightTrace.exe --scenario bistro-ssr-stress --capture-dir "F:/OneOffCaptures" --scratch-dir "G:/FastTemp"
```

The launcher finds `tools/perf_harness.py` by walking up from the
executable's build directory. Set `TOURMALINE_PYTHON` to a Python 3 exe
if `python.exe` is not in PATH. `--trace-frames` defaults to 10 (allowed 1–15).
`--trace-duration-ms` defaults to 10000. `--exe` can select a different
Benchmark target; otherwise the executable next to NsightTrace.exe is used.
Scenario input paks/manifests must exist relative to Benchmark's build dir.

Settings live in `%LOCALAPPDATA%/Tourmaline/nsight_trace_paths.json`
(and `~/.config/Tourmaline/nsight_trace_paths.json` for the Python/Linux
entry point), **never in the repository**. The
`--paths-config` option permits a different settings file. CLI directory
arguments override saved settings for that invocation; only `--save-paths`
writes the preferences. Each GPU trace is written into a new
`<capture-dir>/<scenario>-<UTC timestamp>/` directory, with logs and
`result.json` under `<capture-dir>/_runs/` by default. `--output`
overrides the log/report root independently.

**Disk-I/O limitation:** Nsight Graphics GPU Trace has an official
`--output-dir` control for final exports but **no documented dedicated
scratch-directory switch**. Tourmaline supplies the configured scratch
directory through `TMP`, `TEMP` and `TMPDIR` in Nsight's environment,
and to the traced target through `--env` (Linux adds `XDG_CACHE_HOME`).
This redirects normal OS temp usage; it **cannot guarantee** all
NVIDIA-internal caches, the driver shader cache or Windows pagefile I/O
will use the requested disk. Check actual disk traffic when tuning
capture-heavy workloads. The full trace and crash artifacts are
private and should not go to public CI artifacts or bug reports.

To run without building the native wrapper on Linux:
```bash
python3 tools/perf_harness.py trace --scenario bistro-ssr-stress \
  --exe out/build/linux-Dev/Benchmark --trace-frames 10 \
  --capture-dir /mnt/fast-captures --scratch-dir /mnt/nvme-temp
```

## In-app Nsight Graphics SDK (procedural capture/trace)

The header-only Nsight Graphics SDK (vendored under
`external/nsight_graphics_sdk`, Apache-2.0) lets the engine trigger a Graphics
Capture or GPU Trace procedurally, with no GUI/CLI launch. Build it in with
`-DTOURMALINE_NGFX_SDK=ON` (off by default; shipping/custom builds compile the
no-op stubs in `Engine/Utils/nsight_gfx.*` and never see the NVIDIA headers).

The seam is `Engine/Utils/nsight_gfx.h` (`tour::nsight_gfx`), wired into
`App::Run` (Initialize, before the graphics context) and `App::DrawFrame`
(Poll, once per frame). Exactly ONE activity initializes per process, chosen
from the pins:

| Pin | Effect |
| --- | --- |
| `TOURMALINE_NGFX_CAPTURE_FRAME=<n>` | Graphics Capture: request a 1-frame capture at frame n (delimiter = Present) |
| `TOURMALINE_NGFX_TRACE_START=<n>` / `TOURMALINE_NGFX_TRACE_END=<m>` | GPU Trace: start at frame n, stop at frame m |
| `TOURMALINE_NGFX_BOUNDARY=1` | opt into `NGFX_FrameBoundary_Vulkan` marking (headless/non-present workloads) |

Example (capture frame 42 of the duck scene):
```powershell
$env:TOURMALINE_NGFX_CAPTURE_FRAME='42'
& out/build/x64-Release-ShaderDebug/App.exe --pak content/paks/duck131072.pak --scene scene
```

The SDK loads the Nsight Graphics libraries at RUNTIME (the install is found by
the load callback, `NGFX_LoadLib_NoVerification` — dev only), so the build needs
no Nsight install.

## MCP server CLI (`tools/nsight_mcp.py`)

Nsight Graphics 2026.4+ ships `ngfx-mcp.exe`, a Model Context Protocol server
that lets agents inspect captures and analyze GPU traces. The thin
`tools/nsight_mcp.py` wrapper locates it and issues minimal MCP requests:

```powershell
python tools/nsight_mcp.py locate                       # print the ngfx-mcp.exe path
python tools/nsight_mcp.py serve --profile workflow      # launch the HTTP server (127.0.0.1:7661/mcp)
python tools/nsight_mcp.py list-tools                    # query the advertised tools
python tools/nsight_mcp.py inspect out/nsight-sdbg-bistro/App_...ngfx-gputrace --question "list the draw calls"
```

Override the auto-detected server with `--mcp PATH` or `TOURMALINE_NSIGHT_MCP`.
The Workflow profile is a compact capture/analysis surface; the Native profile
(`--profile native`, stdio by default) exposes the broader experimental catalog.
