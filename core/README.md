# Core memory boundary

`core/include/Core/Memory` is the canonical, backend-independent header
location for allocation policy, linear allocators, arenas, arena-backed flat
arrays, linear vectors and byte-oriented string views.

The CMake `tourmaline_core` interface target exposes these headers to
`engine` and to engine consumers (including the RHI/smvk code).

## Compatibility

Legacy `Engine/Memory/*.h` entry points are forwarding headers. During the
migration, both include paths refer to **exactly the same types and storage**
rather than distinct copies of allocator state. In particular, the
`smvk::Arena`, `smvk::StringViewT` and `tour::mem` type names are retained
to avoid silently breaking consumer ABI.

## Ownership and lifetimes

- `FrameArena()`: borrow for one frame; never cache views, pointers or
  containers beyond the frame reset.
- `AppArena()`: process-lifetime storage; never use it for per-frame growing
  collections.
- `FlightArena(index)`: GPU flight-scoped; reset only after the corresponding
  flight has retired.
- `StringViewT`: borrowed bytes. Intern/copy text to an owner arena if it
  must outlive its input.
- `ArenaFlatArray<T>`: trivially-copyable records only, not a replacement
  for nontrivial resource owners, futures or dynamic strings.

## Remaining implementation split

The allocator implementations are still compiled from
`engine/src/Memory/*.cpp`. In particular `arena.cpp` depends on
`smvk::limits::MaxFramesInFlight`; moving these translation units to a
standalone static Core library requires extracting the flight-slot policy
first. The interface target is therefore an **API boundary**, not yet an
independently linked memory library.

Hot-path migration must be incremental: replace scratch allocations,
then profile, compile and test frame reset / async / GPU fence behavior
before changing persistent owned containers.
