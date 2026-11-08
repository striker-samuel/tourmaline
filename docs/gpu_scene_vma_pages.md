# GPU-scene storage: fixed-entry pages and VMA block bins

## Scope

The persistent GPU-scene database has six row formats. Existing HLSL
consumers index each buffer directly as `rows[slot]` through one buffer
device address. **Do not split a table into independently addressed Vulkan
buffers without first changing every consumer and publication ABI.**

This change uses two layers:

1. **Logical row pages:** every table grows in complete row quanta filling
   at most 64 KiB. All pages reside *inside one contiguous VkBuffer* per
   table per in-flight slot. Entries never straddle pages, and existing
   `row[slot]` addresses remain unchanged. Unused capacity is reserved,
   not individually VMA-allocated.
2. **Physical VMA bins:** the explicit `CreateGpuSceneBuffer` RHI path
   chooses a lazy VMA TLSF pool based on buffer size. VMA suballocates
   complete VkBuffer allocations from a fixed-size device-memory block.
   Pools do not use VMA's linear-allocation algorithm because buffer
   retirement order is not FIFO. Pool allocation failures retry through
   the existing standard VMA allocator.

| Table | GPU row bytes | Rows per logical page | Bytes per page |
|---|---:|---:|---:|
| Mesh | 32 | 2048 | 65,536 |
| LOD | 32 | 2048 | 65,536 |
| Submesh | 32 | 2048 | 65,536 |
| Instance | 128 | 512 | 65,536 |
| Leaf | 80 | 819 | 65,520 |
| LeafMember | 32 | 2048 | 65,536 |

| Allocation request | VMA pool block size |
|---|---:|
| Up to 1 MiB | 4 MiB |
| Over 1 MiB, up to 4 MiB | 16 MiB |
| Over 4 MiB, up to 16 MiB | 64 MiB |
| Over 16 MiB | Default VMA allocator |

*Each block-size class is created lazily. A pool's memory type is selected
from the actual storage-buffer create info; if the type changes, the request
uses ordinary VMA instead of suballocating from an incompatible pool.*

## Lifetime and correctness

- Each flight owns an independent table buffer. Only the acquired, retired
  flight may resize and republish its table.
- A page-boundary grow replaces the entire contiguous table buffer and
  forces the full table snapshot upload. No grow means dirty-row-only uploads
  and no change to shader offsets.
- Retired buffers use ResourceManager Holder's deferred Vulkan destruction.
  VMA pools are destroyed during ResourceManager teardown, after draining
  deferred destroys, before destroying the allocator.
- Allocation failure from a pool cannot silently skip a shader upload: it
  retries normal VMA; the existing RHI failure policy then applies.
- No change to the CPU generational free-slot table, leaf spatial partition,
  GPU culling, indexed indirect buffers, or `TOURMALINE_GPU_SCENE_RASTER`
  opt-in path.

## Validation before merging

- Build Windows/MSVC and Linux/Clang, run GPU-scene headless tests and
  architecture/text-integrity tests.
- On Vulkan hardware, grow every table across multiple page boundaries
  while recycling instances and LOD chains. Run `VALIDATE=1` and require
  zero GPU/CPU mismatches.
- Compare GPU raster and legacy screenshots for static opaque scenes,
  then mixed/skinned scenes (which still use the legacy skinning path).
- Run Vulkan validation and memory-budget captures. Compare VMA block count,
  unused bytes and peak committed memory before/after with equal content.
  Pools reduce placement fragmentation **inside VMA**, but reserved
  partially filled blocks can increase peak committed bytes for tiny scenes.
- Use this allocator PR as the baseline for #276. The mixed GPU/CPU
  ownership cutover remains a separate change.
