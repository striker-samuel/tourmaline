# Semantic scene memory bindings — integration prototype

PR #206 includes PR #207's arena-backed scene storage. The authoritative
`Scene::hierarchy`, `localTransform`, and `globalTransform` fields are
now `tour::mem::ArenaFlatArray<T>`, not `std::vector<T>`.

## Discover streams

```cpp
#include <Engine/Scene/scene_memory_bindings.h>

using namespace tour::scene::memory;
ForEachView(world.scene, [](View v, Layout layout) {
    // v.semantic / v.data / v.count / v.capacity / v.stride / v.alignment
    // layout.fields / layout.fieldCount / layout.nodeIndexed
});
auto hierarchy = GetView(world.scene, Semantic::Hierarchy);
auto layout = Describe(world.scene, Semantic::Hierarchy);
// hierarchy: parent, firstChild, nextSibling, lastSibling, level
```

View pointers are *borrowed*, and must be re-queried after mutation/growth
or scene reload. Views report `Ownership::External` when the scene's
underlying storage is bound to a caller allocation. They are always read-only
**views**, even when the underlying allocation is writable.

The layout describes **native C++ ABI** records. Hierarchy offsets are
computed with `offsetof`; links are signed 32-bit stable slot indices
(`-1` means no relationship). Transforms are four column-major
float32x4 columns with offsets 0, 16, 32, 48.

Other scene tables (many carrying `std::string` / `std::vector`) are
opaque native ABI references, not safe cross-process serialized layouts.

## Adopt caller-owned contiguous transform/hierarchy arrays

```cpp
alignas(16) glm::mat4 locals[1024]{};
const auto old = GetView(scene, Semantic::LocalTransform);
ExternalBinding input{
    Semantic::LocalTransform, locals, old.count, 1024,
    sizeof(glm::mat4), 16, Access::ReadWrite
};
if (!BindExternal(scene, input)) {
    // Refused: bad count, unsupported stride/semantic, access, alignment...
}
// The current locals are copied into the caller's memory.
// Subsequent engine writes through Scene's localTransform hit locals[].
const auto view = GetView(scene, Semantic::LocalTransform);
assert(view.data == locals);
assert(view.ownership == Ownership::External);

// Before locals[] is destroyed or reused:
DetachExternal(scene, Semantic::LocalTransform);
```

`BindExternal` currently supports **only** `LocalTransform`,
`GlobalTransform`, and `Hierarchy`. They require exact contiguous stride
and a bound count matching the current scene count. You can allocate enough
room for future nodes with `capacity`.

**Capacity is a hard limit while bound:** growing beyond it invokes the
existing fatal-OOM policy; it does not quietly take back ownership.
The caller owns the block and is responsible for thread synchronization,
pointer lifetime, and detaching before freeing. The scene does not free it.

**Mutation publication:** use `SetLocalTransform(scene, handle, matrix)`,
not writes to raw `locals[]`, so hierarchy dirty propagation and GPU
journals remain correct. No general-purpose `NotifyExternalWrite` yet.

No implicit ping-pong or arbitrary interleaved/AoS strides are supported.
A genuinely strided external hierarchy stream would require an indexed
access layer throughout the scene mutation and serialization code.

## Remaining integration work

- External binding for nontrivial scene table rows is not implemented.
- The six GPU-scene database tables and per-flight uploader addresses are
  separate from this CPU scene-memory API.
- Native C++ ABI layouts are not a C ABI or cross-process schema.
- No stream version/dirty-range or synchronized acquire/release protocol yet.
- CPU scene views are not GPU addresses; per-flight GPU resource ownership
  remains with the existing renderer.

Run `scene_memory_bindings_tests` and `arena_flat_array_tests` plus scene,
editor, and renderer CI. Treat the branch as experimental until full builds
and frame lifecycle testing complete.
