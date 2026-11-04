# Batch scenario screenshots and state dumps

Capture **all** entries from `benchmarks/scenarios.json` using the existing
state-aware snapshot CLI. Requires Python 3.9+, a built **App** (not
`Benchmark`), a working GPU/Vulkan session, and cooked assets for each
scenario in the App build directory. Bistro scenarios need the separately
cooked Bistro pak.

```sh
# Inspect every command without loading content or creating files:
python tools/export_scenario_screenshots.py --app out/build/x64-Release/App --dry-run

# Run all scenarios (Windows: App.exe):
python tools/export_scenario_screenshots.py --app out/build/x64-Release/App \
  --output out/scenario-snapshots --code-id my-engine-commit

# Select scenarios and choose a specific frame for the JSON state dump:
python tools/export_scenario_screenshots.py --app out/build/x64-Release/App \
  --scenario sponza-atrium --state-frame 48 --pixels
```

Each scenario gets `<output>/<id>/frames/Frame_0001.png`, etc.,
`frames/app.log`, and `<output>/<id>/state.json` (canonical
engine-observable state from the last frame by default). The PNGs themselves
contain per-frame state keyframes/deltas, input events and RNG metadata in
the `tsNp` chunk. The top-level `manifest.json` records successes and
failures. The script continues after individual scenario failures and returns
nonzero if any failed; missing content is reported rather than silently
skipped. A failed run may leave partial artifacts and should be investigated.

Capture uses the **scoped** mode by default (15-frame GPU-resident batches).
Use `--capture-mode legacy` for synchronous readbacks,
`--keyframe-interval N` to adjust full-state checkpoints, `--frames N`
to override saved frame counts, `--seed N` to fix RNG, or
`--workdir PATH` when shaders/content live elsewhere. `--pixels` requires
Pillow and verifies decoded pixels as well as snapshot state metadata.

Outputs intentionally refuse overwrites; choose a fresh output directory
for each run. Inspect and verify captures with the existing CLI:

```sh
python tools/toursnapshot.py inspect out/scenario-snapshots/sponza-atrium/frames --json
python tools/toursnapshot.py verify out/scenario-snapshots/sponza-atrium/frames --pixels --json
```

Snapshot scope is `engine-scene-graph-v1`, **not** complete GPU or process
memory. See [agent snapshot CLI](../docs/agent_snapshot_cli.md) for fidelity
boundaries, replay and state comparisons.
