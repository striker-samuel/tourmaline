# Core memory boundary

`core/include/Core/Memory` is the canonical, backend-independent header
location for allocation policy, linear allocators, arenas, arena-backed flat
arrays, linear vectors and byte-oriented string views.

The CMake `tourmaline_core` static library owns the memory implementations
and exposes the public headers to `engine` and other consumers.

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

## Implementation ownership

`core/src/Memory/*.cpp` implements arenas, linear allocators and global
allocator lifetime. GPU flight arena storage uses Core's
`tour::mem::config::kFlightArenaSlots` rather than depending on the RHI API;
keep it aligned with the backend's `MaxFramesInFlight` setting.
The Core static library can be linked independently of Engine.

Hot-path migration must be incremental: replace scratch allocations,
then profile, compile and test frame reset / async / GPU fence behavior
before changing persistent owned containers.
