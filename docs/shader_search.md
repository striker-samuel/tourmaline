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
  --iterations 5 --repeat 5 --output out/shader-search
```

The suggested shader file and validation script above are *examples*: select
a shader that exists in your checkout and supply your own correctness script.

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
