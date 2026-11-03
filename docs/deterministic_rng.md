# Deterministic gameplay RNG (proposal and initial implementation)

## Goal

Make gameplay randomness reproducible from a saved master seed, and establish
an engine-owned API for a future record/replay or lockstep simulation. This is
**RNG determinism only**: this change does not make the whole engine deterministic.

## API and ownership

- `tour::RandomService` belongs to each `tour::App` instance (no process-global
  mutable RNG); gameplay uses `app::Context::random()`.
- `RandomService::Stream("loot")` returns a persistent stream. Other named
  streams such as `"combat"` do not advance the loot stream. Seed derivation
  uses FNV-1a 64-bit over the UTF-8 name and a fixed 64-bit avalanche mixer.
- `MakeStream(name)` produces an independent by-value stream from the same
  master seed and name; use distinct stable names/IDs for independent worker
  jobs. Reusing a name intentionally repeats that stream's initial sequence.
- `Gameplay()` is shorthand for `Stream("gameplay")`.
- `RandomStream::NextU32()`, `NextBounded(exclusiveUpper)`, and
  `NextFloat01()` provide stable values without implementation-defined
  standard-library distribution behavior. Bounded draws use rejection
  sampling; changing the bound may consume a different number of draws.
- `Save()` / `Restore()` capture the stream state and odd PCG increment.
  Capture the master seed and every active named stream's state when building
  a future checkpoint format. `Reseed()` clears the service's named streams,
  invalidating any previously retained references to them.

## Launch and example

The packaged game host generates a seed from `std::random_device` when none
is supplied, then prints the **effective** seed and algorithm at startup.
Use `TOURMALINE_RNG_SEED` to reproduce a run (unsigned decimal uint64,
including zero; invalid/overflowing values warn and are ignored). The launch
seeds the service before any game elements attach.

```cpp
void Game::onStart(app::Context& ctx) {
    const uint32_t lootIndex = ctx.random().Stream("loot").NextBounded(100);
    // Save ctx.random().Seed() alongside other future replay metadata.
}
```

For a custom host that bypasses `RunPakApp`, call
`host.Random().Reseed(seed)` before game updates. The core service starts
with seed zero unless explicitly seeded, allowing standalone tests to avoid
ambient entropy.

## Determinism contract

- **Algorithm v1:** PCG32 XSH-RR, 64-bit state, multiplier
  `6364136223846793005`, odd stream increment and the documented PCG
  two-step seeding sequence. All arithmetic is unsigned and defined.
- The same seed + stream name + sequence of calls yields identical outputs
  across supported platforms. The reference vector for
  `RandomStream(42, 54)` is pinned in `tests/test_random.cpp`.
- Calls to a shared stream are ordered and **not** thread-safe. Never let
  concurrent job scheduling decide the draw order; derive a separate stream
  by stable entity/job identifier or use deterministic single-threaded
  allocation of draws.
- This is **not cryptographic** randomness and must not be used for secrets.
- Do not alter the PCG seeding/mixing or name hashing without a version bump
  and migration plan for recorded replays.

## Follow-up for full-game replay (out of scope)

1. Fixed simulation tick and consistent floating-point/physics settings.
2. Timestamped input/event recording, with deterministic event ordering.
3. Stable iteration order for game systems, entities, jobs, and asset loading.
4. Replay manifest: engine/game/content version, effective master seed,
   PRNG algorithm version, tick rate, player inputs, and stream checkpoints.
5. State hashing per tick and replay integration tests to catch divergence.

`RandomService` deliberately does not claim deterministic physics, rendering,
audio, GPU particles, wall-clock usage, or unordered/concurrent update order.
