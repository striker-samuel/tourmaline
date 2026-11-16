# Pure point-array transformation graphs

This is a **functional array-to-array** foundation, not a scene-node graph
and not a second renderer. One authored PointCloud can remain one logical
GPUScene instance while its graph evaluates zero or more points for downstream
consumers such as instanced mesh drawing or particles.

```text
Input PointArray (N points, stable IDs)
        |
    Translate
        |
    SetColor
        |
    SetVelocity
        |
   SetPrototype
        |
  KeepEveryNth
        |
Output PointArray (M points)
        |
Future: GPU instancing / point rendering / simulation
```

Each operation maps or filters a point array **without mutating its input**.
An array contains position, color, velocity, prototype index, and a stable
64-bit point ID. Prototype is an index into a separately owned mesh table,
not a per-point mesh copy or a GPUScene node. The output cardinality can
change; a zero-length result is valid. Nodes carry no accumulated state and
evaluating the same program with the same input yields the same output.

`Engine/Placement/point_array_graph.h` provides:

- `Graph`: node IDs, one upstream input per operator, and a selected
  output. Multiple alternative output paths can share an input.
- `Compile`: validates node identity, references, cycles, operator
  parameters and one reachable Input, then walks dependencies in
  deterministic order, removing unrelated branches.
- `Program`: backend-neutral sequence of array operations.
- `Evaluate`: CPU reference backend, retaining input data and replacing
  output only on success.

The initial operations are Translate, Scale, SetColor, SetVelocity,
SetPrototype and KeepEveryNth. They form a deliberately narrow executable
vertical slice. Branching currently means selecting one graph output;
the compiler does not yet merge multiple arrays or support arbitrary
user-defined attributes. Scale is component-wise around the local origin.
Filtering uses the stable point ID (rather than current array index).

## GPUScene / Placement integration (not yet implemented)

This PR **does not** upload a `.shape` PointCloud to GPUScene, compile
GPU shaders, or wire instanced draws. The follow-on implementation should:

1. Load the existing packable `.shape` primitive as **one logical cloud**,
   retain its local-space source positions and generate stable point IDs.
2. Lower the validated point-array program to a GPU backend, using
   bounds-checked buffer spans, ping-pong/alias analysis, and compacting
   passes for filters. It should never create one CPU scene node per point.
3. Preserve attribute semantics and stable IDs across kernels, and resolve
   the cloud's world transform when rendering/instancing.
4. Feed the resulting arrays into Placement's GPU prototype selection,
   culling and indirect drawing path, then connect the GPUScene residency,
   materials and LOD integration as a separate change.
5. Keep the cloud as one logical instance even if GPU processing splits
   arbitrarily large arrays into chunks. The existing 65,536-per-tile
   Placement limit is not an asset limit in this graph IR.

Future operations should use typed input/output attribute declarations,
immutable dataflow semantics, and backend capability validation before
shader emission. Dynamic velocity here is just an **attribute**: there
is deliberately no time-step integration, mutation, or simulation side effect.

## Validation

`PointArrayGraphTests` checks pure mapping, filtering, dead-branch
elimination, deterministic topological order, unchanged source arrays,
attribute propagation and malformed-graph rejection. It is CPU-only.
