# Replayable PNG snapshot sequences

**Agent-facing CLI and manual:** [agent_snapshot_cli.md](agent_snapshot_cli.md)
defines `python tools/toursnapshot.py record|inspect|verify|extract|compare|compare-sequence|input-tape`,
its JSON envelope, deterministic seed/pin planning, complete CLI examples
and exit-code contract. Prefer it for new agent integrations. The commands
below describe the native App flags and original low-level utilities.


Tourmaline's App host can export **one PNG per simulation tick**, each with a
private PNG ancillary chunk containing a versioned scene-state checkpoint or
incremental JSON Patch delta, RNG stream state, engine input events, and image
integrity metadata. The renderer still produces ordinary PNG images.

This is the foundation for independently verifying another agent's rendering
and engine-visible scene state. It does **not** require or execute the
bring-your-own-game-code implementation.

## Scoped GPU capture (default)

Snapshot recording defaults to **scoped** capture: a reusable device-local
buffer with space for 15 tightly packed RGBA8 frames. Each frame's
`TonemapCapture` output is copied by a render-graph transfer pass
(`vkCmdCopyImageToBuffer`) directly into its assigned GPU-buffer offset.
The render graph provides the color-attachment -> transfer-source layout
transition and GPU dependency; no CPU wait, allocation or image readback
occurs in the per-frame capture path. The matching scene state, RNG and
inputs are retained on the CPU at the original simulation tick.

After 15 frames, or on a shorter final batch, the host waits for the
capture submissions, downloads the used region of the GPU buffer with
**one batched copy/submission to host-visible staging**, and then writes
the original per-frame PNGs with `tsNp` metadata, state deltas and hashes.
The GPU batch buffer is reused for the next 15 frames. Export/PNG encoding
still runs synchronously at the batch boundary.

Use `--snapshot-capture=legacy` in App or `--capture-mode legacy`
in `tools/toursnapshot.py record` for the original synchronous per-frame
image download path. The default is `scoped`.

Capture memory is roughly `15 * width * height * 4` bytes of device-local
buffer storage, plus the host-visible staging allocation used during a
batch drain. A capture resolution change during a sequence fails instead
of producing mixed-size files. The PNG wire format and verification
contract are unchanged.

## Record a sequence

```sh
# GPU-capable host, from the build directory (cooked paths are build-relative)
App --project content/game.tapp \
    --snapshot-sequence exports/walk \
    --snapshot-count 123 --snapshot-keyframe 60 \
    --snapshot-code-id <game-commit-or-digest>

# Or use the source-controlled scenario catalog through the harness.
python tools/perf_harness.py snapshot --scenario sponza-atrium \
    --exe out/build/x64-Release/App \
    --frames 123 --keyframe-interval 60 \
    --output out/snapshots/atrium --code-id <game-revision>
```

Use the platform's App binary name as appropriate. `--snapshot-sequence`
creates the output directory and **refuses to mix or overwrite existing
`Frame_*.png`**. The first frame is `Frame_0001.png` with a full
checkpoint. With a 60-frame interval, `Frame_0061.png` is the next
keyframe; other frames carry incremental RFC 6902 JSON patches against the
previous reconstructed state. Each PNG contains its own frame index and tick,
and all event data for that tick. A private ancillary `tsNp` chunk holds
`TSNP1\0` followed by UTF-8 JSON; standard PNG viewers simply ignore it.

The existing `--input-record` and `--input-replay` modes work independently.
Snapshot recording automatically captures normalized input when no tape is
configured. A replay can produce fresh snapshots for state/image comparison.
Scenario snapshot mode launches **App** instead of the dedicated
benchmark executable so game extensions are not stripped. It does not force
`--disable_control`, use benchmark priming, or skip 10 warmup ticks.

## Verify / reconstruct

```sh
python tools/snapshot_sequence.py inspect out/snapshots/atrium/sequence
python tools/snapshot_sequence.py verify out/snapshots/atrium/sequence --pixels
python tools/snapshot_sequence.py extract out/snapshots/atrium/sequence \
    --frame 61 --output out/agent2_state.json
python tools/snapshot_sequence.py compare out/snapshots/atrium/sequence \
    --frame 61 --candidate out/agent2_state.json --json
python tools/snapshot_sequence.py input-tape out/snapshots/atrium/sequence \
    --output out/replay.tinp
python tools/snapshot_sequence.py compare-sequence out/snapshots/atrium/sequence \
    --candidate out/agent2/sequence --pixels --json
```

`--pixels` requires Pillow and checks SHA-style **FNV-1a 64-bit** hashes
of decoded RGBA8 pixels (these are checksums, **not cryptographic hashes**).
`verify` without `--pixels` is Python-stdlib-only. Both validate PNG
chunk CRCs, record schema, consecutive frame numbers and ticks, the
previous-snapshot chain, and reconstructed world-state hashes. Missing,
truncated, malformed, reordered or mismatched frames fail closed. The
`compare` command reports the first differing JSON pointer; pass another
agent's independently generated canonical-state JSON to compare against the
captured engine state.

**Agent-2 model:** the PNGs provide a standalone, deterministic,
engine-visible scene representation. Agent 2 can reconstruct that state
without running Agent 1's code, compare its own state, and read the images.
The `input-tape` extraction makes replay possible using the same
`--input-replay` host interface. Re-running *gameplay rules* requires their
own implementation or compatible game code; input+RNG cannot reconstruct
untracked game-private state by magic.

## Snapshot scope and limitations

The v1 `engine-scene-graph-v1` snapshot serializes live, generational node
slots and identities, names and UUIDs, local/global transforms as exact
float32 bit patterns, hierarchy, visibility/static/LOD-related flags, typed
resource association indices, engine-authored GI/environment/audio/physics/
trigger/schema associations, camera position/forward and named PCG32 stream
state. Frame metadata includes the effective seed, source launch identity,
scene, available catalog identity fingerprint, optional source-file modified
time, render-scale/AA pins and an optional **code identity** (revision/hash).
Game source text is **never** read, embedded, or executed. Catalog identity
fingerprints are NOT full content-byte fingerprints; future releases should
add deployed per-asset hashes and a stable generation timestamp when
available. A value of `unavailable`/null is honest missing provenance.

This format does not yet export opaque physics solver bodies, gameplay
components unknown to the engine, animation scheduler internals, renderer
temporal-history buffers, GPU particles, or allocator heap bytes. The
`physics_authoring` block is authoring state, **not** a live Jolt solver
snapshot. Full binary-perfect cross-GPU images are not guaranteed; compare
decoded pixels on compatible machines or apply deliberate image tolerances.
Allocator snapshots are optional through
`WorldRenderer::SetSnapshotStateExtension(std::function<tour::snapshot::Json()>)`.
A BYOC host can register a provider returning logically named state
(including its own allocator-aware or relocatable data) as a JSON object; it
then appears under `state.extensions` and participates in every keyframe,
delta and state checksum. No provider is installed by default, and Tourmaline
never guesses what raw pointers mean. Automatic snapshots of raw pool/arena
memory remain future work; never serialize raw pointer-bearing memory as a
mandatory replay requirement.

A game can always export its independently tracked game state via its own
format beside the PNGs, but the engine will not capture or run arbitrary
game code. The PNGs alone preserve the engine-observable state above.

## Test

```sh
python -m unittest discover -s tests -p test_snapshot_sequence.py -v
cmake --build <build-dir> --target SnapshotSequenceTests
ctest --test-dir <build-dir> -R '^snapshot_sequence_tests$' --output-on-failure
```

The codec and reconstruction tests are GPU-free. A full App sequence needs
Vulkan and a cooked scene. Sequence bytes can be large because v1 embeds
JSON, and a first/periodic full keyframe can be significantly larger than
regular deltas. Later revisions may compress payloads and add deduplicated
asset references, but will require explicit format versioning.
