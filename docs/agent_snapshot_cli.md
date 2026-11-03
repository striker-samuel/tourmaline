# Agent manual: record, replay and verify PNG snapshot sequences

This is the **agent entry point** for Tourmaline snapshot sequences.
Start here if your task is to reproduce a game run, inspect another
agent's result, compare world states, or diagnose frame divergence.

The supported interface is **`python tools/toursnapshot.py`**. The native
renderer is invoked only for **record**. Inspection, verification, extraction
and comparison are CPU-only and use Python's standard library by default.

The earlier `tools/snapshot_sequence.py` decoder and
`tools/perf_harness.py snapshot` command remain available for compatibility;
new agent workflows should use `toursnapshot.py` for a single command,
consistent JSON output, safe file writes, and explicit exit statuses.

## Commands at a glance

| Command | Purpose | Needs GPU? |
| --- | --- | --- |
| `record` | Launch a cooked `App` and write `Frame_0001.png` … `Frame_NNNN.png` | Yes |
| `inspect` | Summarize sequence, keyframes, RNG seed, provenance and state scope | No |
| `verify` | Validate frame/tick continuity, CRC, state and chain checksums; optionally decoded pixels | No |
| `extract` | Reconstruct an exact engine-observable world-state JSON at a numbered frame | No |
| `compare` | Compare Agent B's canonical JSON state with a captured frame | No |
| `compare-sequence` | Find first mismatched tick, timestep, input, world state or image between two sequences | No |
| `input-tape` | Convert embedded input events into a standalone `.tinp` file | No |

Every command supports `--json` (place it **after the subcommand**).
One JSON object is written to stdout; errors are JSON too. `record`
redirects App logs to `OUTPUT/app.log`, not stdout, keeping piping safe.

```sh
python tools/toursnapshot.py --help
python tools/toursnapshot.py record --help
python tools/toursnapshot.py compare-sequence --help
```

### Exit codes

| Code | Meaning | Agent action |
| --- | --- | --- |
| **0** | Success; `record --dry-run` also returns 0 with `status=planned` | Continue |
| **1** | Runtime failure, invalid/missing artifact, corrupt PNG, IO failure or failed verification | Report error and inspect App log |
| **2** | Valid inputs but captured/agent state or sequence **differs** | Parse `type`, `frame` and `first_difference`; diagnose |
| **64** | CLI usage error (unknown flag, missing required flag, unsafe override) | Fix arguments and retry |

An ordinary `compare` mismatch **is not** a broken file. This is why it
has a separate exit code from corruption and launch failures.

### Stable JSON result

Success example (values illustrative):

```json
{"schema_version":1,"operation":"verify","status":"ok",
 "frames":123,"first_tick":0,"last_tick":122,
 "sequence_id":"sequence-id-from-PNG",
 "keyframes":[1,61,121],"pixels_checked":false,
 "sequence_dir":"/absolute/path/to/frames"}
```

State comparison mismatch example:

```json
{"schema_version":1,"operation":"compare","status":"different",
 "type":"state","frame":61,"first_difference":"/nodes/31:0/global_f32/12",
 "frames":123,"sequence_id":"sequence-id-from-PNG"}
```

A malformed image or a failed launch produces `status=error`,
`message` and exit 1. Invalid arguments produce `status=error`
and exit 64. Additional non-breaking fields may be present;
consumers should use the named fields rather than string matching.

## Agent A: capture a reference run

Run from the repo root. Supply an already-built **App** binary and one
cooked source (a `.tapp` project or one/more `.pak` files). For a custom
BYOC game host, point `--app` at *that host*, provided it implements
Tourmaline's App snapshot flags. **Do not** point at the dedicated
`Benchmark` binary: its game hooks are deliberately disabled.

```sh
python tools/toursnapshot.py record \
  --app out/build/x64-Release/App \
  --project out/build/x64-Release/content/game.tapp \
  --output out/agent-a/frames \
  --frames 123 --keyframe-interval 60 \
  --seed 42 --code-id game-commit-abc123 \
  --json
```

For a pak, replace `--project PATH` with `--pak PATH` (repeatable).
For a particular cooked scene, use `--scene NAME` or
`--scene-index N` with `--pak`.

For an existing source-controlled scenario:

```sh
python tools/toursnapshot.py record \
  --app out/build/x64-Release/App \
  --scenario sponza-atrium \
  --catalog benchmarks/scenarios.json \
  --output out/agent-a/frames \
  --frames 123 --keyframe-interval 60 --code-id engine-build-abc \
  --env TOURMALINE_AA=off --json
```

The scenario supplies its own `source_args`, `app_args`, renderer
environment pins and default frame count. Its cooked source paths are
relative to the **App working directory** (`--workdir`, defaulting to the
App executable's parent). When source paths are passed with `--project`
or `--pak` instead, the CLI resolves them from your invoking directory,
then passes absolute paths to App.

All inherited `TOURMALINE_*` environment variables are cleared before
recording. The CLI applies scenario pins, then explicit `--env` values.
`--seed` overrides the scenario's RNG pin; otherwise it uses the
scenario's `TOURMALINE_RNG_SEED` or a default of **0**. For a true replay,
use the seed reported by `inspect`. Both frame input and RNG stream state
are stored **inside the PNGs**, not in a required sidecar.

Use `--app-arg=--minimal`, `--app-arg=--no-vfs`, etc. for other ordinary
App switches. Flags that would override snapshot paths, source selection,
replay mode, or benchmark frame accounting are rejected. No shell
interpolation is performed.

Before a GPU run, verify the command without creating any files:

```sh
python tools/toursnapshot.py record \
  --app out/build/x64-Release/App \
  --scenario sponza-atrium \
  --output out/agent-a/frames --frames 123 --dry-run --json
```

The planned JSON includes `argv`, `cwd`, explicit environment pins,
output location and log path. Actual runs refuse to overwrite any existing
`Frame_*.png` sequence or App log. Use a fresh directory for each run.
App output goes to `OUTPUT/app.log`; `record` then **reconstructs all
frames** and checks the expected count and keyframe schedule before
reporting success. Set `--timeout N` for longer runs, and use `--pixels`
to verify decoded image pixels as well (requires Pillow).

## Agent B: inspect, reconstruct, compare

First verify the received sequence. A standard PNG viewer sees only the
lossless image; the private ancillary `tsNp` chunk contains the state
record.

```sh
python tools/toursnapshot.py verify out/agent-a/frames --json
python tools/toursnapshot.py verify out/agent-a/frames --pixels --json
python tools/toursnapshot.py inspect out/agent-a/frames --json
```

`--pixels` decodes every PNG to RGBA8 using Pillow and compares
an FNV-1a 64-bit checksum. Without it, `verify` validates PNG chunk CRC,
per-frame state checksum, tick/chain continuity and delta application, but
**does not decode image pixels**. These checksums detect routine corruption;
they are *not cryptographic signatures* and cannot prove authorship.

Reconstruct a single frame as canonical UTF-8 JSON:

```sh
python tools/toursnapshot.py extract out/agent-a/frames \
  --frame 61 --output out/agent-b/reference-frame-61.json --json
```

The output file is **raw world-state JSON**, not the CLI's status envelope.
Your agent can produce its independently computed state in the same schema
and compare it:

```sh
python tools/toursnapshot.py compare out/agent-a/frames \
  --frame 61 --candidate out/agent-b/my-frame-61.json --json
```

A mismatch returns exit **2** and the first JSON Pointer path.
Float values in the scene representation are **exact IEEE-754 float32
bit patterns** (unsigned integers), not rounded decimal floats.
For example, `/nodes/31:0/global_f32/12` means the fourth column's
first matrix component in node slot 31, generation 0. Node identities
are `slot:generation`; node deletion/reuse is observable.

If Agent B also produces PNG snapshots, compare complete sequences:

```sh
python tools/toursnapshot.py compare-sequence out/agent-a/frames \
  --candidate out/agent-b/frames --pixels --json
```

The comparison stops at the first divergent **frame** and reports one of
`frame_count`, `tick`, `timestep`, `input`, `state` or `pixels`.
The state check covers RNG stream progress as well. The two sequences
need not have the same sequence ID or capture timestamp; independent
runs deliberately receive different IDs. `--pixels` verifies each
sequence's actual decoded pixels before comparing their checksums.

## Reuse captured input in a second run

Export the embedded event stream to Tourmaline's binary input tape:

```sh
python tools/toursnapshot.py input-tape out/agent-a/frames \
  --output out/agent-b/replay.tinp --json
```

Replay through the normal host while recording the new results:

```sh
python tools/toursnapshot.py record \
  --app out/build/x64-Release/App \
  --project out/build/x64-Release/content/game.tapp \
  --input-replay out/agent-b/replay.tinp \
  --seed 42 --frames 123 \
  --output out/agent-b/frames --json
```

A `.tinp` reproduces normalized engine input, not user game logic. The
second App must provide compatible gameplay rules or its own independently
implemented behavior. `record` does not load or execute BYOC source from
the PNGs. Input replay still requires the same tick count, compatible
scene/assets and deterministic application behavior to match.

## Snapshot file contract and provenance

- **File names:** `Frame_0001.png`, `Frame_0002.png`, …, consecutive
  and starting at one. Simulation ticks begin at zero.
- **Keyframes:** frame 1 and then frames `1 + k * interval`. They store
  the whole **engine-observable** state and a provenance block.
- **Deltas:** RFC 6902 `add`/`replace`/`remove` patches, applied to
  the previous reconstructed state. Each frame contains input events,
  timestep, state checksum, decoded-pixel checksum and a hash-chain link.
- **PNG record:** one `tsNp` ancillary chunk containing `TSNP1\0`
  followed by versioned UTF-8 JSON. Do not recompress/strip metadata
  with an image editor; ordinary image optimizers may silently discard
  it. Missing metadata is a hard verification failure.
- **Identities:** runtime scene graph node slots + generations, local
  and global transforms, visible/hidden flags, authored component rows
  and stable resource URI references when available. Empty URI means
  no stable catalog identity was resolved.
- **RNG:** PCG32 master seed and all named persistent RNG stream states.
  Unregistered by-value game RNG streams are not captured.
- **Catalog and code:** available catalog identity fingerprint, source
  file time information, scene source recipe and caller-provided
  `--code-id`. A source-file timestamp is not a content-byte hash;
  absent catalog generation timestamps stay explicitly null.

## Fidelity boundaries and safety

The scope identifier is `engine-scene-graph-v1`. It is **not**
a full process-memory or GPU checkpoint. This version does not capture
running physics solver internals, all dynamic game-defined components,
GPU temporal histories, or raw allocator pages. Authored physics data
is not the same thing as a live physics solver snapshot.

BYOC hosts may register an **opt-in logical-state provider** through
`WorldRenderer::SetSnapshotStateExtension` to append serializable
engine/game/allocator state under `state.extensions`. Never embed
pointer-bearing raw allocator memory or arbitrary source code; logical
states must use stable addresses/IDs or relocation-safe representations.

Reconstructing the state for verification needs **only the PNG files**;
rendering a reconstructed scene still needs the relevant engine, shader
and asset resources, and executing gameplay still needs compatible rules.
Pixel-exact replay across different GPUs/drivers is not promised.
Do not compare raster outputs blindly without checking resolution,
AA/postprocess settings, camera and content identity.

All output files are **non-overwriting**. Do not parse metadata as
trusted executable code. Validation is bounded by the decoder's
frame/PNG/event limits and fails closed on missing or altered chunks.

## Failure-handling checklist

When `record` exits **1**, inspect `OUTPUT/app.log` first. Confirm
the project/paks exist, the App binary is built, Vulkan can create a
device and the output directory is fresh. When `verify` exits **1**,
inspect the error message for the corrupt/missing frame and rerun
the producer instead of suppressing the failure.

When `compare` or `compare-sequence` exits **2**, use
`first_difference` to isolate the earliest state mismatch, compare
`rng.streams` and input events at the preceding tick, then check the
render environment and asset identities if only `pixels` differs.
Avoid treating a comparison mismatch as a broken PNG.

For CPU-only agent CLI regression testing:

```sh
python -m unittest discover -s tools/tests -p 'test_toursnapshot.py' -v
python -m unittest discover -s tests -p 'test_snapshot_sequence.py' -v
```

See [snapshot_sequences.md](snapshot_sequences.md) for the underlying
format and [input_tape.md](input_tape.md) for the normalized input ABI.
