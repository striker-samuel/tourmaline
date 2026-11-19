# AGENTS.md — Tourmaline contributor guidance

Tourmaline is an actively developed C++20 engine, renderer, editor, and content pipeline. This file is for **how to work in the repository**, not a changelog or a catalog of implemented features. Code, tests, and current build configuration take precedence over prose when they disagree.

## Working approach

1. Inspect the relevant implementation, callers, tests, and documentation before changing a subsystem. Do not infer current behavior from historic notes, old PRs, or the name of a design document.
2. Keep changes scoped to the requested problem. Preserve existing interfaces and content identity unless the task calls for changing them. Do not perform unrelated refactors.
3. Reuse the owning subsystem's existing seam rather than adding a parallel parser, cache, allocator, content resolver, or renderer path.
4. Update adjacent tests and documentation when changing a contract. Explain non-obvious lifetime, synchronization, or compatibility reasoning in code comments.
5. Run the narrowest meaningful checks first, then broader checks when feasible. Report precisely what ran, what failed, and what could not be run. Never claim a GPU or platform validation based on a CPU-only test.
6. Do not silently weaken validation, update golden baselines, or add compatibility shims merely to make a failing change pass. Investigate the mismatch first.

## Finding the right source

| Task | Start here |
| --- | --- |
| Tool discovery, pak inspection, release diffs, pixel comparison | [docs/agent_cli_tools.md](docs/agent_cli_tools.md) |
| Snapshot record/inspect/verify/compare | [docs/agent_snapshot_cli.md](docs/agent_snapshot_cli.md) |
| Native tooling API | [docs/tooling_api.md](docs/tooling_api.md) |
| Runtime, typed assets and shared ownership | [docs/content_as_code.md](docs/content_as_code.md), `app/src/`, `engine/include/Engine/Asset/` |
| Project builds, cooking and package format | `cmake/TourmalineProject.cmake`, `tools/project_pipeline/`, [docs/pak_vnext.md](docs/pak_vnext.md) |
| Editor changes | `editor/src/`, relevant `docs/editor_*.md` |
| Rendergraph, shaders, GPU scene | `engine/include/Engine/Rendergraph/`, `engine/src/Renderer/`, `engine/shaders/`, relevant `docs/gpu_scene_*.md` |
| Audio | [docs/audio_source.md](docs/audio_source.md), [docs/audio_graph.md](docs/audio_graph.md) |
| Benchmark reproducibility | [docs/perf_harness.md](docs/perf_harness.md), [docs/headless_bench.md](docs/headless_bench.md), `benchmarks/` |
| Recent source-vs-documentation audit | [docs/implementation_status.md](docs/implementation_status.md) |

Documentation describes a particular scope and point in time. Verify a claim against source and tests before relying on it, especially when a document uses words such as “landed”, “TODO”, “only”, or “always”.

Prefer headless tools for inspection; do not start a GUI or Vulkan renderer just to list or validate cooked assets. Read the relevant CLI manual before using a tool, and check its `--help` for current flags.

## Building and testing

Use **Ninja** and out-of-source CMake builds. Do not mix generators in an existing build tree.

```sh
cmake -B out/build/x64-Release -G Ninja -DCMAKE_BUILD_TYPE=Release .
cmake --build out/build/x64-Release --target App

cmake -B out/build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON .
cmake --build out/build/tests
ctest --test-dir out/build/tests --output-on-failure
```

Choose a suitable build directory and toolchain for the host platform; the examples are not universal paths. On Windows, initialize the MSVC developer environment (or use `build.bat`). On Linux, consult `install_dependencies.sh` and the repository's current CMake requirements. Initialize submodules after cloning:

```sh
git submodule update --init --recursive
```

Tests are plain executables registered with CTest. For a focused change, build and run the relevant target(s) before a full suite. GPU tests and capture comparisons require suitable hardware/drivers; hosted CI cannot substitute for those runs. Do not change test expectations without explaining the intended behavioral change.

Run the UTF-8 integrity check after bulk/scripted text edits:

```sh
python -m unittest discover -s tests -p "test_text_integrity.py" -v
```

Repository text is UTF-8. Preserve legitimate Unicode; do not globally replace punctuation or non-English characters. When writing via PowerShell, use UTF-8 without BOM. Avoid encoding-unsafe whole-file rewrites.

## Architecture and API boundaries

- **Ownership direction:** `tour::` owns engine policy, assets, scenes, world, rendering policy, audio, and services. `smvk::` is the RHI; `smvk::detail::vulkan` is private implementation. Engine code may call qualified RHI APIs; RHI public headers must not depend upward on engine code.
- New engine-owned APIs belong in `tour::` namespaces. Do not introduce broad `using namespace ::smvk;` directives in public headers or new global engine types. Existing compatibility aliases are transitional, not the preferred spelling.
- `tests/test_architecture.cpp` enforces R1–R9 boundary checks with tracked baselines. If an intentional change requires a baseline adjustment, document and review it explicitly; do not bypass the test.
- Public API collection inputs normally use **pointer + count/size**, not STL container parameters. Keep ownership, lifetimes, and zero-count behavior explicit. Internal storage may use containers where appropriate.
- Use the repository's lifetime-specific allocators and scratch containers for engine hot paths; do not add per-frame or per-item heap churn without justification. Verify a scratch allocation does not outlive its arena/reset boundary.
- Respect GPU layout contracts across C++ and HLSL, including alignment, offsets, static assertions, shader mirrors, and code generation. Treat in-flight GPU resource retirement and synchronization as correctness requirements.
- Resource exhaustion should be reported at the call site using the existing strict-resource policy; do not add operational aborts inside the RHI.

Large renderer and Vulkan implementation translation units are intentionally retained. Work through their existing interfaces and helper seams; do not split them as an incidental cleanup.

## Assets, projects, and runtime

- Preserve persistent project/asset identity and tombstones. Identity must not be reconstructed from mutable names, paths, or incidental ordering.
- Generated project catalog headers are output artifacts: change the generator or authoritative source, **not** the generated header by hand. Do not renumber existing serialized asset-kind values or ABI fields.
- Follow the current SMP8 and project-runtime contracts when changing pak reads, cooking, or deployment. Do not add an alternative content-resolution path. Check [docs/pak_vnext.md](docs/pak_vnext.md) and the corresponding tests before touching the format.
- Content reads should go through the existing registry/session ownership path. Do not assume a VFS overlay, whole-pak preload, or path lookup has the same contract as another read lane without checking.
- No automatic backwards-compatibility migration is required merely because a serialized format changes. Make deliberate compatibility decisions for the task; prefer explicit failures to silently accepting incompatible or corrupt content.
- When authoring runtime/editor changes, preserve unsaved editor state and existing resource ownership/lifetime guarantees. Verify whether a change belongs to a project, scene, shared asset, or spawned instance.
- New `TOURMALINE_*` runtime settings belong in the centralized `RunConfig` parse/log/unknown-setting path, with test coverage. Do not scatter ad hoc environment reads.

## Rendering, shaders, and performance

- Change the owning rendergraph pass or frame-renderer seam. Maintain explicit resource dependencies and barriers; do not silently add another rendering path or duplicate uploads.
- Rebuild affected shader targets when changing HLSL or shared CPU/HLSL structures. Check the applicable generated-layout validation and shader tests.
- Treat GPU captures, Nsight traces, and goldens as **diagnostics**, not interchangeable performance baselines. Compare reproducible scenarios with identical workload, environment, and build configuration.
- For benchmarking, read [docs/perf_harness.md](docs/perf_harness.md) and [benchmarks/README.md](benchmarks/README.md). Use the separate `Benchmark` target for harness runs; use `App` for interactive scene setup. Record configuration and limitations alongside results.
- Do not infer GPU rendering correctness from C++ unit tests, pixel parity on one device, or a successful shader compile alone.

## Change-specific verification

Select checks from the actual change, rather than running a fixed giant checklist:

| Changed area | Relevant checks |
| --- | --- |
| General C++ / architecture | Build affected target; related CTest targets; `architecture_tests` for namespace/RHI public-header changes |
| Project, package, asset catalog | Project pipeline / pak host / content registry tests; applicable cook and deployment checks |
| CLI or Python tooling | Its `tools/tests` or `tests/test_*.py` suite; representative `--help` and dry-run/JSON paths |
| Editor and runtime | Related editor/world tests; manual GPU/editor verification when behavior requires it |
| Shader / renderer / GPU memory | Layout and rendergraph tests; relevant GPU capture/regression on supported hardware |
| Benchmark changes | Harness tests, scenario catalog validation, matching repeatable baseline and comparison |

For repository-wide checks, use `ctest --test-dir <build-dir> --output-on-failure` and the UTF-8 integrity command above. If a target, script, command, or document listed here has moved, use the current CMake/test registration and update this file rather than inventing a replacement.

## Completion notes

At the end of a code task, summarize the behavior changed, the files touched, the tests/builds actually run, and any outstanding risks. Distinguish **verified** behavior from assumptions and work that requires a GPU, different OS, external assets, or further integration. Do not present speculative implementation status as established fact.
