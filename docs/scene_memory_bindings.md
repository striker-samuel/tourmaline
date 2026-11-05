# Scene memory bindings — ergonomics prototype

This experimental API is additive and **read-only**. It exposes borrowed,
typed/strided views of the current `tour::scene::Scene` storage with explicit
semantic IDs, sizes, formats, alignment, and hierarchy field offsets.

The goal is to test discovery and layout ergonomics before replacing
`std::vector` ownership, adding external allocators, or connecting GPU buffers.

## Try it

```cpp
#include <Engine/Scene/scene_memory_bindings.h>

const tour::scene::Scene& scene = world.scene;
using namespace tour::scene::memory;

auto hierarchy = GetView(scene, Semantic::Hierarchy);
auto locals = GetView(scene, Semantic::LocalTransform);
auto globals = GetView(scene, Semantic::GlobalTransform);

assert(hierarchy.stride == sizeof(tour::scene::Hierarchy));
for (size_t i = 0; i != hierarchy.count; ++i) {
    const auto* row = reinterpret_cast<const std::byte*>(hierarchy.data)
                    + i * hierarchy.stride;
    // Within this C++ build, the fields are described by HierarchyFields.
    // External tools can examine layout metadata rather than duplicating
    // struct packing assumptions.
}
```

To prototype a caller-owned allocation *descriptor* (not binding):

```cpp
alignas(16) glm::mat4 myTransforms[128]{};
ExternalBinding proposed{Semantic::LocalTransform, myTransforms,
                         0, 128, sizeof(glm::mat4), alignof(glm::mat4)};
assert(Validate(proposed));
// There is no BindExternalBuffer yet. The Scene still owns its vectors.
```

## Lifetime and safety

- `GetView` exports a borrowed pointer only. Callers must not free it.
- Query again after scene mutation, vector growth, scene reload, or World
  resolve. No pointer-stability or multithreading guarantees are provided.
- Direct writes bypass `SetLocalTransform`, transform dirty propagation,
  and the GPU publication journal. All exported views are read-only.
- Table rows with `std::string`, `std::vector`, or other C++ members are
  **opaque C++-ABI views**, not portable packed serialization formats.
- `nodeGeneration` can lag the hierarchy's length. Consult
  `IsNodeAlive` and `NodeHandleValid` for validity, not raw generation
  words alone. Hierarchy links are signed 32-bit indices with -1 sentinel.
- `ExternalBinding::Validate` checks the generic shape, **not** semantics,
  platform ABI, producer synchronization, or memory ownership.

## Not in this PR

- Actually adopting/replacing Scene allocations.
- External allocation and growth callbacks or writeback.
- Exposing GPU-scene uploader resources or RHI handles.
- Field schemas for every authored table and GPU data format.
- Cross-language C ABI or automatic SoA-to-AoS transforms.

Those should follow an ergonomics review; the first decision is whether
the caller wants `Semantic + View` queries, a registered stream table,
or immutable per-frame snapshots.
