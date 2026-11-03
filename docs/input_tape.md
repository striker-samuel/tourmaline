# Deterministic input tape (v1)

Input tapes record Tourmaline's **normalized engine input events**, not OS
keystrokes or screenshots. This is the foundation for future scenario PNG
snapshot sequences, world-state deltas, RNG stream checkpoints, and agent
reconstruction. The API lives in
`engine/include/Engine/App/input_tape.h`, and the App pump wires it between
the SDL3 backend adapter and `Input::onEvent`.

## Usage

Record in App against the chosen cooked source:

```sh
App --project content/game.tapp --input-record exports/walk.tinp
App --project content/game.tapp --input-replay exports/walk.tinp
```

Alternatively use `--pak content/paks/world.pak`. Both flags accept
`--input-record=FILE` or `--input-replay=FILE`, but are mutually exclusive.
A replay terminates automatically after the last recorded simulation tick.
The paths are relative to the process working directory; **create the output
directory first**. Existing output paths (and `.tmp` siblings) are refused
rather than overwritten. A failed recording or truncated replay causes a
non-zero process exit.

Frames include every normalized input event seen before the simulation
update, preserving order, key transitions and repeats, modifier bits, mouse
motion (absolute and relative), wheel, text input, and gamepad hotplug,
buttons and axis samples. Each frame includes the engine timestep in
nanoseconds, independent of rendering/wall timing. Physical gameplay input
is ignored during replay. OS close/resize events still work; a manual
close during replay is treated as incomplete.

This is **not yet deterministic game simulation**: random streams, game
private state, the world-state serializer, GPU temporal history, content
hashes and PNG chunk export are not included. The current App dispatches
replayed normalized events to host `OnInputEvent` and `Input`.
ImGui's SDL-specific event processing is suppressed during replay; UI
interaction needs a separate deterministic semantic UI-command layer.
Gamepad device IDs are preserved within the tape; future multi-host
controller mapping may require canonical slots.

## Format contract

The `.tinp` v1 format is portable and deliberately simple:

- 8-byte ASCII magic `TOURINP1`, little-endian u32 version (1) and
  little-endian u32 number of frames.
- For every frame: little-endian u64 zero-based sequential tick, u64
  timestep in nanoseconds, u32 event count.
- Every event: u8 engine EventType, u32 key, mouse button, gamepad button
  and axis, u32 gamepad ID, u32 modifier flags; five IEEE-754 float32
  values (axis, x, y, xrel, yrel), u8 repeat, u32 UTF-8 byte length,
  followed by the raw bytes.
- No native struct dumps, pointers, SDL types, or host-endian assumptions.

The decoder rejects malformed headers, unknown versions, malformed
enums, non-finite floats, invalid timesteps, nonsequential ticks, excess
event counts, truncated payloads, oversized text, trailing data, and
files above 128 MiB. A rejected decode leaves an existing tape intact.
Quit, window-close and file-drop events are intentionally not recorded.
The format does **not** execute data from the tape.

Run the GPU-free unit test through CMake/CTest:

```sh
cmake --build <build-dir> --target InputTapeTests
ctest --test-dir <build-dir> -R '^input_tape_tests$' --output-on-failure
```

## Follow-up scope

Keyframe and delta world-state encoding, PNG ancillary chunks, RNG
stream checkpoints, scene/catalog/content fingerprints, image hashing,
and input-driven state verification belong in subsequent changes built
on this stable tick-indexed event layer.
