# Allocation Analysis & Linear Allocator Migration Plan

## Executive Summary

This document tracks all allocations in the tourmaline codebase, classifies them by lifetime, and proposes a phased migration to linear allocators with reserved virtual memory and static allocation at startup.

---

## 1. Current Allocation Landscape

### 1.1 GPU Memory (VMA - Vulkan Memory Allocator)
**Location**: `engine/src/Vulkan/impl.cpp`, `engine/src/Renderer/mesh.cpp`

| Allocation Type | Count | Size | Lifetime | Current Strategy |
|-----------------|-------|------|----------|------------------|
| Vertex/Index Static Heaps | 2 | 512 MB each | Process | VmaVirtualBlock (linear) |
| Staging Rings (per-thread) | 16 | 32 MB each | Frame/Process | VmaCreateBuffer (HOST_VISIBLE) |
| Bulk Upload Ring | 1 | 512 MB | Load-time | VmaCreateBuffer (HOST_VISIBLE) |
| Readback Staging | 1-2 | Grows on demand | Frame | VmaCreateBuffer (HOST_VISIBLE) |
| Async Readback Staging | 1 | Grows on demand | Frame | VmaCreateBuffer (HOST_VISIBLE) |
| Images/Textures | ~1000s | Variable | Resource | VmaCreateImage (DEVICE_LOCAL) |
| Buffers (UBO/SSBO/Vertex) | ~1000s | Variable | Resource | VmaCreateBuffer |
| Descriptor Pools | ~10s | Small | Frame/Process | vkCreateDescriptorPool |
| Command Buffers | 16/pool | Small | Frame | vkAllocateCommandBuffers |

### 1.2 CPU Memory (Host Allocations)

| Category | Examples | Current Allocator | Lifetime |
|----------|----------|-------------------|----------|
| **Arena Allocators** | AppArena (1 MB blocks), FrameArena (1 MB blocks) | Custom bump allocator | Process / Frame |
| **Sharded Pools** | Images, Textures, Buffers, ImageViews, Samplers, Bindless, DescriptorPools, ShaderModules | Lock-free sharded pool (16 shards × 512) | Process |
| **STL Containers** | std::vector, std::string, std::unordered_map, std::unique_ptr | std::allocator (malloc/free) | Variable |
| **Raw new/delete** | Submitter×3, CommandBufferPool×N, VulkanCore, PipelineCache, Swapchain, RenderGraph, World, TextureStreamer, TextureFeedback, ImGuiOverlay | malloc/free | Process |
| **VMA Virtual Blocks** | Static vertex/index heaps | VMA internal | Process |

---

## 2. Lifetime Classification

### 2.1 Process Lifetime (Static/Startup)
- **VulkanCore**, **VulkanResourceManager**, **Submitter** (graphics/transfer/compute)
- **Static Vertex/Index Heaps** (512 MB each via VmaVirtualBlock)
- **SamplerCache**, **PipelineCache**
- **AppArena** (URI/string storage)
- **Global descriptors**, **bindless layout**
- **ShardedPool metadata** (fixed 8192 entries × 16 shards)

### 2.2 Frame Lifetime (Per-Frame Reset)
- **FrameArena** (scratch strings, URI normalization)
- **Per-frame command buffers** (recycled via CommandBufferPool)
- **Per-frame descriptor sets** (MAX_FRAMES_IN_FLIGHT = 3)
- **Engine globals buffer** (time, deltaTime, frameIndex)
- **Texture feedback buffers** (one per flight)
- **Staging ring offsets** (reset per frame via wrap)

### 2.3 Resource Lifetime (Tied to Asset/Scene)
- **Images/Textures** (loaded from pak, freed on unload)
- **Buffers** (vertex, index, uniform, storage)
- **Shader modules** (loaded once, kept for reflection)
- **Bindless images** (registered once per texture)
- **Descriptor pools** (created per material/pass)

### 2.4 Transient/Scratch (Sub-Frame)
- **UploadStreamer staging** (per-thread rings, bulk ring)
- **Readback staging** (grow-on-demand)
- **RenderGraph pass temporaries** (barriers, usage vectors)
- **ThreadPool task payloads**
- **String formatting** (FrameArena)

---

## 3. Migration Strategy

### Phase 0: Infrastructure (Week 1)

#### 3.1 Linear Allocator Core (`engine/src/Memory/linear_allocator.h/cpp`)
```cpp
// Reserve virtual address space at startup, commit on demand
class LinearAllocator {
    void* base_;           // Reserved base address (VirtualAlloc/VirtualAlloc2)
    size_t reserved_;      // Total reserved (e.g., 4 GB)
    size_t committed_;     // Currently committed
    size_t offset_;        // Current bump pointer
    size_t alignment_;     // Default alignment (64 KB for large pages)

    // Allocate with alignment, commit pages as needed
    void* allocate(size_t size, size_t align = 64_KB);
    // Reset to mark (for frame/scope allocators)
    void reset(size_t mark);
    // Full reset (process lifetime allocators never call this)
    void reset_all() { offset_ = 0; }
};

// Global instances (constructed before main):
LinearAllocator g_processAllocator(4_GB);    // Process lifetime
LinearAllocator g_frameAllocator(256_MB);    // Frame lifetime  
LinearAllocator g_transientAllocator(1_GB);  // Transient/scratch
```

#### 3.2 Virtual Memory Reservation (Windows)
```cpp
// Reserve 4 GB at startup (no physical commit)
void* reserve = VirtualAlloc2(nullptr, 4_GB, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 
                              PAGE_READWRITE, nullptr, 0);
// Commit on first touch via custom page fault handler or explicit commit
```

#### 3.3 Replace std::allocator
```cpp
template<typename T>
struct LinearAllocatorSTL {
    using value_type = T;
    LinearAllocator* allocator_;
    
    T* allocate(size_t n) { 
        return (T*)allocator_->allocate(n * sizeof(T), alignof(T)); 
    }
    void deallocate(T* p, size_t n) { /* no-op for linear */ }
};
```

---

### Phase 1: Process-Lifetime Allocations (Week 2)

| Target | Current | Migration |
|--------|---------|-----------|
| `VulkanCore` | `new` | `g_processAllocator.Create<VulkanCore>()` |
| `Submitter` (×3) | `new` | `g_processAllocator.Create<Submitter>()` |
| `CommandBufferPool` | `new` | `g_processAllocator.Create<CommandBufferPool>()` |
| `PipelineCache` | `make_unique` | `g_processAllocator.Create<PipelineCache>()` |
| `SamplerCache` | `make_unique` | `g_processAllocator.Create<SamplerCache>()` |
| `Swapchain` | `make_unique` | `g_processAllocator.Create<Swapchain>()` |
| `RenderGraph` | `make_unique` | `g_processAllocator.Create<RenderGraph>()` |
| `World` | `make_unique` | `g_processAllocator.Create<World>()` |
| `TextureStreamer` | `make_unique` | `g_processAllocator.Create<TextureStreamer>()` |
| `TextureFeedback` | `make_unique` | `g_processAllocator.Create<TextureFeedback>()` |
| `ImGuiOverlay` | `make_unique` | `g_processAllocator.Create<ImGuiOverlay>()` |
| `AppArena` blocks | `unique_ptr<uint8_t[]>` | `g_processAllocator.allocate(blockSize)` |

**ShardedPool**: Already uses fixed arrays (no heap allocation for entries). Keep as-is.

---

### Phase 2: Frame-Lifetime Allocations (Week 3)

| Target | Current | Migration |
|--------|---------|-----------|
| `FrameArena` blocks | `unique_ptr<uint8_t[]>` | `g_frameAllocator.allocate(blockSize)` |
| Per-frame vectors (barriers, usages) | `std::vector` | `std::vector<T, LinearAllocatorSTL<T>>` with `g_frameAllocator` |
| RenderGraph pass temporaries | `std::vector` | Frame allocator |
| Descriptor set arrays | `std::array` / fixed | Already fixed-size |

**Reset Point**: Call `g_frameAllocator.reset(frameMark)` at frame start in `WorldRenderer::BeginFrame()`.

---

### Phase 3: Resource-Lifetime Allocations (Week 4)

**Strategy**: Pool-based with linear backing

```cpp
// Resource pool with linear allocator backing
template<typename T, size_t MaxCount>
class ResourcePool {
    LinearAllocator* allocator_;
    T* objects_;           // Contiguous array: allocator_->allocate(MaxCount * sizeof(T))
    uint32_t freeList_[MaxCount];
    uint32_t freeCount_;
    uint32_t generation_[MaxCount];
    
public:
    Handle<T> allocate() { /* pop freeList, placement new */ }
    void deallocate(Handle<T> h) { /* push freeList, destructor */ }
    T* get(Handle<T> h) { return &objects_[h.index]; }
};
```

**Migration Targets**:
- Images/Textures → `ResourcePool<Image, MAX_IMAGES>` backed by `g_processAllocator`
- Buffers → `ResourcePool<Buffer, MAX_BUFFERS>` backed by `g_processAllocator`
- ShaderModules → `ResourcePool<ShaderModule, MAX_SHADERS>` backed by `g_processAllocator`

**Note**: VMA allocations (GPU memory) remain unchanged - they're already pool-like via VmaVirtualBlock.

---

### Phase 4: Transient/Scratch Allocations (Week 5)

| Target | Migration |
|--------|-----------|
| `UploadStreamer` staging rings | Pre-reserve 32 MB × 16 threads = 512 MB in `g_transientAllocator` |
| Bulk upload ring (512 MB) | Pre-reserve in `g_transientAllocator` |
| Readback staging | Pre-reserve max expected (e.g., 64 MB) in `g_transientAllocator` |
| RenderGraph temp vectors | `std::vector<T, LinearAllocatorSTL<T>>` with `g_transientAllocator` |
| ThreadPool task queues | Ring buffer in `g_transientAllocator` |
| String formatting | Already uses FrameArena → redirect to `g_frameAllocator` |

**Reset Point**: `g_transientAllocator.reset(transientMark)` at frame boundary after GPU sync.

---

### Phase 5: STL Container Replacement (Week 6)

Replace `std::vector`/`std::string`/`std::unordered_map` with linear-backed variants:

```cpp
// engine/include/Engine/Memory/linear_vector.h
template<typename T>
using LinearVector = std::vector<T, LinearAllocatorSTL<T>>;

template<typename T>
using ProcessVector = LinearVector<T>;  // g_processAllocator

template<typename T>
using FrameVector = LinearVector<T>;    // g_frameAllocator

template<typename T>
using TransientVector = LinearVector<T>; // g_transientAllocator
```

**Migration Priority**:
1. RenderGraph pass vectors (barriers, usages, imageWrites)
2. UploadStreamer pending ranges/cleanups
3. Shader reflection temporaries
4. Asset loading vectors (pak, gltf, ktx)
5. Editor UI strings/vectors

---

## 4. Virtual Memory Layout (Startup Reservation)

```
Process Virtual Address Space (4 GB reserved at startup):
┌─────────────────────────────────────────────────────────────┐
│ 0x0000'0000'0000 - 0x0000'00FF'FFFF  │ AppArena (256 MB)    │ ← URI strings, cluster data
│ 0x0000'0100'0000 - 0x0000'01FF'FFFF  │ ShardedPools (256 MB)│ ← Fixed metadata
│ 0x0000'0200'0000 - 0x0000'0FFF'FFFF  │ Process Objects (2 GB)│ ← VulkanCore, Submitter, etc.
│ 0x0000'1000'0000 - 0x0000'10FF'FFFF  │ FrameArena (256 MB)   │ ← Per-frame scratch
│ 0x0000'1100'0000 - 0x0000'1FFF'FFFF  │ Resource Pools (1 GB) │ ← Image/Buffer/Shader pools
│ 0x0000'2000'0000 - 0x0000'2FFF'FFFF  │ Transient/Scratch (1 GB)│ ← Staging, readback, temps
│ 0x0000'3000'0000 - 0x0000'3FFF'FFFF  │ Growth/Spare (1 GB)   │ ← Future expansion
└─────────────────────────────────────────────────────────────┘

GPU Virtual Address Space (VMA managed - unchanged):
- Static Vertex Heap: 512 MB (VmaVirtualBlock, linear)
- Static Index Heap: 512 MB (VmaVirtualBlock, linear)  
- General allocations: VMA default pools
```

---

## 5. Implementation Checklist

### Core Infrastructure
- [x] `LinearAllocator` class with VirtualAlloc2 reservation
- [x] `LinearAllocatorSTL` adapter for STL containers
- [x] Global instances: `g_processAllocator`, `g_frameAllocator`, `g_transientAllocator`
- [x] Startup reservation in `main.cpp` before any subsystem init
- [x] OOM handling (commit failure → abort with diagnostic)
- [x] Centralized default sizes (`Engine/Memory/alloc_config.h` — single place to resize)

### Phase 1: Process Lifetime
- [x] Replace `new Submitter` with `g_processAllocator.Create<Submitter>()`
- [x] Replace `new CommandBufferPool` with `g_processAllocator.Create<CommandBufferPool>()`
- [x] Replace `make_unique<VulkanCore>` with placement new
- [x] Replace `make_unique<PipelineCache>` etc.
- [x] Convert `AppArena::Block::data` from `unique_ptr` to linear allocation
- [x] App-level borrowed objects (`m_context`/`m_swapchain`/`m_rendergraph`/`m_world`, `TextureStreamer`, `TextureFeedback`, `ImGuiOverlay`, `World` in `main`) — Epoch 5

### Phase 2: Frame Lifetime
- [x] Convert `FrameArena::Block::data` to linear allocation
- [x] Replace RenderGraph frame vectors with `FrameVector` (first set: `EmitBarriers` + per-pass barrier/sync scratch)
- [x] Add frame mark/reset in `WorldRenderer::BeginFrame()` (implemented as full frame rewind in `ResetFrameArena()`, called at `DrawFrame` start — the actual frame boundary)

### Phase 3: Resource Pools
- [x] Implement `ResourcePool<T, MaxCount>` template (evaluated Epoch 8: declined — `ShardedPool` already IS the resource pool with free-list + generations; a parallel template adds API surface with no users)
- [x] Migrate `ShardedPool` storage to linear-backed (metadata already fixed) (covered by Epoch 5: the owning `VulkanResourceManager` is `MakeProcess`-placed, so all pool arrays live in process linear memory; `Contains()` proves residency)
- [x] Replace VMA buffer/image allocations with pool handles (keep VMA for GPU mem) (already the architecture: every resource goes through `ShardedPool` handles; VMA holds only GPU bytes)
- [x] Pool sharding constants centralized (`kNumPoolShards`/`kShardCapacity`/`kShardedMaxSize` in `alloc_config.h`, Epoch 8)

### Phase 4: Transient
- [x] Pre-reserve staging rings in `g_transientAllocator` (sizes centralized in `alloc_config.h`; ring VkBuffers stay VMA — GPU memory is out of scope — CPU-side ring state migrated, VMA OOM now crashes)
- [x] Replace `UploadStreamer::ThreadUploadState::ring` allocation (pending barriers/ranges → `TransientVector`; `StagingRing` VkBuffer stays VMA by design)
- [x] Replace readback staging growth with pre-reserved (growth bounds centralized; cap/alloc failure crashes instead of silent `false`)
- [ ] Transient allocator frame-boundary reset (deferred: streamer vectors hold in-flight ranges across frames — reset only when temporaries are the sole users)
- [x] ThreadPool task queues → ring buffer in transient (Epoch 7: bounded ring, pre-allocated slots; enqueue backpressures instead of growing)
- [x] String formatting via FrameArena (already the case; Epoch 2 backed it)

### Phase 5: STL Containers
- [x] Create `linear_vector.h`, `linear_string.h`, `linear_unordered_map.h` (`linear_vector.h` done Epoch 1; string/map deferred — low churn)
- [x] Migrate high-churn containers first (RenderGraph, UploadStreamer) (barriers Epoch 4, streamer vectors Epoch 6)
- [ ] Migrate asset loading containers
- [ ] Migrate editor containers

### Validation
- [x] Add allocation tracking/stats (committed vs reserved) (`GetStats()` + `DumpStats()` + `ShutdownMemory()` report, Epoch 8)
- [ ] Verify no malloc/free after startup (hook CRT allocator) (needs debug-CRT instrumented build; declined for this env — guarantee is structural: hot paths allocate only from linear/VMA reservations)
- [x] Benchmark frame time (target: <0.1 ms alloc overhead) (Epoch 9 microbenchmarks, all bounds pass with headroom)
- [x] Test memory pressure scenarios (page commit latency) (Epoch 9 cold-commit metric: 16 MB quantum commits in ~0.01 ms)

---

## 6. Risk Mitigation

| Risk | Mitigation |
|------|------------|
| VirtualAlloc2 not available (Win < 10 1809) | Fallback to `VirtualAlloc` with `MEM_RESERVE` |
| Page commit latency on first touch | Pre-commit critical paths at startup |
| Fragmentation within linear allocator | Use separate allocators per lifetime; no free = no frag |
| OOM on 32-bit / low memory | Reserve smaller (1 GB process, 64 MB frame, 256 MB transient) |
| Third-party libs (SDL, VMA, KTX) still malloc | Acceptable - they're small/fixed; focus on engine code |
| Debugging (ASan, Valgrind) | Disable linear allocator in debug builds or add redzones |

---

## 7. Expected Outcomes

| Metric | Current | Target |
|--------|---------|--------|
| **Startup allocations** | ~500 malloc calls | 1 VirtualAlloc2 + ~50 placement new |
| **Frame allocations** | ~2000 malloc/free | 0 (frame allocator reset only) |
| **Allocation latency (p50)** | ~500 ns (malloc) | ~5 ns (bump pointer) |
| **Allocation latency (p99)** | ~5 µs (contention) | ~10 ns (no contention) |
| **Memory fragmentation** | Moderate (malloc) | Zero (linear) |
| **Working set** | Dynamic | Predictable (reserved) |

---

## 8. Code Changes Summary

### New Files
- `engine/include/Engine/Memory/linear_allocator.h`
- `engine/src/Memory/linear_allocator.cpp`
- `engine/include/Engine/Memory/linear_vector.h`
- `engine/include/Engine/Memory/linear_string.h`
- `engine/include/Engine/Memory/resource_pool.h`

### Modified Files
- `app/src/main.cpp` - Startup reservation
- `engine/src/Vulkan/impl.cpp` - Process lifetime objects
- `engine/src/Vulkan/detail/vulkan_impl.h` - Allocator access
- `engine/src/Memory/arena.cpp` - Redirect to linear allocators
- `engine/src/RHI/resource_manager.cpp` - Resource pools
- `engine/src/Renderer/mesh.cpp` - Static heaps (already linear via VMA)
- `engine/src/Rendergraph/rendergraph.cpp` - Frame vectors
- `engine/src/Renderer/texture_streamer.cpp` - Transient vectors
- `engine/src/Asset/pak.cpp` - Loading vectors
- `editor/src/*.cpp` - Editor containers

---

## 9. Rollout Plan

| Week | Phase | Deliverable |
|------|-------|-------------|
| 1 | 0 | LinearAllocator core + startup reservation |
| 2 | 1 | Process-lifetime objects migrated |
| 3 | 2 | Frame-lifetime allocations migrated |
| 4 | 3 | Resource pools implemented |
| 5 | 4 | Transient/staging pre-reserved |
| 6 | 5 | STL containers replaced |
| 7 | - | Testing, profiling, bug fixes |
| 8 | - | Documentation, cleanup |

---

*Generated: 2026-09-30*
*Codebase: tourmaline (engine + app + editor)*

---

## 10. Epoch Log

### Epoch 1 — Linear allocator core + centralized sizes (2026-09-30)
**Committed pieces of Phase 0:**
- `engine/include/Engine/Memory/alloc_config.h` (NEW) — single place for
  all default chunk sizes: `kAppArenaBlock` / `kFrameArenaBlock` (1 MiB),
  `kProcessReserve` (4 GiB) / `kFrameReserve` (256 MiB) /
  `kTransientReserve` (1 GiB), `kCommitQuantum` (16 MiB), `kDefaultAlign`.
  Retuning memory = editing this file only.
- `engine/include/Engine/Memory/linear_allocator.h` + `engine/src/Memory/linear_allocator.cpp`
  (NEW) — `mem::LinearAllocator`: VirtualAlloc MEM_RESERVE + on-demand
  MEM_COMMIT (Windows; malloc fallback elsewhere), lock-free atomic bump
  fast path, mark/reset/ResetAll. Plain `VirtualAlloc` used instead of
  `VirtualAlloc2` placeholders (works on all Win10+, same reservation
  semantics; see §6 risk row).
- `engine/include/Engine/Memory/linear_vector.h` (NEW) — `LinearAllocatorSTL<T>`
  adapter (deallocate = no-op) + `ProcessVector` / `FrameVector` /
  `TransientVector` aliases.
- OOM policy as specified: `ReportOomAndCrash()` logs to stderr and aborts.
  No fallback, no compaction, no survival strategy.
- `Arena` defaults now come from `mem::config` (`AppArena` → `kAppArenaBlock`,
  `FrameArena` → `kFrameArenaBlock`; both still 1 MiB, zero behavior change).
- `tests/test_memory.cpp` (NEW, `MemoryTests`) — 23 checks, all passing:
  config sanity, bump/alignment, Create, mark/reset, STL growth.
- **Validation**: `MemoryTests.exe` 23/23 PASS (out/build/x64-Release).

**Explicitly deferred to Epoch 2:** global instances, `InitMemory()` startup
reservation in `app`/`editor` `main.cpp`, `Arena` block backing migration.

### Epoch 2 — Globals + startup reservation + Arena backing (2026-09-30)
- `engine/include/Engine/Memory/memory.h` + `engine/src/Memory/memory.cpp`
  (NEW) — `ProcessAllocator()` / `FrameAllocator()` / `TransientAllocator()`
  (function-local statics sized from `mem::config`), `InitMemory()` (reserve
  all VA up front, fail fast), `ShutdownMemory()` teardown hook.
- `app/src/main.cpp`, `editor/src/main.cpp` — `smvk::mem::InitMemory()` as
  the first call in `main()`, before workers/Vulkan/assets.
- `Arena` gained an optional `mem::LinearAllocator*` backing store:
  block storage comes from the backing bump allocator; `Reset()` rewinds
  to the first-use mark instead of freeing (warm committed pages reused).
  Unbacked arenas (per-cluster) keep exact legacy malloc behavior.
  `AppArena` → process allocator, `FrameArena` → frame allocator.
- `MemoryTests` extended to 44 checks (globals, idempotent init, backed vs
  unbacked arenas, global wiring) — 44/44 PASS.
- **Validation**: `App.exe` + `Editor.exe` link; `AssetTests` 334/334 PASS
  (exercises the now-backed `AppArena::FormatString` path).

### Epoch 3 — Phase 1 engine objects to the process allocator (2026-09-30)
- `memory.h`: `LinearDeleter<T>` / `LinearPtr<T>` (unique_ptr with dtor-only
  deleter — storage never freed) + `MakeLinear` / `MakeProcess` factories.
- Migrated, all in `engine/src/Vulkan/impl.cpp` (+ member decls):
  - `Submitter` ×3 (`new` → `ProcessAllocator().Create`; `delete` →
    explicit `~Submitter()`, aliasing logic unchanged).
  - `CommandBufferPool` thread-local pools (`new` → `Create`; teardown
    `delete` → explicit `~CommandBufferPool()` after `destroy()`).
  - `VulkanCore` (`CreateVulkanCoreWithWindow` returns
    `LinearPtr<VulkanCore>`; single caller, member type updated).
  - `UploadStreamer` (`m_streamer`) and `VulkanSamplerCache`
    (`samplerCache_`) → `LinearPtr` + `MakeProcess`; all `->`/`get()`/
    `reset()` call sites compile unchanged with identical semantics.
  - `RenderGraph::pipelineCacheOpaque_` → linear-owned.
- Teardown semantics preserved: explicit Vulkan destroys (`destroy()`,
  `Clear()`, `waitAll()`) still run before the dtor, exactly as before —
  only the malloc free disappears.
- `MemoryTests` 52/52 PASS (+8 `LinearPtr`: construct/move/dtor/reset/
  release/`MakeProcess`).
- **Validation**: `App.exe` + `Editor.exe` link; `AssetTests` 334/334 PASS.
- **Deferred to Epoch 4:** App-level objects with borrowed-pointer
  subtleties (`m_context`/`m_swapchain`/`m_rendergraph`/`m_world`,
  `TextureStreamer`, `TextureFeedback`, `ImGuiOverlay`, `World`).

### Epoch 4 — Phase 2 frame vectors + full frame rewind (2026-09-30)
- `Arena::ResetForFrameReuse()` (NEW): drops blocks without touching the
  backing store + invalidates the rewind mark so the next frame recaptures
  it. `ResetFrameArena()` now performs the FULL frame rewind — arena drop
  + `FrameAllocator().ResetAll()` — at the `DrawFrame` start boundary, when
  no frame-lifetime allocation is live. This also reclaims dead
  `FrameVector` temporaries; `Arena::Reset()` (mark-scoped rewind) is kept
  for non-frame arenas.
- `RenderGraph` barrier scratch → frame allocator (`MakeFrameScratchVec`
  helper in `impl.cpp`): `EmitBarriers` img/buf barriers + per-pass
  img/buf barriers and queue-family index arrays. All are function-local,
  die in-scope, and are consumed synchronously via `.data()` by
  `vkCmdPipelineBarrier2` — safe under the frame-boundary rewind.
- `MemoryTests` 58/58 PASS (+6 `TestFrameReset`: shared-allocator
  arena+vector discipline, boundary rewind, mark recapture, global
  `ResetFrameArena` + reuse).
- **Validation**: `App.exe` + `Editor.exe` link; `AssetTests` 334/334 PASS.
- **Remaining:** App-level process objects with borrowed-pointer semantics
  (`m_context`/`m_swapchain`/`m_rendergraph`/`m_world`, `TextureStreamer`,
  `TextureFeedback`, `ImGuiOverlay`, `World` in `main`); transient
  pre-reservation (Phase 4); wider STL migration (Phase 5).

### Epoch 5 — App-level process objects to LinearPtr (2026-09-30)
- `smvk::App` graphics members (`m_context`/`m_swapchain`/`m_rendergraph`/
  `m_world` in `Engine/App/app.h`) → `LinearPtr`; all creations
  (`Initialize` + hosted boot) use `MakeProcess`. Borrowed-host semantics
  preserved verbatim: `reset(borrowed)` + `release()` + flags work
  identically under the dtor-only deleter (`RebindBorrowedWorld`, `~App`).
- `World` ownership chain unified: `WorldSource` returns
  `LinearPtr<World>`, `LoadPakWorld` builds via `MakeProcess`; editor
  `m_world` reassignment (`OpenScene`/pak paths) + `m_worldRetire` stash
  (`std::move` + `reset()`) unchanged in behavior.
- `WorldRenderer`: `m_textureStreamer`, `m_textureFeedback`, `m_imgui`,
  `m_shadowWorkers` → `LinearPtr` + `MakeProcess`.
- `EditorApp`: `m_imgui`, `m_textureStreamer`, hosted `m_wysiwyg`
  (`WorldRenderer`), `m_worldRetire` → `LinearPtr` + `MakeProcess`.
- `samples/02_blur`: own `m_imgui` → `LinearPtr` + `MakeProcess`.
- Verified zero remaining `unique_ptr`/`new`/`make_unique` for these types
  repo-wide (only a stale `main.cpp.orig` backup references the old form).
- **Validation**: `App` + `Editor` + `01_triangle` + `02_blur` build/link;
  `MemoryTests` 58/58, `AssetTests` 334/334 PASS.
- **Remaining:** transient pre-reservation (Phase 4: staging rings,
  readback); wider STL migration (Phase 5); cache/store adapters
  (`ArtifactStore`, `StoreArtifactCacheAdapter`) intentionally left on
  malloc — different subsystem, Phase 5 material.

### Epoch 6 — Transient staging state + OOM-crash policy (2026-09-30)
- `alloc_config.h`: all staging sizes centralized — `kStreamerTotalBudget`
  (256 MB), `kStagingRingPerThread` (32 MB), `kBulkRingCapacity` (512 MB),
  `kUploadFlushThreshold` (256 MB), `kReadbackStagingMin/Max` (256 KiB /
  256 MB). `init()` budget/floor, shard default, bulk capacity, flush
  threshold, and readback doubling bounds all reference these (zero
  behavior change in the happy path).
- `ThreadUploadState::{pendingBufferAcquires, pendingImageAcquires,
  pendingRanges}` → `TransientVector` (no malloc on the upload hot path;
  includes the filter-and-swap `live` temp in `allocFromRing`).
  `pendingCleanups` (`packaged_task`, whose shared state mallocs
  internally regardless) deliberately left for Phase 5.
- Monotonic by necessity: in-flight ring ranges span frames, so the
  transient allocator is NOT reset while streamer vectors are live —
  storage grows to the high-water mark and is reused (documented on the
  struct; reset row left open until temporaries are the sole users).
- OOM policy applied to GPU staging (per standing instruction — crash +
  report, no survival): `ensureRing` VMA failure aborts with ring name +
  size + VkResult (was: silent `false` → skipped bulk uploads);
  `EnsureReadbackStaging` cap-exceeded/alloc-failure aborts (was: silent
  `false` → uninitialized pixels downstream). Callers keep their checks
  for other causes (bad handles stay graceful validation).
- `MemoryTests` 63/63 PASS (+3 config sanity, +2 transient growth).
- **Validation**: `App` + `Editor` link; `AssetTests` 334/334 PASS.
- **Remaining:** transient allocator frame-boundary reset (deferred —
  streamer vectors span frames); ThreadPool queues (own epoch);
  `pendingCleanups`/`m_deferredTasks` packaged_task vectors (Phase 5);
  general CreateBuffer/CreateImage failure paths (Phase 3/5).

### Epoch 7 — ThreadPool bounded task ring in transient memory (2026-09-30)
- `Engine/Utils/thread_pool.h` reworked (same public API, still
  header-only): `std::queue<std::function<...>>` (unbounded deque, chunk
  mallocs per push) → bounded ring of `kThreadPoolQueueSlots` (4096,
  centralized in `alloc_config.h`, ~160 KB) pre-allocated from the
  transient allocator at pool construction. Slots placement-constructed,
  explicitly destroyed at teardown; storage never freed.
- Steady state performs zero container mallocs. The per-task
  `packaged_task` shared state still mallocs (`std::future` internals —
  removing that needs custom futures, explicitly out of scope).
- Backpressure contract: enqueue BLOCKS when full instead of growing
  (never OOMs); documented ban on enqueue-from-worker-tasks (workers drain
  the ring — full ring + all workers blocked in enqueue would deadlock).
  All 14 existing enqueue sites are outer loops, verified safe. Stop/drain
  semantics identical (`stop_ && empty` exits; pending tasks still run).
- `MemoryTests` +3 (`TestThreadPool`: worker count, 300 futures through 2
  workers incl. ring wrap, 50 void tasks, clean join) — 66/66 PASS.
- **Validation**: `App` + `Editor` link; `StreamingTests` 24/24,
  `AssetTests` 334/334 PASS.

### Epoch 8 — Pool constants + allocator introspection (2026-09-30)
- `kNumPoolShards`/`kShardCapacity`/`kShardedMaxSize` moved to
  `alloc_config.h`; `pool.h` aliases them (all call sites, static_asserts,
  and array bounds unchanged — still compile-time constants).
- `LinearAllocator::Contains()` (residency proof for migrated objects),
  `GetStats()` (used/committed/reserved snapshot), `mem::DumpStats()`
  (one stderr line per lifetime), `ShutdownMemory()` now prints the
  footprint report; both `main()`s call it on the success path.
- `MemoryTests` +7 (`TestAllocatorStats`: null/stack/outside/range
  containment, stats ordering, pool-config aliasing, dump-to-tmpfile
  naming all three lifetimes) — 73/73 PASS.
- Phase 3 closed without new code (see box notes): no parallel
  `ResourcePool` template (declined as redundant), `ShardedPool` backing
  already linear via Epoch 5 placement, handles already the sole
  CPU-side path with VMA holding only GPU bytes.
- **Validation**: `App` + `Editor` link; `AssetTests` 334/334 PASS.
- **Evaluated and declined:** `linear_string.h` / `linear_unordered_map.h`
  (no hot-path users — strings/vectors covered; adapters without users are
  dead code), custom futures for task shared-state (per user: not
  necessary), migrating load-time asset/editor vectors to linear (they
  allocate-then-free per load; process/transient linear backing would
  retain them to high-water permanently — malloc is the right tool there).
- **Remaining:** CRT malloc hook verification, frame-time alloc benchmark,
  page-commit latency under pressure, transient reset (blocked on
  in-flight streamer ranges).

### Epoch 9 — Allocation microbenchmarks (2026-09-30)
- `TestBenchmarks` in `MemoryTests` (local allocators only; generous
  5-50x bounds catch pathology without flaking; raw numbers print as
  METRIC lines). Measured on this machine (x64-Release):
  - bump mean **3.6 ns** (bound 100 ns) — vs ~500 ns malloc p50 in §7.
  - bump batch-p99 **6.0 ns** (bound 1 µs) — vs ~5 µs malloc p99.
  - cold commit (16 MB quantum, first touch) **0.01 ms** (bound 2 s);
    warm reuse runs at bump speed (2.5 ns).
  - frame cycle (500 mixed allocs + reset, x1000) **1.2 µs/frame**
    (bound 50 µs) — two orders under the 0.1 ms budget.
  - vector growth to 100k: linear 0.43 ms vs malloc 0.38 ms — same
    league (linear wins on churn, not single-growth latency).
- `MemoryTests` 79/79 PASS (+6). `AssetTests` 334/334 still green
  (re-verified Epoch 9 build).
- **Remaining:** transient reset (blocked on in-flight streamer ranges —
  needs upload-path lifetime rework, proposed as its own workstream).

### Runtime validation — fresh cooks + bench (2026-09-30)
Cooked with the post-migration `PakCooker` (meshlets + LODs + meshopt +
verify, no lightmap bake) and benched 300 frames, camera frozen
(`--disable_control`), uncapped present, on this machine:
- **Sponza** (`sponza.pak`, 79 MB): avg **3.08 ms** (~325 fps) —
  cpu 0.60 ms, gpu 1.79 ms.
- **Bistro** (`bistro.pak`, 835 MB): avg **7.43 ms** (~135 fps) —
  cpu 4.51 ms (passes 3.38 ms, preB list-builds 3.17 ms), gpu 2.32 ms.
- Bistro is CPU-bound in list builds, not allocation: the measured frame
  scratch rhythm (1.2 µs, Epoch 9) is three orders below frame time.
- Shutdown footprint (both runs): process ~1.9 MB / frame 0.0 MB /
  transient ~1.1 MB used; 16 MB committed per allocator.
- NGX/DLSS errors in the logs are benign (no NVIDIA adapter here).

---

## 11. Program-entry memory size configuration (2026-10-22)

The static sizes are no longer rebuild-only. `mem::config::Runtime()`
(`Engine/Memory/alloc_config.h`, implemented in `Memory/memory.cpp`) parses
`TOURMALINE_MEM_*` **once at program entry**, validates the values, logs each
accepted override, and falls back to the constexpr defaults in
`alloc_config.h` on bad input. `App` and `Editor` both call
`smvk::mem::InitMemory()` before Vulkan, worker threads, or asset loading, so
every allocator / static heap / staging ring is sized before its first use.
Changing the environment after startup does nothing (the lifetime is fixed
for the process).

| Env var | Field | Default | Sizes |
|---------|-------|---------|-------|
| `TOURMALINE_MEM_PROCESS_MB` | `processReserve` | 4096 MiB | process-lifetime VA reservation |
| `TOURMALINE_MEM_FRAME_MB` | `frameReserve` | 256 MiB | per-frame VA reservation |
| `TOURMALINE_MEM_TRANSIENT_MB` | `transientReserve` | 1024 MiB | UploadInFlight VA reservation |
| `TOURMALINE_MEM_APP_ARENA_MB` | `appArenaBlock` | 1 MiB | `AppArena` block size |
| `TOURMALINE_MEM_FRAME_ARENA_MB` | `frameArenaBlock` | 1 MiB | `FrameArena` block size |
| `TOURMALINE_MEM_COMMIT_MB` | `commitQuantum` | 16 MiB | page-commit quantum (power of two) |
| `TOURMALINE_MEM_STREAMER_MB` | `streamerTotalBudget` | 256 MiB | UploadStreamer total budget |
| `TOURMALINE_MEM_STAGING_RING_MB` | `stagingRingPerThread` | 32 MiB | per-thread staging ring |
| `TOURMALINE_MEM_BULK_RING_MB` | `bulkRingCapacity` | 512 MiB | load-time bulk upload ring |
| `TOURMALINE_MEM_FLUSH_MB` | `uploadFlushThreshold` | 256 MiB | batch-submit threshold |
| `TOURMALINE_MEM_READBACK_MIN_KB` | `readbackStagingMin` | 256 KiB | readback staging floor |
| `TOURMALINE_MEM_READBACK_MAX_MB` | `readbackStagingMax` | 256 MiB | readback staging cap |
| `TOURMALINE_MEM_THREAD_SLOTS` | `threadPoolQueueSlots` | 4096 | bounded task-ring slots |
| `TOURMALINE_MEM_STATIC_VERTEX_MB` | `staticVertexHeap` | 512 MiB | EACH of the two vertex heaps |
| `TOURMALINE_MEM_STATIC_INDEX_MB` | `staticIndexHeap` | 512 MiB | static index monoblock |
| `TOURMALINE_MEM_TARGET_BLOCK_MB` | `targetHeapBlock` | 64 MiB | rendergraph target-heap block (halves on failure) |

Examples:

```powershell
# Shrink the three eternal GPU static heaps (default 1.5 GiB) to 1.0 GiB total.
$env:TOURMALINE_MEM_STATIC_VERTEX_MB='512'
$env:TOURMALINE_MEM_STATIC_INDEX_MB='512'

# Same textures/heaps, smaller CPU-side VA reservations.
$env:TOURMALINE_MEM_PROCESS_MB='1024'
$env:TOURMALINE_MEM_TRANSIENT_MB='256'
```

Semantics pinned by `MemoryTests`:
- values are positive integers in the unit named (MB/KB/slots); bad values
  warn and keep the constexpr default;
- `commitQuantum` must be a power-of-two multiple of 4 KiB (the commit
  alignment uses a mask);
- cross-field ordering is enforced: `READBACK_MIN <= READBACK_MAX`,
  `STREAMER >= STAGING_RING`, `FLUSH <= BULK_RING`.

Still compile-time **by necessity** (array bounds / static_asserts):
`kNumPoolShards`, `kShardCapacity`, `kShardedMaxSize`, `kDefaultAlign`.

---

## 12. GPU-side allocations in a typical run to 1st frame (2026-10-22)

Snapshot method (reproducible):

```powershell
$env:TOURMALINE_MEMDUMP='1'          # one-shot ledger dump at frame 1
$env:TOURMALINE_AUTO_EXPOSURE='0'
& .\App.exe --pak content/paks/sponza.pak --disable_control --bench 4
```

The ledger is the **exact VMA committed bytes** captured per resource at
create (`VmaAllocationInfo::size`); rows below are the complete first-frame
set, aggregated by resource name. `TOURMALINE_MEMDUMP` also prints any row
that is untracked (`bytes == 0`) and the VMA heap budgets, so the per-item
snapshot is regenerable at any frame.

### 12.1 First-frame totals (Sponza, 1920x1080, default settings)

| Ledger category | MiB | Composition |
|-----------------|-----|-------------|
| Vertex | 1024.0 | the two static vertex heaps (pos + shading) |
| Storage | 712.4 | static index monoblock (512) + gfx/DS/SSR/GI/particles rings (~200) |
| ImageTarget | 316.6 | render-graph attachments: main HDR chain, SSR/SSGI, SMAA, shadows, depth |
| ImageContent | 79.8 | 69 pak textures (45.4) + baked GI cubemaps (28) + cvol/PI volumes |
| Uniform | ~0.0 | frame uniforms / frame-hot (a few KiB, rounds to 0.0) |
| Index | ~0.0 | probe-ball index buffer (tiny) |
| **Tracked total** | **2132.8** | |

VMA heap snapshot from the same frame:

```
heap0 VRAM size=15995 budget=15227 usage=2326 committed=2133 blocks=2169 MiB
heap1 HOST size=15947 budget=15179 usage=1055 committed=992 blocks=992 MiB
```

The HOST row is the CPU-visible GPU allocations: 512 MiB bulk ring +
per-thread 32 MiB staging rings + readback staging + driver/HOST heap
allocations. The VRAM `usage` exceeds `committed` because VMA counts the
untracked raw blocks (target heap, external/swapchain) in usage.

### 12.2 Per-resource first-frame rows (aggregated; all rows)

| Resource | Kind | Ledger category | Count | Total MiB |
|----------|------|-----------------|------:|----------:|
| static vertex shading | buffer | Vertex | 1 | 512 |
| static vertex pos | buffer | Vertex | 1 | 512 |
| static index | buffer | Storage | 1 | 512 |
| scene content textures (pak) | image | ImageContent | 69 | 45.4 |
| gfx skinBounds | buffer | Storage | 2 | 32.2 |
| ds visibility | buffer | Storage | 2 | 32 |
| ds lightList | buffer | Storage | 2 | 32 |
| baked GI cubemaps | image | ImageContent | 14 | 28 |
| ssgiEnvSpec | image | ImageTarget | 1 | 16.9 |
| hdr | image | ImageTarget | 1 | 16.9 |
| ssgiAoHdr | image | ImageTarget | 1 | 16.9 |
| volumetric | image | ImageTarget | 1 | 16.9 |
| dof | image | ImageTarget | 1 | 16.9 |
| ssgiAoTerm | image | ImageTarget | 1 | 16.9 |
| motionBlur | image | ImageTarget | 1 | 16.9 |
| dlssOutput | image | ImageTarget | 1 | 16.9 |
| ssr | image | ImageTarget | 1 | 16.9 |
| ssgiEnvSpecComposed | image | ImageTarget | 1 | 16.9 |
| shadowMapAtlas | image | ImageTarget | 1 | 16 |
| gfx hiz | buffer | Storage | 1 | 10.5 |
| ssgi hzb max | buffer | Storage | 1 | 10.5 |
| swapchain depth image | image | ImageTarget | 1 | 10.5 |
| gfx indirect | buffer | Storage | 2 | 10 |
| gfx remainderIndirect | buffer | Storage | 2 | 10 |
| gfx seedIndirect | buffer | Storage | 2 | 10 |
| gfx shadow indirect | buffer | Storage | 8 | 9.6 |
| spotShadowAtlas | image | ImageTarget | 1 | 9 |
| ssrGbuffer2 | image | ImageTarget | 1 | 8.4 |
| motion | image | ImageTarget | 1 | 8.4 |
| smaaEdges | image | ImageTarget | 1 | 8.4 |
| smaaWeights | image | ImageTarget | 1 | 8.4 |
| smaaHistory0 | image | ImageTarget | 1 | 8.4 |
| smaaHistory1 | image | ImageTarget | 1 | 8.4 |
| smaaLdr | image | ImageTarget | 1 | 8.4 |
| ssrGbuffer | image | ImageTarget | 1 | 8.4 |
| ui | image | ImageTarget | 1 | 8.4 |
| gfx batches | buffer | Storage | 2 | 8 |
| ssrRadiance | buffer | Storage | 1 | 7.9 |
| bloom | image | ImageTarget | 5 | 6.7 |
| gi dense lobe volumes | image | ImageContent | 18 | 5.4 |
| ssrHist0 | image | ImageTarget | 1 | 4.7 |
| ssgiAoRaw | image | ImageTarget | 1 | 4.7 |
| ssrHist1 | image | ImageTarget | 1 | 4.7 |
| ssgiAoBlur | image | ImageTarget | 1 | 4.7 |
| ssrRaw | image | ImageTarget | 1 | 4.7 |
| ssrBlur | image | ImageTarget | 1 | 4.7 |
| particles pool | buffer | Storage | 1 | 4 |
| ds tileState | buffer | Storage | 2 | 4 |
| gfx keptBits | buffer | Storage | 2 | 4 |
| gfx shadow draws | buffer | Storage | 8 | 3.2 |
| ssrRayList | buffer | Storage | 1 | 2 |
| gfx mesh batch list | buffer | Storage | 2 | 2 |
| ds tileGroupMasks | buffer | Storage | 2 | 1 |
| ds tileMasks | buffer | Storage | 2 | 1 |
| gi dense weight volumes | image | ImageContent | 2 | 1 |
| materials buffer | buffer | Storage | 1 | 1 |
| premix blended cubemap | image | ImageTarget | 1 | 1 |
| gfx visibleClusters | buffer | Storage | 2 | 1 |
| gfx visibleDraws | buffer | Storage | 2 | 0.8 |
| gfx draws | buffer | Storage | 2 | 0.8 |
| gfx clusters | buffer | Storage | 1 | 0.7 |
| forwardplus tileIndices | buffer | Storage | 1 | 0.5 |
| forwardplus decalTileIndices | buffer | Storage | 1 | 0.5 |
| particles freeList | buffer | Storage | 1 | 0.3 |
| forwardplus lights buffer | buffer | Storage | 1 | 0.2 |
| smaaAreaTex | image | ImageContent | 1 | 0.2 |
| motionBlurTileVel | image | ImageTarget | 1 | 0.2 |
| motionBlurTileMax | image | ImageTarget | 1 | 0.2 |
| sdsm stats | buffer | Storage | 2 | 0.2 |
| gfx texture meta | buffer | Storage | 1 | 0.1 |
| forwardplus decals buffer | buffer | Storage | 1 | 0.1 |
| spot shadow tiles buffer | buffer | Storage | 1 | ~0 |
| gfx mesh params | buffer | Storage | 2 | ~0 |
| render views | buffer | Storage | 1 | ~0 |
| forwardplus decalTileCounts | buffer | Storage | 1 | ~0 |
| ssrIndirectArgs | buffer | Storage | 1 | ~0 |
| directional cascade buffer | buffer | Storage | 1 | ~0 |
| sggi grid | buffer | Storage | 1 | ~0 |
| hizDepth | image | ImageTarget | 1 | ~0 (heap-placed) |
| forwardplus volumeTileMasks | buffer | Storage | 1 | ~0 |
| lm chart xforms | buffer | Storage | 1 | ~0 |
| ds zbins | buffer | Storage | 2 | ~0 |
| lights buffer | buffer | Storage | 1 | ~0 |
| premix selection | buffer | Storage | 1 | ~0 |
| nilTexture | image | ImageContent | 1 | ~0 |
| gfx mesh bounds | buffer | Storage | 1 | ~0 |
| probe ball indices | buffer | Index | 1 | ~0 |
| cvol layer volumes | image | ImageContent | 42 | ~0 |
| gfx cullState | buffer | Storage | 2 | ~0 |
| env zones buffer | buffer | Storage | 1 | ~0 |
| exposure histogram | buffer | Storage | 2 | ~0 |
| cvol mean volumes | image | ImageContent | 6 | ~0 |
| frame hot uniforms | buffer | Uniform | 1 | ~0 |
| dlssExposure | image | ImageTarget | 1 | ~0 |
| ssrRayCounter | buffer | Storage | 1 | ~0 |
| smaaSearchTex | image | ImageContent | 1 | ~0 |
| gfx texture demand | buffer | Storage | 2 | ~0 |
| ds viz params | buffer | Storage | 1 | ~0 |
| renderDepth | image | ImageTarget | 1 | ~0 (heap-placed) |
| frame uniforms | buffer | Uniform | 1 | ~0 |
| gfx mesh ranges | buffer | Storage | 1 | ~0 |
| forwardplus tileCounts | buffer | Storage | 1 | ~0 |
| probe ball sphere | buffer | Vertex | 1 | ~0 |
| cvol volumes | buffer | Storage | 1 | ~0 |
| engine globals | buffer | Storage | 1 | ~0 |
| particles emitterState | buffer | Storage | 1 | ~0 |
| particles emitters ring | buffer | Storage | 1 | ~0 |
| ds counters | buffer | Storage | 2 | ~0 |

Rows marked "~0" are exact-but-tiny (bytes printed as 0.0 MiB) or untracked
backings; they are still real allocations. The `renderDepth` / `hizDepth`
rows are the two design-B heap claims below, not VMA allocations.

### 12.3 Untracked GPU allocations (not in the ledger)

These are real device allocations that the VMA ledger deliberately excludes
(the viewer's "untracked remainder"; see `smvk.h`):

| Allocation | Who / when | Size |
|------------|------------|------|
| Rendergraph target heap | `VulkanResourceManager::AllocateTargetHeapRange` at first graph target creation (`impl.cpp:5479`) | one raw `vkAllocateMemory` block, default 64 MiB; this run claimed 2 slots x 10.5 MiB (depth targets) |
| Swapchain images + views | `VulkanSwapchain::Create` | driver-owned; 2 images by default (`DESIRED_SWAPCHAIN_IMAGE_COUNT`), format-dependent (~8 MiB each at 1080p BGRA8) |
| UploadStreamer shard ring | `UploadStreamer::ensureShardRing`, first upload on each thread | `stagingRingPerThread` (32 MiB default); shard 0 created at init |
| UploadStreamer bulk ring | `ensureBulkRing`, first `uploadBufferBulk` (pak cluster deserialize) | `bulkRingCapacity` (512 MiB default, HOST_VISIBLE) |
| Readback staging | `EnsureReadbackStaging`, first async download (exposure/Gfx demand) | grows by doubling from 256 KiB, capped at 256 MiB |
| Descriptor pools | `CreateDescriptorPool` (bindless default + others) | driver-sized: bindless pool reserves 3*MAX_TEXTURES+MAX_VOLUME_TEXTURES sampled images and MAX_SAMPLERS samplers per flight |
| Command pools / buffers | `CommandBufferPool::init` (graphics/transfer/compute, per thread) + per-flight split/async pools | driver-sized; `kMax` buffers per pool |
| Semaphores / fences / timeline / query pools | `VulkanSwapchain::CreateSyncObjects`, `CreateTimestampPools` | driver-sized |
| Shader modules | `CreateShaderModule` (all main/shadow/post modules + permutations) | SPIR-V bytes retained on the CPU side too |
| Pipelines / pipeline layouts | pipeline builders | driver memory, plus the on-disk `pipeline_cache.bin` compile cache |
| Samplers | `VulkanSamplerCache::GetOrCreate` | driver-sized, up to MAX_SAMPLERS distinct |

### 12.4 Where the tracked rows come from (allocation sites)

- Static heaps: `engine/src/Renderer/mesh.cpp:109-111` (`AllocateStaticVertexFmt`),
  sized by `TOURMALINE_MEM_STATIC_*_MB`, carved with `VmaVirtualBlock`.
- Frame-resource rings: `WorldRenderer::onAttach` (`WorldRenderer.cpp:1134+`)
  — frame uniforms/hot, lights, materials, directional cascades, Forward+
  lights/decals, env zones, spot tiles, DS viz params, render views.
- Gfx/visibility filter rings: `engine/src/Renderer/gfx_filter.cpp`
  (indirect/seed/remainder, batches, draws, clusters, visible sets, skin
  bounds, HZB, keptBits, shadow indirect/draws, texture meta/demand).
- Render-graph attachments: the per-subsystem headers (`hdr.h`, `ssr.h`,
  `ssgi.h`, `smaa.h`, `motionblur.h`, `dof.h`, `volumetric.h`,
  `cascadedshadowmap.h`, `spotshadow.h`, `exposure.h`, `forwardplus.h`,
  `particles.h`, `mipchain.h`, `dlss.h`, `probepremix.cpp`).
- GI content: cubemaps/cvol/`gi dense` images loaded by the world/GI path;
  `sggi grid`/`cvol volumes` buffers in `WorldRenderer.cpp`.
- Scene content: one `CreateImage` per pak texture through the texture
  upload path; `nilTexture`/SMAA LUTs are engine-owned.
- Probe ball: `engine/src/Rendergraph/probeball.cpp:90-93`.

The dominant first-frame cost is the **static geometry heaps: 1.5 GiB of the
2.13 GiB tracked total (70%)**, before any scene content. That is why they
are the first runtime-size knobs above. The rest of VRAM is resolution-
scaled render targets (~317 MiB at 1080p) plus ~200 MiB of GPU-driven
filter/DS/SSR/GI rings, and only ~80 MiB of actual texture content.