# D3D12 RHI porting notes (for the Vulkan-familiar)

Status: living document. **Milestones 1-3 + SPONZA are green**: `D3D12Smoke`,
`D3D12RhiTest` (buffers, textures, bindless table sampling, samplers, swapchain,
render-graph triangle + sampled-texel gates), and `D3D12Sponza` — the ENGINE'S
OWN pak load stack (69 KTX2/BCn textures, 25 materials, 103 static meshes into
the shared static heap) drawn through the public RenderGraph/CommandBuffer
APIs, verified by screenshot (`sponza_final.png` at the repo root). Compute
pipelines / indirect / mesh tasks / transient-heap aliasing remain staged (§8).
Every "gotcha" below was hit for real on the RTX 5070 Ti + NVIDIA driver stack;
they are not hypothetical.

Branch `feature/dx12-rhi`, build gate `-DTOURMALINE_DX12=ON`, everything under
`engine/src/D3D12/` + `tools/d3d12_*`. The Vulkan tree is untouched; the D3D12
backend is a separate static lib (`engine_d3d12`) linked only by the D3D12
tools. The two facades (`smvk::ResourceManager`, `smvk::Swapchain`, later
`smvk::RenderGraph`/`smvk::CommandBuffer`) are implemented per-backend in
separate TUs — never link both into one binary (ODR: `smvk::Image` etc. are
defined in `dx12_rhi.h`, not `vulkan_impl.h`).

---

## 1. Build / architecture mapping

| Vulkan | D3D12 | Notes |
|---|---|---|
| `engine` lib (`src/Vulkan` + `src/RHI` facades) | `engine_d3d12` lib (`src/D3D12/*.cpp` + `spirv_reflection.cpp` + `vertex_format.cpp`) | `list(FILTER ENGINE_SRC EXCLUDE REGEX "/src/D3D12/")` keeps the Vulkan engine clean |
| volk / VMA | raw d3d12.h + `Microsoft::WRL::ComPtr` | committed resources only for now (no placed/placed-alias heaps yet) |
| `vk_loader` etc. | `d3d12.lib dxgi.lib dxguid.lib` + DXC (`dxcompiler.lib`, DLLs staged) | no loader dance |
| `VK_EXT_debug_utils` | `ID3D12Debug::EnableDebugLayer` + `ID3D12InfoQueue` | gated by `TOURMALINE_DX12_DEBUG=1` |

**Facade pattern**: the public RHI types (`smvk::ResourceManager`, `smvk::Swapchain`)
keep their headers; each backend defines its own `struct ...::Impl` in its own
internal header (`dx12_rhi.h`, `dx12_swapchain.h`) and forwards everything.
`ResourceManager::Impl = { tourmaline::dx12::Dx12ResourceManager dx12; std::vector<std::packaged_task<void()>> deferred; }`.

**Handle pools are shared code** (`Engine/Memory/pool.h`): `ShardedPool<Tag, T>`
+ `Holder<T>` + free `destroy(ResourceManager*, Handle)` overloads. The destroy
overloads are declared in `dx12_rhi.h`, defined in `dx12_resource_manager.cpp`
against `tourmaline::dx12::Dx12ResourceManager*` — the Holder deleter is a type-
erased lambda, so this links as long as the declarations precede instantiation.

**The smoke `tourmaline::dx12::Context`** (milestone 1, `dx12_context.h/.cpp`)
stays INDEPENDENT of the RHI: it owns its own device/swapchain. Accepted
duplication; migrating the smoke onto the RHI is optional cleanup, not a goal.

---

## 2. Resource creation

| Vulkan concept | D3D12 port | Gotchas |
|---|---|---|
| `VkImage` + `vkCreateImage` + VMA alloc | `CreateCommittedResource` on `D3D12_HEAP_TYPE_DEFAULT` | MSVC: pass `D3D12_HEAP_PROPERTIES`/`D3D12_RESOURCE_DESC` as NAMED LOCALS — `&HeapProperties(...)` fails C2102 (no rvalue address) |
| `VK_IMAGE_USAGE_*` initial layout | `InitialResourceState(usage)`: RT→`RENDER_TARGET`, DS→`DEPTH_WRITE`, Storage→`UNORDERED_ACCESS`, Sampled→`PS\|NON_PS_RESOURCE`, else `COMMON` | barriers must match the tracked state EXACTLY; the RM tracks `state` per resource and `Transition(list, res, state&, after)` updates it in place |
| `VK_IMAGE_USAGE_STORAGE` image | `desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` | **THE TDR BUG**: `BufferType_Storage` maps to `UNORDERED_ACCESS` barriers but buffers were created with `FLAG_NONE` → debug layer "states mismatch RESOURCE_FLAGS" → driver hung → `DXGI_ERROR_DEVICE_HUNG` TDR, detected one frame later at `Present`. ALWAYS set the flag for anything that will sit in `UNORDERED_ACCESS` (buffers too!). |
| `VkBuffer` + VMA | committed buffer on heap by type: `Staging`/`Uniform`→UPLOAD (so `MapBuffer` works), `TransferSrc`→READBACK, rest→DEFAULT | upload/readback heap resources must NEVER get barriers (they are implicitly GENERIC_READ/COPY_DEST) — never transition them |
| `VK_KHR_external_memory` win32 swapchain images | back buffers are fetched via `IDXGISwapChain::GetBuffer` and REGISTERED as external images with initial state `PRESENT` | |
| `vkCreateImageView` | `CreateShaderResourceView`/`CreateUnorderedAccessView` into CPU-only heaps (`viewSrvHeap_`, `viewUavHeap_`) at the view handle's index | view format mapping for depth SRVs: D32→R32_FLOAT, D24S8→R24_UNORM_X8_TYPELESS, D32S8→R32_FLOAT_X8X24 — mirrors Vulkan's view-format-on-depth trick |
| cubemap SRVs | `D3D12_TEXCUBE_ARRAY_SRV` has NO `FirstArraySlice`/`ArraySize` — it uses `First2DArrayFace` + `NumCubes` (faces, not cubes... `NumCubes` counts CUBES) | D3D12 filter enum names are inconsistent: 0x5 is `MIN_POINT_MAG_MIP_LINEAR` (no explicit second component); check `d3d12.h` before writing `D3D12_FILTER_*` names from memory |
| `vkCreateSampler` + immutable sampler array | one shader-visible SAMPLER heap (MAX_SAMPLERS=8) + `map<SamplerCreateInfo, slot>` cache | **slot contract is cross-backend**: 0=linear-repeat default, 1=shadow comparison (LessEqual), 2=linear clamp-to-edge. Shaders + baked materials hardcode `samplers[0/1/2]`. `Initialize()` populates + VERIFIES the order (misorder → hard error). |

---

## 3. Bindless (the big one)

**Vulkan model** (`common.hlsli`): set 0 has
- binding 0: `Texture2D textures[]` (8192 slots, `MAX_TEXTURES`), aliased by
  `Texture2D<uint4> uintTextures[]` (stencil-aspect views ride the same slots),
- binding 5: `TextureCube cubemaps[]` — SAME index space,
- binding 4: `Texture3D volumeTextures[]` — same index space,
- binding 6: `Texture2DMS<float4> texturesMS[]` — same index space,
- binding 1: `SamplerState samplers[]` + `SamplerComparisonState comparisonSamplers[]`
  (immutable, one array aliased both ways),
- plus `[[vk::push_constant]]` blocks (256 B, `kEnginePushConstantSize`).

D3D12 has exactly ONE bound CBV_SRV_UAV heap + ONE sampler heap
(`SetDescriptorHeaps`). You cannot bind "a heap per dimension". The port model:

- ONE shader-visible CBV_SRV_UAV heap, capacity `2*MAX_TEXTURES`:
  slots `[0, 8192)` = the SRV descriptor for that bindless slot (whatever
  dimension the view has — the descriptor encodes it), slots
  `[8192, 16384)` = the UAV descriptor for the same view (Storage usage).
- ONE shader-visible sampler heap (8 slots, stable indices).
- Shader-side access via **SM 6.6 dynamic resources**:

  ```hlsl
  // the exact equivalent of the Vulkan alias/bindless arrays:
  Texture2D<float4>  t = ResourceDescriptorHeap[bindlessIndex];
  TextureCube<float4> c = ResourceDescriptorHeap[bindlessIndex];
  Texture3D<float4>  v = ResourceDescriptorHeap[bindlessIndex];
  Texture2DMS<float4>[4] m = ResourceDescriptorHeap[bindlessIndex];
  RWTexture2D<float4> u = ResourceDescriptorHeap[bindlessIndex + 8192];
  SamplerState s = SamplerDescriptorHeap[0];
  ```

  Same slot, different typed fetch — precisely the `textures[]/cubemaps[]/
  volumeTextures[]` alias trick, minus the `vk::binding` annotations. This
  requires re-authoring the shared binding block in `common.hlsli` (once) and
  `#define`-level shims for `textures[i].Sample(...)` style fetches (a helper
  returning a local `Texture2D` works — SM 6.6 objects are values).
  D3D12 Ultimate hardware (RTX 5070 Ti: SM 6.7) is assumed — same assumption
  tier as Vulkan descriptor-indexing on this project.

- Bindless slot = `BindlessImage` pool handle index (`handle.index()`), the
  same "pool index is descriptor index" contract as the Vulkan backend.
- `CopyDescriptorsSimple` CPU-side: the view's SRV is created into the CPU-only
  view heap at the VIEW handle index (cheap dedupe) and copied into the bindless
  heap at the BINDLESS slot. Two indirections, zero GPU cost.

**Why not unbounded `register(t0)` arrays?** Two unbounded arrays cannot share
one heap (each claims "base→end"). Bounded arrays ([8192]x N) would work with
disjoint bases but then EVERY slot needs a valid descriptor or a stray index
faults the device. SM 6.6 dynamic access has neither problem and matches the
engine's alias model 1:1.

---

## 4. Uploads / readback

| Vulkan | D3D12 port | Notes |
|---|---|---|
| `UploadStreamer` ring + transfer queue, async | ONE direct-queue command list (`uploadList_`), batched: `OpenUploadList` → N staging maps/copies/barriers → `ExecuteUploadList` (Close+Execute+Signal+CPU wait) | synchronous (each flush = full fence wait). Simple and correct; the engine's batched call pattern (one FlushUploads per load phase) keeps this acceptable. A real ring (with in-flight fences + deferred staging release) is the perf follow-up. |
| `vkCmdCopyBuffer` + image layouts | `CopyBufferRegion` / `CopyTextureRegion` with `GetCopyableFootprints`; source assumed mip-major tightly packed (mip outer, layer inner); subresource index = `mip + layer*mipLevels` (3D: `mip` only; 3D uploads restricted to `mipLevels==1`) | image upload transitions `state → COPY_DEST → initial state` with the tracked state updated in place |
| async downloads (ticket/poll) | DEGRADED: `SubmitAsync...` copies synchronously (ticket=1), `Poll...` returns true | fine for the current callers (debug tooling); needs the fence-ring port eventually |
| `vkMapMemory` | `Map/Unmap` on UPLOAD/READBACK resources; `DownloadDataFromBuffer` maps the readback staging directly | D3D12 `Map` needs no allocation-time flag |

**Fence contract**: every D3D12 wait first checks
`fence_->GetCompletedValue() == UINT64_MAX` — that is the documented
"device removed" sentinel. Without the check a dead device makes every wait
"succeed" instantly and callers read garbage (this masked the real failure mode
during the TDR bug hunt). `DrainInfoQueue(context)` prints the removal reason.

**Deferred release (the DeferDestroy port)**: recorded-but-unexecuted upload
batches REFERENCE their destination images/buffers; a Holder can die in that
window (the Vulkan backend queues destroys behind fences for exactly this).
`Dx12ResourceManager::DestroyImage/DestroyBuffer` move the ComPtr onto a
deferred list keyed by the pending batch's fence value; the pool entry dies
immediately (slots stay reusable) and the resource releases when the batch
retires (`RetireDeferredReleases` at ExecuteUploadList end / Shutdown). The
debug layer CAUGHT the un-deferred version ("resource referenced in a command
list being closed was deleted prior to closing" — the texture upload path,
verified by naming the object). Exceptions: the swapchain's
`ReleaseBackBufferResources` releases immediately (`deferRelease=false`) —
`ResizeBuffers` fails `DXGI_ERROR_INVALID_CALL` while ANY back-buffer
reference, deferred included, is alive. Views/bindless entries are plain
descriptor values and release immediately. **The facade's
`DestroyImage/DestroyBuffer/DestroyTexture` methods must route through the
deferred helpers** — the inline free `destroy(ResourceManager*, ...)` overloads
in `resource_manager.h` call the FACADE methods, not the dx12-typed free
overloads.

**THE SPONZA BUG (the last one)**: `GetCopyableFootprints` reports
`rowSizeInBytes` PER ROW ALREADY — the upload staging divided it by the row
count, so for BC textures it staged ~ONE BLOCK PER ROW (8 bytes). Every KTX
texture landed 99% black; the draw sampled it correctly — the "black screen
with sparse vertical streaks" was literally each row's first block rendered.
Per-row staging bytes = `min(sourceRowBytes, footprint.RowPitch)`. Found by
reading the FULL mip 0 back (the first-texels probe had masked it) + the
forced-known-texture bisect.

---

## 5. Swapchain

| Vulkan | D3D12 | Notes |
|---|---|---|
| acquire image + semaphore | there is no acquire — `GetCurrentBackBufferIndex()` + PRESENT→RT barrier recorded into the frame's own list | the swapchain owns 2 `Frame`s (allocator/list each, `kFramesInFlight`) and its OWN fence (separate from the RM's upload fence) |
| `vkAcquireNextImageKHR` in-flight throttling | Acquire waits the frame's fence value before `allocator->Reset()` | |
| present + layout decay | `RT → PRESENT` barrier then `Present(syncInterval, flags)`; FIFO=1, Immediate=0(+`DXGI_PRESENT_ALLOW_TEARING` when `IDXGIFactory5::CheckFeatureSupport` says tearing is allowed) | `AllowTearing` flag on the swapchain only when supported |
| `vkQueuePresentKHR` | `Queue()->Signal(fence)` AFTER Present; `frameIndex_` round-robins | Presenting to a HIDDEN window works fine (the smoke + rhi test do it) |
| `outOfDate` / recreate | resize detected at Acquire (`SDL_GetWindowSizeInPixels` vs cached) → caller calls `Recreate()`: wait all frames → release back buffer + depth registrations → `ResizeBuffers` → re-fetch + re-register | **`~Swapchain()` MUST call the internal `Shutdown()`** — the default dtor would drop ComPtrs without the fence wait and leak the DXGI swapchain state |
| image count / formats | flip-discard, `bufferCount_=2` (RTX: DXGI format 28 = R8G8B8A8_UNORM_SRGB), depth D32 | swapchain depth is an RHI-created image (DS|Sampled) so the graph can attach it |

**sRGB gotcha (two parts, both bit hard)**:
1. **Flip-model swapchains reject the `_SRGB` format at creation outright**
   (`DXGI_ERROR_INVALID_CALL` on the NVIDIA driver): create with the BASE
   UNORM format (`SwapchainCreationFormat`) and render through explicit
   `_SRGB` RTVs instead.
2. **A null-desc RTV defaults to the resource's base UNORM format** — the
   hardware sRGB encode NEVER happens silently (empirical: a 0.75 linear
   write stored as byte 191, not 225). The engine contract is
   "the swapchain is an _SRGB format (hardware encodes on store)" — so every
   RTV creation passes an explicit view format derived from the smvk format
   (`GetOrCreateRtv`, the swapchain's own RTVs). Same trap will apply to any
   view-format-sensitive path (SRVs are explicit already).

Back buffers register as EXTERNAL images with `state = PRESENT` and get
sampled SRV views (the UI composite pass samples them in the Vulkan engine).

****NDC / winding (RESOLVED — the user-visible inversion)**: the NDC conventions are OPPOSITE — D3D12's +Y points UP (the GL convention; the viewport transform maps it to the top-left-origin screen), Vulkan's points DOWN. The engine's Vulkan compensation (proj[1][1] *= -1, UpdateFrameUniforms) must NOT apply on D3D12: carried over, the atrium rendered upside down (and the mirror inverted the screen-space winding, which is why the gate dodged with CullMode::None). With the flip removed, the glTF CCW front faces map straight: CullMode::Back at the engine's FrontFace::CounterClockwise is correct (verified empirically — full coverage, mean luma identical). The backend difference is exposed as ResourceManager::NeedsProjectionYFlip() (Vulkan true, D3D12 false) so the engine's frame-uniform build queries it instead of forking with ifdefs when WorldRenderer ports.
---

## 6. Diagnostics (use them — they found the TDR)

- `TOURMALINE_DX12_DEBUG=1` → debug layer + `ID3D12InfoQueue` captured;
  `Dx12ResourceManager::DrainInfoQueue("tag")` prints all stored messages +
  `GetDeviceRemovedReason()` at every failure site (present / recreate /
  download / fence wait / shutdown). Without it, the only symptom of a dead
  device is DXGI hr codes two frames late.
- `Dx12ResourceManager::SetDebugName(object, name)` (debug-layer-gated) is
  the `VK_EXT_debug_utils` equivalent: named objects made the info-queue
  messages actionable ("'test checker' was deleted prior to closing...") —
  that's what identified the deferred-release gap. Name new GPU objects as
  they land.
- The debug-layer device does NOT QI to `IDXGIDevice` (E_NOINTERFACE)! To find
  the owning adapter (for `QueryVideoMemoryInfo`) use
  `ID3D12Device::GetAdapterLuid()` + `factory->EnumAdapters1` LUID compare.
  `GetGpuMemoryInfo` failed until that was fixed — pure debug-layer quirk.
- D3D12RhiTest's `Check()` prints OK to stdout / FAIL to stderr; the test sets
  `setvbuf(stdout, nullptr, _IONBF, 0)` so a crash can't swallow the OKs
  (stderr is unbuffered — a crash mid-run loses buffered stdout only).

---

## 7. Current stubs (honest list)

- KTX texture load (`CreateTexture(ktxTexture2&)`) → empty handle
- `CreateStreamingTexture` / `RetargetStreamingTexture` → empty/no-op
- `CreateDescriptorPool` / `ClaimTargetHeapRange` → empty (no descriptor-pool
  concept needed yet; revisit when the target-heap aliasing paths get ported)
- `SetImageViewVkView` / `GetTextureVkImageView` / `GetVkSamplerOpaque` →
  nullptr (ImGui Vk backend glue — the D3D12 ImGui backend won't need them)
- `HasDedicatedComputeQueue` → false, queue families → 0
- `SupportsMeshShaders` → false (DX MS is available on this HW; port when the
  mesh-shader gfx filter path gets ported)
- async downloads → synchronous (see §4)
- `SupportsEvsm` / `SupportsMSAA2x` / `SupportsBc6h3DSampling` → real
  `CheckFeatureSupport` probes (done)

---

## 8. Milestone 3 — what landed (the render-graph slice + sponza)

`engine/src/D3D12/dx12_rendergraph.h/.cpp` implement the
`smvk::RenderGraph` + `smvk::CommandBuffer` facades:

- **PSO cache** (`Dx12RenderGraph::GetOrCreateGraphicsPipeline`): linear-
  scan key = ShaderProgram + VertexFormat + Depth/Stencil/Blend/MSAA/Raster
  (operator< equivalence for the settings that only define order) +
  topology + attachment formats + depth format. VS+PS DXIL for graphics;
  compute PSOs live in their own program-keyed cache (§9's compute item).
- **Fixed root signature**: slot 0 = root CBV (the 256-byte push-constant
  block). `PushConstants(offset, size, data, stages)` = write into a
  per-PASS shadow block + copy the FULL block to a fresh 256-aligned slice
  of a per-FLIGHT UPLOAD ring + `SetGraphicsRootConstantBufferView(0)` —
  that keeps the accumulate-by-offset contract of `vkCmdPushConstants`
  (scene push once, per-mesh tails at offset 192 later in the same block)
  with zero reset bookkeeping. 256 B of ROOT CONSTANTS would eat the whole
  64-dword budget, hence the CBV ring. The ring is per flight; overwriting
  flight f at frame f+2 is safe because Acquire waits flight f's fence.
- **`BindRenderPipeline`**: PSO + `SetGraphicsRootSignature` +
  `IASetPrimitiveTopology` + root-CBV rebind + a graphics-bind fast path
  mirroring the Vulkan one. **Input-layout trap**: the root signature needs
  `D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` or
  `CreateGraphicsPipelineState` returns E_INVALIDARG with the debug layer
  spelling it out.
- **Vertex strides live in the VIEWS, not the pipeline** (vs Vulkan where
  they live in the pipeline's vertex input state): `BindRenderPipeline`
  seeds per-binding strides from the format description and re-issues
  cached bindings; `BindVertexBuffer` records + re-issues with the seeded
  stride.
- **`Execute(fb)`**: resolve transients (plain per-frame create; no design-B
  interval-packed aliasing yet), `SetDescriptorHeaps` once (ONE
  CBV_SRV_UAV + ONE sampler heap), then the barrier model (§9):
  usages → topo sort → per pass: the barrier batch → on-demand
  RTV/DSV (`GetOrCreateRtv/Dsv`, CPU-only heaps, exhaustion follows the
  `OnResourceExhaustion` call-site contract) → `OMSetRenderTargets` +
  loadOp clears → viewport/scissor → the recording lambda (compute
  passes: the batch + the lambda only). One pass = one
  `CommandBuffer`.
- **Gate**: D3D12RhiTest compiles its own SM 6.6 VS/PS at runtime through
  DXC (`-T vs_6_6 -E mainVS -HV 2021`; **the -E must match the real entry
  function name** — "missing entry point definition" otherwise; dxcapi.h
  needs the Windows headers included FIRST), draws a push-tinted triangle
  into the back buffer via the public API, presents, copies the back buffer
  back (`DownloadDataFromImage`) and asserts the sRGB-encoded pixels
  (225/188/137 from a 0.75/0.5/0.25 linear tint) — plus the PSO cache
  dedupes across frames (cache count == 1 after three frames). A SECOND
  gate samples a 1x1 texture through the bounded register(t0) array +
  `samplers[0]` (the raw-bytes-are-LINEAR trap: expected 165/208/241, not
  the naive 96/160/224), including the HIGH-SHARD case — worker-thread
  textures at pool indices 512..8191 via `smvk::RegisterPoolShard()`
  (the sponza topology; the ShardedPool gives each THREAD one 512 slot).

**SPONZA (the content gate)**: `tools/d3d12_sponza` — the ENGINE'S OWN
load stack runs unchanged on engine_d3d12 (`World::LoadClusterFromPak` →
`DeserializeCluster` → parallel KTX loads → `UploadStaticMeshesDirect` →
`ResolveAndFixup`), then a minimal draw loop: `forEachRenderItem` →
`BindRenderPipeline(StaticVertexFormat)` + `BindStaticVertexFmt` +
per-surface `PushConstants` + `DrawIndexed`. The material texture slots
go through the ENGINE'S OWN pool→bindless remap (`BuildPoolToBindlessMap`
— `values.textures[0]` stores the RHI TEXTURE POOL index; the shader's
arrays index the BINDLESS heap; the identity only holds while nothing
churns the pools). The screenshot (a staging-image copy INSIDE the
frame's list before the present — flip-discard makes post-present
content UNDEFINED) verifies real content; 5/5 deterministic, the debug
layer silent.

**Shared-tree extractions** (both engine libs compile these now; the
Vulkan suite still passes 35/35):
- `engine/src/Renderer/vertex_format.cpp` — `GetVertexFormatDescription`
  out of `mesh.cpp` (which is Vulkan-coupled through the static heap).
- `engine/include/Engine/Renderer/static_vertex_heap.h` — the
  `StaticVertexFmtHeap` + the CPU `StaticHeapArena` (the VmaVirtualBlock
  replacement: opaque uint64 tokens, a coalesced free list, the same
  contract shape — the gfx filter's allocBits packing works verbatim) +
  `StaticMeshAllocation`; `mesh.cpp`'s `AllocateStaticVertexFmt` +
  `cluster.cpp`'s upload paths are now pure facade, so the WHOLE load
  stack (pak → cluster → world) compiles into `engine_d3d12` unchanged
  (~20 TUs — see ENGINE_D3D12_SRC in engine/CMakeLists.txt; embree +
  OptickCore + imgui_core ride along).
- `mesh.h` includes glm → glm is now a PUBLIC dep of `engine_d3d12`
  (imgui_core too, via core.h in camera.h's include chain).

**RenderGraph is a ~MB-scale object**: three inline `Pool` members, each
embedding an `objects_[8192]` array. A stack local = STATUS_STACK_OVERFLOW
(hit it). Heap-allocate: the engine uses `mem::MakeProcess<RenderGraph>(ctx)`
(WorldRenderer), the gate test does the same.

Staged stubs (printed once per call, not silent): `DrawIndexedIndirect`,
`DrawMeshTasks` (DX MS), `WriteStageTimestamp` (timestamp query heaps).
`BindComputePipeline`/`Dispatch`/`DispatchIndirect` are REAL now (the
compute milestone — see §9's compute item); `BindStaticVertexFmt` is real
(the static-heap rework landed with the sponza slice);
`SetStencilReference` is real (`OMSetStencilRef`).

## 9. Open concerns / TODOs (the honest list)

- **Barrier model port — DONE (the item-3 pass)**: the D3D12 Execute now
  runs the Vulkan executor's model — `CollectPassUsages` +
  `TopologicalSort` verbatim (the sync REQUESTS were already
  backend-neutral RHI data), the per-pass barrier batch mapped to states.
  The mapping rules, MEASURED on the Win10 22H2 runtime (RTX 5070 Ti):
  a state CHANGE is a plain transition (a D3D12 transition orders ALL
  prior accesses to the resource against all subsequent ones — one
  barrier form where Vulkan needs per-stage mask pairs); the SAME state
  splits by class — OM writes (RENDER_TARGET/DEPTH_WRITE) and copies are
  implicitly ordered within one command list (no barrier form needed),
  while UAV-state write handoffs (WAW/RAW/WAR — the compute-chain case)
  take the per-resource UAV barrier, which is the ONLY same-state form
  the runtime accepts: **it REJECTS UAV barriers on resources without
  `ALLOW_UNORDERED_ACCESS` — `Close()` fails with `E_INVALIDARG`** (the
  first port cut emitted them for OM-state WAW and both gates caught
  it). The tracked state lives ON the pool records (mutated inline as
  the list records) — the Vulkan executor's snapshot/commit map pair
  exists for its async-split cross-queue handoffs; D3D12's single direct
  queue records one list in order, and command lists SERIALIZE per
  queue, so the Vulkan executor's cross-frame timeline wait (frame N's
  Main writing hdr vs N-1's Tonemap reading it — same-queue submissions
  may overlap per the Vulkan spec) is structurally unnecessary here.
  First-use-of-frame needs no barrier for the same reason (the "last
  access this frame was a WRITE" bookkeeping is a local map, not the
  record state). Gates: the rhi test's barrier-chain gate (consumer
  pass inserted FIRST — the sort must reorder it last; WAW on the
  unchanged RT state; the RT→SRV RAW handoff; the pixel must be the
  second write's texel through the sRGB encode) + the UAV WAW gate (a
  Storage image written by two graphics PSes through the u0 bounded
  table — the per-resource UAV barrier is load-bearing; the readback
  must show the second write's texel). Remaining barrier-model items:
  the MSAA resolve (RESOLVE_DEST states), the async split, and the
  design-B transient interval packing (placed heaps + aliased
  resources).
- **Async compute — EMULATED on the main queue (the accepted limitation,
  confirmed)**: async-marked passes execute inline in topo-sorted order
  on the single direct queue. CORRECTNESS is identical (the sort + the
  barrier batch order the passes exactly as the split would); what is
  lost is OVERLAP: the dedicated-compute window that motivates the split
  (the EVSM prefilter/blur running while G1's graphics work continues)
  never happens — every async-marked pass serializes on the main queue,
  so frames with heavy async work pay its full cost on the critical path
  instead of hiding it. The Vulkan G1/C/G2 submit chain (per-pass C
  command buffers + timeline semaphores + the swapchain's EnsureAsyncSplit)
  has no D3D12 equivalent here; a future port needs a second
  ID3D12CommandQueue (D3D12_COMMAND_LIST_TYPE_COMPUTE) + cross-queue
  fences per flight — the barrier model itself is already queue-neutral
  (the per-resource barriers + the sort carry the ordering; only the
  submit plumbing changes). Until then: no correctness gap, a perf
  ceiling on async-heavy scenes (EVSM blur, same-frame modes).
- **Compute passes — DONE (the compute milestone)**: `BindComputePipeline`
  builds/loads a compute PSO from a separate linear-scan cache keyed on
  the program alone (no attachments/formats to vary it —
  `GetOrCreateComputePipeline`; the graphics `PipelineCacheCount` stays
  the GRAPHICS population, the hitch-logger contract) and binds it with
  the SAME fixed root signature + the three descriptor tables at the
  COMPUTE binding space (`SetComputeRootDescriptorTable` — the two spaces
  are INDEPENDENT: compute binds do not disturb the graphics tables).
  `Dispatch` = the bound-PSO guard + `FlushComputePush` (the deferred
  ring slice at the first consuming dispatch, its own bind-point flag) +
  `Dispatch`. `DispatchIndirect` = the graph-owned 3-uint dispatch
  command signature (`DispatchSignature`, lazily created) +
  `ExecuteIndirect` — the ARGS state is the barrier model's job (the
  facade contract: the consuming pass declares IndirectCommandRead +
  DrawIndirect, which the Execute batch maps to
  `D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT`). **The single-PSO-slot
  trap**: D3D12 has ONE pipeline-state slot (unlike Vulkan's separate
  graphics/compute bind points) — a compute `SetPipelineState` DISPLACES
  the graphics PSO, so the shared-header fast path's unconditional
  pointer restore (correct for Vulkan) would leave a compute PSO bound
  at the draw. The fix: the frame-ctx `boundComputePso` flag guards the
  graphics fast path (a compute bind in the pass forces the full
  re-issue) and doubles as Dispatch's bound-pipeline check. Gate: the
  rhi test's compute-chain gate — a CS writes a 4x4 Storage image
  through the u0 table TWICE, the second dispatch via
  `DispatchIndirect` (uploaded args; the GPU-produced form waits on the
  buffer-bindless/BDA rework), the readback must show the second write
  everywhere — real Dispatches now, not pixel-shader UAV stores.
- **Shader recompile**: `CompileShaders.cmake` needs a DXIL variant (same
  HLSL, `-T *s_6_6`, no `-fspv-*`); `HLSL_FORCE_O3_FILES` semantics carry
  over. The gate compiles at runtime; the engine shader build is not
  blocking but is required before any engine pass ports.
- **The `vk::`-annotation layer**: `common.hlsli`'s binding block (§3) must
  be re-authored for the bounded-table register layout (the alias model maps
  1:1 — `textures[]` becomes `Texture2D<float4> textures[8192] :
  register(t0)` etc. — but the `vk::binding` attributes are SPIR-V-only).
- **THE SPONZA DEBUG STORY (the bug that hid behind four others)**: the
  "black screen with vertical streaks" was the CORRECTLY-SAMPLED content
  of textures that were 99% black — each row's FIRST BLOCK had staged
  (`GetCopyableFootprints`' `rowSizeInBytes` is PER ROW already; dividing
  it by the row count staged ~8 bytes/row). The decisive diagnostics were:
  (1) the forced-known-texture bisect (a raw 1x1 forced for every material
  — full-screen coverage proved the draw path), (2) the FULL-mip readback
  (mean luma 0 with the first 8 bytes valid — the first-texels probe had
  masked it), (3) ASCII-luma screenshots (no image input needed in this
  session — 80x40 char maps revealed real architecture once fixed).
- **Sampler heap slots**: keep the baked-material slots (0/1/2) contract.
- **Push-constant ring**: updates accumulate in the per-pass SHADOW by offset; the 256B slice copy + the root-CBV bind are DEFERRED to the first consuming draw/dispatch (several pushes before one draw cost ONE slice; pushes that precede no draw cost none; a pipeline rebind invalidates the slot and the next draw re-binds - pushGfxBound/pushComputeBound per bind point, a dirty shadow always reallocates). The per-flight rings reset per frame; a 64-dword ROOT-CONSTANT path could be added for small blocks later (two root signature variants) - measure first.- **Compute queue**: single direct queue only. `HasDedicatedComputeQueue`
  reports false; the EVSM async prefilter path stays emulated until the
  compute queue + the split-submit port.
- **Heap growth**: bindless heap capacity is compile-time 2x8192; the strict
  exhaustion contract (`smvk::OnResourceExhaustion` call-site pattern)
  applies at pool-create sites — spread the gate as new exhaustion-prone
  sites appear.
- **Bindless slot retirement**: destroying a bindless image leaves a stale
  descriptor in the heap (Vulkan relies on null descriptors there); a
  freed-slot policy (write a null SRV at retire) is pending for churny
  loads.
- **MSAA + resolve**: `GetOrCreateRtv/Dsv` create 2D mip-0 views only;
  MSAA targets, resolve destinations (`SetColorWithResolve`) and array
  targets need explicit view descs when those paths port.
- **ImGui**: the SEAM landed (ui:: in imgui_overlay.h) — the overlay TU is backend-neutral (Init/NewFrame/record/AddTexture/Remove/Shutdown all behind ui::; the Vulkan side lives in impl.cpp, the D3D12 side is a documented stub until the imgui_impl_dx12 port: the vendored backend exist; the port needs the shared-descriptor-heap form — its SRV heap + root signature over our ONE bindless heap, the font texture through the upload path, the bindless-slot AddTexture form).
- **DLSS**: THE last Vulkan-coupled renderer TU (dlss.cpp). The public contract (dlss.h) is neutral; the coupling is Streamline's sl::Resource constructor (takes VkImage/VkImageView/VkImageLayout) + the vkCommandBuffer in the evaluate path. SL's interposer supports D3D12 (a different Resource form: ID3D12Resource* + state). The port seam: a backend-native resource-tagger turning (ImageHandle, ImageLayout) into sl::Resource + the native cmd handle; the rest (slInit, frame tokens, evaluate sequencing) is shared. A real validation chunk — not a mechanical swap; NOT DONE, the plan stands.- **Timestamps / GPU stage timers**: query heaps + `EndQuery` pairs at
  WriteStageTimestamp; the swapchain's per-flight pools mirror the Vulkan
  `GpuStageTimer` design when ported.

---

## 10. The engine port onto the RHI � what the seams proved, what remains

The goal (the user's framing, and the right one): WorldRenderer and the
whole engine tree sit ONLY on the RHI facade; a backend is a linked
library, nothing more. Status after the seam session:

**Backend-neutral engine TUs (all verified linking both libs):**
`WorldRenderer.cpp` was ALREADY clean (zero Vulkan symbols � the
"Vulkan-coupled renderer" was never WorldRenderer itself; the coupling
was in its satellites). The 2026-10 session landed four seams:
- `gfx_filter.cpp` � the vestigial vulkan_impl.h include removed (the
  static-heap port had already left nothing behind; two `VkDeviceSize`
  casts were the last Vulkan types, now `uint64_t`).
- `skinning.cpp` � the raw `vkCmdPipelineBarrier2` (the BDA deformed
  heap's compute->graphics dependency) behind the new
  `CommandBuffer::EmitGlobalMemoryBarrier()` (rendergraph.h, the shared
  header): Vulkan emits the all-commands barrier; D3D12 emits a
  null-resource UAV barrier. THE pattern for item 3 below.
- `profile.cpp` � `InitVulkanGPU` moved behind
  `profiler::InitBackendGpuTimelines[Impl]` (impl.cpp carries the Optick
  Vulkan init; dx12_rhi_globals.cpp the not-wired notice). The TU now
  carries only the neutral gates/histogram (its unconditional
  `#include <optick.h>` via profile.h keeps it out of engine_d3d12's TU
  list until that include is gated � the seam contract is done).
- `imgui_overlay.cpp` � the full `ui::` seam (Init/NewFrame/record/
  AddTexture/AddTextureView/RemoveTexture/ShutdownBackend, imgui_overlay.h):
  the overlay TU is backend-neutral; the Vulkan side lives in impl.cpp
  (the descriptor pool + ImplVulkan init + the paired SDL3 platform init
  + the texture descriptor sets); the D3D12 side is a documented stub
  until the imgui_impl_dx12 port (the shared-descriptor-heap form).

**The last coupled TU: `dlss.cpp`** � Streamline's sl::Resource takes
VkImage/VkImageView/VkImageLayout + the evaluate path's vkCommandBuffer.
dlss.h's contract is neutral; the seam is a backend-native
resource-tagger + the native cmd. Not done; a validation chunk (SL's
D3D12 interposer form needs proving).

**ITEM 3 (the barrier model - the main worry): DONE - landed and gated.**
The graph's sync REQUESTS are ALREADY backend-neutral RHI
data - `ImageSyncRequest`/`BufferSyncRequest` with
`AccessFlags`/`PipelineStageBit` masks + `ImageLayout` live in smvk.h
(the shared header); the engine declares them per-pass through the shared
RenderPass API. The Vulkan executor maps that neutral data to
vkCmdPipelineBarrier2 (EmitBarriers + TopologicalSort - both DECLARED in
the shared rendergraph.h, implemented per-backend). The D3D12 port WAS a
MAPPING-LAYER port on the same data, not an architecture change:
- `TopologicalSort` ported VERBATIM (pure bookkeeping over usages).
- The barrier emission ports as the per-pass batch in Execute (the LIVE
  Vulkan form - the standalone `EmitBarriers` member is its legacy
  mirror, uncalled): requests map to `ResourceBarrier` transitions via
  the RM's tracked record state + `ImageLayoutToState`, buffer states
  (UNORDERED_ACCESS for writes, COMMON for reads), and per-resource UAV
  barriers for the same-state UAV hazards. See 9's measured rules (the
  runtime's UAV-barrier rejection; implicit OM ordering; the
  queue-serialized cross-frame argument).
- The async-compute split: single direct queue - async-marked passes run
  inline in sorted order (the split is a future second-queue port).
- The depth fixup (depth-format + ShaderReadOnly promotes to the
  read-only depth layout) is LOAD-BEARING on D3D12: the SRV-over-depth
  state is DEPTH_READ (a plain shader-resource state on a depth resource
  is a debug-layer error), not just conservative like the Vulkan form.
Gates: the barrier-chain gate (topo reorder + WAW + the RT-to-SRV
handoff) + the UAV WAW gate (the per-resource UAV barrier through the u0
bounded table) in D3D12RhiTest; sponza stays deterministic (mean luma
91), the debug layer silent across all three tools.

**ITEM 4 (the shader build): the honest state.** The DXIL twin compiles
the same HLSL through the same DXC (no -spirv), SM 6_5 (the box's
HighestShaderModel), `-DTOURMALINE_DXIL=1` flips common.hlsli's binding
block to the bounded-register form (the five 8192-wide regions matching
the root signature's tables). The `vk::` ATTRIBUTES pass DXC un-ignored
(warnings). BUT `vk::BufferPointer` (the engine's BDA constants model �
FrameUniforms/lights/materials/renderViews/... in the push blocks) is
SPIR-V-ONLY: 108/111 stage shaders fail on it (measured; 3 debug
shaders without it are the live allowlist, `HLSL_DXIL_OK_FILES`). The
remaining rework: push blocks carry OFFSETS into bound
ByteAddressBuffers instead of raw BDAs � the root signature gains a few
constant-buffer table slots (one BAB per engine buffer table) and the
push shims from address to (slot, offset). That is the real per-shader
mountain (52 files carry BufferPointer types) � the build plumbing, the
binding shim, and the measurement are done; the rework is the milestone.

## 11. The missing-steps inventory (the handoff before D3D12 integration)

The backend runs the sponza gate deterministic + the full barrier model +
real compute dispatches. What is MISSING before the backend could integrate:

1. **The BDA rework (THE blocker for engine compute passes)** -
   `vk::BufferPointer` (52 files, the push-block address model) is
   SPIR-V-only, 108/111 stage shaders fail DXIL. The rework: push blocks
   carry (slot, byte-offset) pairs into bounded ByteAddressBuffer arrays
   (the u0/t0 regions - see 10's ITEM 4) + a buffer-bindless registration
   API on the backend (today only images have CreateBindlessImage; storage
   buffers have no slot assignment). Until then NO engine compute pass
   runs on D3D12 (only gate-authored bounded-table shaders).
2. **MSAA resolve** - the resolveView entries land in usages as
   color-attachment writes (RENDER_TARGET), but real resolves need
   RESOLVE_SOURCE/RESOLVE_DEST states + ResolveSubresource.
3. **Async compute** - EMULATED inline on the main queue (the accepted
   limitation, 9: correctness identical, the loss is overlap; a future
   port needs a second COMPUTE queue + per-flight cross-queue fences).
4. **Design-B transient interval packing** - placed heaps + aliased
   resources (currently plain per-frame create/destroy).
5. **DrawIndexedIndirect + DrawMeshTasks** - staged stubs (DX MS needs
   mesh-shader PSOs).
6. **GPU stage timers** - WriteStageTimestamp is a stub (timestamp query
   heaps).
7. **The ImGui D3D12 port** - the ui:: seam's D3D12 side is a stub
   (imgui_impl_dx12 adapted to the shared bindless heap + the fixed root
   signature).
8. **DLSS/Streamline** - dlss.cpp is the last Vulkan-coupled renderer TU
   (sl::Resource takes VkImage/VkImageView/VkImageLayout).
9. **Optick D3D12** - profile.h's unconditional <optick.h> include keeps
   profile.cpp out of engine_d3d12's TU list (the seam contract itself is
   done); OPTICK_USE_D3D12 is pinned off in the root build.
10. **The barrier trace** - TOURMALINE_BARRIER_TRACE not ported (the
    debug layer covers state validation in the meantime).

**Master-integration candidates (backend-neutral, inert on Vulkan)** - the
seam contracts the D3D12 work surfaced, replayable onto master WITHOUT the
backend: the seams commit's neutral half (gfx_filter/skinning's
EmitGlobalMemoryBarrier facade + the profile seam + the imgui ui:: seam +
the DXIL twin build plumbing + common.hlsli's gated binding shim). The
bigger replay (the RMI facade surgery + the static-heap de-Vulkanized load
stack + NeedsProjectionYFlip, from the earlier milestones) chains the
shared headers first - a deliberate follow-up, not a cherry-pick.