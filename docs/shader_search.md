# Incremental AI shader optimization (experimental)

`tools/shader_search.py` runs a bounded series of **whole-file HLSL** rewrite
proposals against the existing Tourmaline benchmark scenarios. It does not
require parameters, macros, or other optimization hooks in the shaders.

## Models

The model endpoint is configurable, and the runner uses only the Python
standard library. Both protocols accept arbitrary model IDs:

- `--provider openai --api-base https://api.openai.com/v1 --model MODEL`: OpenAI-compatible
  chat completions, including third-party gateways and self-hosted servers
  (e.g. a local vLLM/LM Studio server). Export `OPENAI_API_KEY` as needed.
- `--provider ollama --api-base http://localhost:11434 --model MODEL`: native Ollama
  `/api/chat`, no token required by default.

Use `--api-key-env MY_TOKEN_VARIABLE` to select an alternate environment
variable. Tokens are **not** embedded in run reports. Avoid sending
private/proprietary shader code to a remote endpoint without authorization.

## Usage

Build the Release `Benchmark` binary and ensure the chosen scenario's
pak/project is available relative to that executable's build directory.

Supply TWO commands:

1. `--build-cmd`: compiles the edited shader and refreshes the files that
   the benchmark loads, e.g. `cmake --build out/build/x64-Release --target Benchmark`.
2. `--validate-cmd`: **required correctness gate**, exit 0 only when the
   candidate's rendered output passes comparison to trusted baseline images.
   Implement it for the specific shader and scenario(s). For example,
   orchestrate deterministic renders and call
   `python tools/pixel_compare.py ref.png candidate.png --threshold 6 --stride 1`.
   An exit code of 0 from a command that does not actually compare new
   candidate output is NOT a correctness test.

Commands are argv strings, **not shell expressions**. If validation needs
several operations, put them in a separate script and pass that script's argv.
During the command the edited file path is available as
`SHADER_SEARCH_SHADER`, and the report folder as `SHADER_SEARCH_OUTPUT`.
A correctness script must capture a fresh frame for each candidate; don't
compare stale screenshot files. Use deterministic scenario pins, seeds, and
multiple frames if the pass is temporal.

Example (PowerShell; replace the correctness script with your implementation):

```powershell
python tools/shader_search.py `
  --shader engine/shaders/ssr.frag.hlsl `
  --exe out/build/x64-Release/Benchmark.exe `
  --scenario bistro-ssr-stress `
  --build-cmd "cmake --build out/build/x64-Release --target Benchmark" `
  --validate-cmd "python tools/local_shader_correctness.py" `
  --provider ollama --model qwen2.5-coder:14b `
  --iterations 5 --repeat 5 --target-resolution 1080p `
  --reference-screenshot out/references/ssr-1080p.png `
  --candidate-screenshot out/captures/current-ssr.png `
  --output out/shader-search/experiment-001
```

The suggested shader file and validation script above are *examples*: select
a shader that exists in your checkout and supply your own correctness script.

## Numerical screenshot correctness and target resolution

**A scored screenshot comparison is required by default.** Supply a trusted
saved PNG and a path where the `--validate-cmd` script will create a **new**
candidate PNG on every iteration. The runner deletes an old candidate screenshot
before invoking the script, saves a copy and PNG difference map per iteration,
and uses `tools/pixel_compare.py` to compute the numerical metrics.

```powershell
# Additional options to the example invocation above:
--target-resolution 1080p `
--reference-screenshot out/references/ssr-1080p.png `
--candidate-screenshot out/captures/current-ssr.png `
--pixel-threshold 6 `
--max-difference-fraction 0.001 `
--max-average-channel-delta 0.05 `
--output out/shader-search/experiment-001
```

`--target-resolution` uses the existing perf harness `--resolutions` path
and validates that the reported extent matches the requested 16:9 dimensions.
The requested width and height are also exposed as
`SHADER_SEARCH_TARGET_WIDTH` and `SHADER_SEARCH_TARGET_HEIGHT` to the
capture/validation command. That command **must** render at the requested
resolution with matching scenario/environment settings; the runner verifies
the final PNG dimensions but cannot control a custom capture script's camera,
frame, quality settings, or render scale automatically.

`difference_fraction` is the fraction of RGB pixels with sum of channel
absolute differences greater than `--pixel-threshold`. The second metric,
`average_channel_delta`, is the mean sum of RGB absolute differences
over all pixels. Both limits must pass. The numeric result is saved under
`candidate-NNN/visual.json`, included in the experiment result, and fed
to subsequent model prompts. Pillow is required for pixel comparison and is
installed by the optional extended installer.

The default limits are zero (exact pixel match). Choose controlled tolerances
only for scenes with expected nondeterminism and inspect difference maps before
accepting an optimized variant. For genuinely nonvisual workloads,
`--allow-no-image-check` explicitly waives this gate; the required external
`--validate-cmd` still runs. Don't use a no-op validator.

Use a **fresh empty output directory** per search; reruns refuse to overwrite
existing experiment records. On normal exit the original HLSL is restored
and rebuilt, so compiled SPIR-V/binaries are not intentionally left pointing
to the last experimental candidate. An uncatchable process kill, power loss
or concurrent build can still leave an inconsistent tree.

## How acceptance works

The runner benchmarks the original shader, then asks the model to change one
thing at a time. Each proposal is written to the original shader path for the
duration of the build and evaluation. Build and validation failures reject it.
Successful candidates are measured using `tools/perf_harness.py bench`,
one run set per `--scenario` with its normal warm-up behavior. A candidate is
accepted only if its mean log runtime ratio improves on the best accepted
variant by at least `--min-gain` percentage points (relative to the baseline),
and no measured scenario exceeds `--max-regression` percent slowdown
versus baseline. Rejected proposals still feed diagnostics to the next prompt.

Outputs include `baseline.json`, per-iteration candidate source and
`result.json`, `summary.json` and optionally `best.hlsl` for manual review.
The original source file is restored when the runner exits normally or throws
a Python exception. **Do not run concurrently with shader edits/builds**, and
use a clean working tree. A hard process kill or power failure can interrupt
restoration. The runner does not modify git, apply the winner automatically,
or open PRs for model-generated changes.

## Limitations / next steps

- Performance is currently the **whole-frame** benchmark median, not
  per-pass GPU time. A shader improvement can be hidden by unrelated work.
- Benchmark data is not comparable across GPUs, drivers, or changing content.
- The correctness gate is deliberately external, because a universal
  screenshot test is not suitable for all temporal/stochastic effects.
- Only one source file is modified per run; there is no multi-file edit protocol.
- There is no built-in SPIR-V disassembly feedback or GPU profiler integration.
- It is an experimental local optimizer, not an unattended CI auto-merge path.

## Machine and toolchain provenance

The search creates `environment.json` and embeds its contents in
`summary.json`. These records include OS/Windows build, CPU type, GPU name
and installed driver version (Windows CIM and/or NVIDIA-SMI), DXC tool
version/path/SHA-256 and relevant CMake shader build options when discoverable.
Use `--dxc C:/path/to/dxc.exe` to identify an explicitly configured DXC
compiler. Missing tools are represented as null rather than fabricated values.

When comparing optimizations across GPUs or OS/driver/compiler revisions,
treat the environment metadata as part of the experimental identity. The
reported DXC binary is only the detected/selected executable; verify that
it matches your build configuration. Driver metadata is best effort, not an
authoritative Vulkan physical-device identification.

## Hash-addressed loops, branches and register pressure

Every discovered loop receives an ID derived from its containing function,
statement kind and normalized loop header. **Source line and ordinal index are
not part of the hash.** Discover IDs first:

```powershell
python tools/shader_search.py --shader engine/shaders/forwardplus.cs.hlsl --list-regions
```

Give each selected loop its own policy through `--region-knobs knobs.json`:

```json
{
  "regions": {
    "loop:mainCS:HASH_FOR_LOOP_A": "unroll(4)",
    "loop:mainCS:HASH_FOR_LOOP_B": "loop",
    "branch:mainCS:HASH_FOR_BRANCH": "flatten"
  }
}
```

Use the complete IDs from discovery. The example hashes above are placeholders.
`unroll`, `unroll(N)` (N 1–1024), and `loop` target `for` loops;
`branch` and `flatten` target `if` statements. The CLI no longer
supports positional `--loop-index` or `--branch-index` options.
`--register-pressure balanced|minimize` remains an optional AI optimization
priority, **not** a hardware register-allocation guarantee.

Inserting a different loop before a known loop does not change its hash.
For edits to the loop itself, the runner also tracks identities through
unchanged-line diff correspondence and unique content matches, retaining the
baseline ID when matching is unambiguous. Duplicate identical loops are
marked ambiguous rather than differentiated by a fragile ordinal. If the
loop header changes so much that correspondence cannot be determined safely,
that knob fails closed until the ID is refreshed.

VGPRs are hardware-specific. Actual register allocation is determined by the
GPU driver's compilation of the SPIR-V, not the source-level DXC hint alone.
**Do not infer VGPR consumption from SPIR-V instruction counts.** To enforce
register pressure, provide a command that extracts real per-thread register
counts (e.g. using a device-specific compiler/profiler report) as JSON:

```json
{"vgpr": 32, "spill_bytes": 0}
```

`--register-report-cmd "python tools/local_register_report.py"` invokes your
adapter after each build, with `SHADER_SEARCH_REGISTER_REPORT` set to the
required output JSON destination. `registers_per_thread` can be used instead
of `vgpr` for NVIDIA-style register reporting. A minimum positive, finite
count is required; `spill_bytes` defaults to zero. The runner rejects a
candidate if its register count exceeds the baseline by more than
`--max-register-regression` percent (default zero), or measured spill bytes
increase. Missing/malformed reports fail the experiment, never silently pass.
This is optional; without it, the harness makes **no VGPR safety claim**.

Optick remains Tourmaline's CPU/game trace format. Nsight Graphics remains
the dedicated graphics capture/tracing path; you can extract GPU counters with
an external adapter, but the harness does not itself parse Nsight reports or
Optick `.opt` captures. Use the same GPU, driver, resolution and build profile
for register comparisons.


## Addressable shader regions and Git-style identity mapping

Discover individual loop, branch and function-section locations without
starting a model or GPU benchmark:

```powershell
python tools/shader_search.py --shader engine/shaders/forwardplus.cs.hlsl --list-regions
```

The discovery result contains a content-hash `id`, `kind`, `function`, source `line`,
and (for loops) `syntax` and a short statement header. IDs are anchored to
the initial source and tracked across successive AI-generated candidates with
a `difflib.SequenceMatcher` line-diff correspondence, similar in purpose
to Git's moved/unchanged context matching. Insertion of unrelated lines
does not renumber the identified loops. A uniquely identical header in the
same function can be reattached even if the loop moves. A heavily rewritten
loop header **may lose its identity**; missing/duplicated/ambiguous matches
are rejected instead of controlling another loop.

Use `--region-knobs PATH` to set *several* independent loop/branch hints:

```json
{
  "regions": {
    "loop:mainCS:REPLACE_WITH_DISCOVERED_ID": "unroll(4)",
    "loop:mainCS:REPLACE_WITH_ANOTHER_ID": "loop",
    "branch:mainCS:REPLACE_WITH_DISCOVERED_ID": "flatten"
  }
}
```

Use the exact discovered IDs, not the illustrative placeholders above.
The same JSON file can target multiple loops, and non-targeted regions
remain unchanged. Every trial saves `regions.json` alongside the candidate
for inspection. Function `section:...` IDs are discoverable for future
localized AI proposals, but **not** directly rewriteable with HLSL loop hints.
Source-level detection supports `for`, `while`, `do`, `if` as regions;
attributes currently target `for` and `if` only.

The source scanner is conservative and does not replace a real HLSL AST.
DXC SPIR-V reflection can help cross-reference actual compiled instruction
loops and debug locations when present, but cannot generally recover
original source loop identity, especially after optimization or inlining.
No reflection-backed matching is currently claimed.
