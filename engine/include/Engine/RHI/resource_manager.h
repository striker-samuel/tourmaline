// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include <Engine/smvk.h>
// Windowing is SDL3 (see App/app.h). Forward-declared so RHI headers stay
// light; .cpp files include <SDL3/SDL_video.h>.
struct SDL_Window;
#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>
#include <Engine/Renderer/spirv_reflection.h>
#include <memory>
#include <vector>
#include <span>

struct ktxTexture2;

namespace smvk {

// True when an opt-in debug image dump is armed via env: TOURMALINE_SSR_DUMP
// or TOURMALINE_EVSM_DUMP present with a frame number >= 0 (the same arm
// condition the SceneRenderer one-shot dumps use). Dump readback goes
// through ResourceManager::DownloadDataFromImage, which needs TransferSrc
// on the source image; render targets add that usage flag only when this
// returns true, so the normal path keeps the leanest usage set.
bool DebugImageDumpArmed();

// True when the device was created with VK_EXT_mesh_shader enabled
// (taskShader + meshShader features, probed, never required). Gates the
// mesh-shader gfx filter path; the compute+indexed CPU fallback stays
// otherwise. Also settable off at device level via TOURMALINE_MESH=0.
bool SupportsMeshShaders();

class ResourceManager {
public:
    // Creates Vulkan core + allocator. window is currently REQUIRED (the
    // core's device pick + queue selection are surface-based - see
    // CreateVulkanCoreWithWindow, which asserts it). Offscreen hosts pass a
    // HIDDEN window (the retirement fuzzer does); true headless would need
    // the core surgery in findQueueFamilies/isDeviceSuitable.
    explicit ResourceManager(SDL_Window* window = nullptr);
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;
    ResourceManager(ResourceManager&&) noexcept;
    ResourceManager& operator=(ResourceManager&&) noexcept;

    // Resource creation (RHI-only types, no Vk)
    Holder<ImageHandle> CreateImage(const ImageCreateInfo& ci, const void* data = nullptr);
    Holder<TextureHandle> CreateTexture(const ImageCreateInfo& ci, const void* data = nullptr);
     Holder<TextureHandle> CreateTexture(ktxTexture2& texture, const char* debugName = "Texture", ImageCreateInfo imageCreateInfo = {});
    // Texture-streaming path: uploads only the mip subrange
    // [firstMip, firstMip + mipCount) of an already-transcoded KTX2 object.
    // The VkImage is allocated at the subrange resolution (base extent >>
    // firstMip) with mipCount levels and mip indices remapped so level 0 of
    // the image is full-chain mip firstMip. Normalized-UV sampling is
    // unaffected; the image simply carries less detail. firstMip +
    // mipCount must be <= texture.numLevels after transcoding.
    Holder<TextureHandle> CreateStreamingTexture(ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount,
                                                 const char* debugName = "Texture", ImageCreateInfo imageCreateInfo = {});
    // In-place residency change for a streamed texture: reallocates the
    // backing VkImage for the new subrange and re-uploads from `texture`
    // (same transcode requirement as CreateStreamingTexture). Handles stay
    // stable – the same TextureHandle, ImageViewHandle and bindless
    // descriptor slot keep working, so baked MaterialValues indices are
    // untouched. Old backing memory retires through the deferred-destroy
    // queue (safe while frames are in flight); the caller must still
    // FlushUploads() afterwards to kick the re-upload. A Basis-family
    // payload is transcoded IN PLACE here (PrepareKtxStreamPlan, like the
    // first load — a render-thread hitch); the TextureStreamer fronts
    // this with its cooked/authoring policy (cooked runs refuse Basis
    // payloads up front with the pink error texture; the editor allows
    // the transcode).
    void RetargetStreamingTexture(TextureHandle handle, ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount,
                                  const char* debugName = "Texture");
    Holder<BindlessImageHandle> CreateBindlessImage(ImageViewHandle viewHandle, ImageLayout readLayout);
    Holder<ImageViewHandle> CreateImageView(ImageHandle image, const ImageViewCreateInfo& ci);
    // Recreate helpers (kept for swapchain)
    void SetImageViewVkView(ImageViewHandle handle, void* vkViewOpaque);
    ImageViewHandle RecreateImageView(ImageViewHandle handle, ImageHandle image, const ImageViewCreateInfo& ci);
    ImageHandle RecreateImage(ImageHandle handle, const ImageCreateInfo& ci, const void* data = nullptr);

    Holder<BufferHandle> CreateBuffer(size_t size, BufferType bufferType, const char* debugName = "Buffer");
    Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code, const char* debugName = "ShaderModule");
    Holder<DescriptorPoolHandle> CreateDescriptorPool(const std::vector<std::pair<uint32_t,uint32_t>>& poolSizes, const char* debugName = "DescriptorPool");

    // Content refresh of a texture by RECREATION: creates a fresh texture
    // from (ci, data) and reseats `holder` — releasing the old record, so
    // its pool generation BUMPS (anything still holding the old handle or
    // a captured bindless index is detectably dead instead of silently
    // reading a recycled slot; in-flight frames stay safe through the
    // deferred-destroy queue, exactly like every other texture retire).
    // The bindless index is NOT stable by design — the fresh one returns
    // here and callers re-read it per use (e.g. per-frame frame-uniform
    // fills). Returns UINT32_MAX on failure (the holder is left empty).
    uint32_t UpdateTexture(Holder<TextureHandle>& holder,
                           const ImageCreateInfo& ci, const void* data);

    // SPIR-V readback (CPU-only, headless-safe). Retained bytes per module
    // feed ReflectSpirv*; program helper merges push ranges across stages.
    std::span<const char> GetShaderModuleCode(ShaderModuleHandle handle) const;
    SpirvReflection ReflectShaderModule(ShaderModuleHandle handle) const;
    std::vector<PushConstantRange> ReflectProgramPushConstants(const ShaderProgram& program) const;

    void* MapBuffer(BufferHandle handle);
    void UnmapBuffer(BufferHandle handle);

    // RHI barrier enums (no Vk)
    void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize,
                            PipelineStage dstStages, AccessFlags dstAccess,
                            size_t src_offset = 0, size_t dest_offset = 0);
    // Bulk table-driven staged copy (the load-time batched counterpart of
    // UploadDataToBuffer): ONE content blob + a table of {srcOffset,
    // dstOffset, size} entries (BufferBulkCopy, smvk.h). The backend stages
    // 512 MB blobs — one memcpy pass, one command buffer, one submit and one
    // barrier set per blob — instead of one ring allocation + barrier per
    // entry. Bytes are staged during the call, so `content` only has to stay
    // valid until it returns. The table itself is caller-trusted (bounds
    // cannot be checked against an opaque blob). Returns false only for an
    // invalid dst/content.
    bool BulkCopyBuffer(BufferHandle dst, const void* content,
                        std::span<const BufferBulkCopy> copies,
                        PipelineStage dstStages, AccessFlags dstAccess);
    // Debug readback: GPU buffer -> CPU memory (staging copy + fence wait;
    // stalls, debug-only). Call only when the device is idle-ish (e.g.
    // after WaitForDeviceIdle) or accept one-frame staleness.
    bool DownloadDataFromBuffer(BufferHandle src, void* out, size_t size,
                                size_t srcOffset = 0);
    // Batched readback: N buffer copies through the shared persistent
    // staging with ONE submit + ONE wait (instead of one per copy).
    // Copies land back-to-back in staging; each `out` gets its own bytes.
    // All-or-nothing: returns false without copying anything when any
    // entry is invalid or the staging cannot cover the total.
    bool DownloadDataFromBuffers(std::span<const BufferDownload> downloads);
    // ASYNC batched readback (no CPU wait anywhere): submit records the
    // copies and returns a ticket (timeline value); poll the ticket each
    // frame — when the GPU signals it, `downloads` receive the bytes
    // (same span layout as the submit). One ticket in flight; a submit
    // while busy returns 0 (skip + retry next tick, no backlog). Built
    // for the periodic exposure-histogram / texture-feedback consume,
    // which previously locked the render thread on the graphics-queue
    // backlog with a blocking wait.
    uint64_t SubmitAsyncDownloadDataFromBuffers(std::span<const BufferDownload> downloads);
    bool PollAsyncDownloadDataFromBuffers(uint64_t ticket, std::span<const BufferDownload> downloads);
    bool HasAsyncDownloadInFlight() const;
    // Debug readback: GPU color image (layer 0, mip 0, tightly packed) ->
    // CPU memory. currentLayout must be the image's ACTUAL layout (the
    // rendergraph does not write sync states back; a guessed Undefined
    // would discard the contents on transition). The copy transitions to
    // TRANSFER_SRC and back, so the layout is preserved. out must hold
    // width * height * bytes-per-texel bytes.
    bool DownloadDataFromImage(ImageHandle src, ImageLayout currentLayout,
                               void* out, size_t size);
    void UploadDataToImage2D(const struct Image& image, const void* data,
                             PipelineStage dstStages, AccessFlags dstAccess, size_t src_offset);
    // BATCHED partial upload into a persistent 3D image — the world-field
    // atlas pager's frame form (Engine/WorldFields): ONE packed blob +
    // a {srcOffset, region} table (ImageRegionCopy3D, smvk.h — the
    // BulkCopyBuffer pattern, image edition). One ring allocation, one
    // memcpy, ONE layout-transition pair, one copy command with all
    // regions (vs N single uploads = N barrier pairs). Block alignment +
    // bounds + per-entry payload size are asserted inside. PUBLICATION
    // CONTRACT (both region forms): the copies are recorded into the
    // calling thread's upload-streamer shard and NOT flushed per call —
    // they flush at present time (or the 256MB threshold), and every
    // transfer batch executes only after ALL previously submitted
    // graphics frames have fully retired (submitTransfer waits the
    // latest graphics retirement at ALL_COMMANDS), while the frame's own
    // main submit waits the transfer chain. Net effect: same-frame
    // render passes read the new contents, and overwriting a slot can
    // never race a previous frame's samples of its old contents. Call
    // between frames on the render thread (the v1 world-field
    // discipline); a cross-thread publisher needs the completion-ticket
    // work first.
    void UploadDataToImage3DRegions(ImageHandle image,
                                   const ImageRegionCopy3D* copies, size_t copyCount,
                                   const void* blob, size_t blobBytes,
                                   PipelineStage dstStages, AccessFlags dstAccess);
    // The 1-entry convenience of the batch above (single region; `data`
    // is the region's tight payload).
    void UploadDataToImage3DRegion(ImageHandle image, const ImageRegion3D& region,
                                   const void* data, size_t dataSize,
                                   PipelineStage dstStages, AccessFlags dstAccess);

    void UpdateDescriptors(uint32_t frameIndex = 0);
    uint32_t GetBindlessIndex(BindlessImageHandle handle) const;
    // Bindless descriptor-array slot of a TextureHandle (its sampled view).
    uint32_t GetTextureBindlessIndex(TextureHandle handle) const;
    // Live-texture listing for debug UIs (the texture inspector): one row
    // per live texture, pool order, NO content filter. RENDER THREAD ONLY —
    // the pool walk is unsynchronized (the ShardedPool forEach contract).
    std::vector<TextureInfo> ListTextureInfos();
    // Cubemap introspection (backing image + sampled view). Used by the
    // cubemap renderer loader to validate KTX2 cubemap uploads.
    bool IsCubemapTexture(TextureHandle handle) const;
    uint32_t GetTextureArrayLayers(TextureHandle handle) const;
    uint32_t GetTextureCubemapSize(TextureHandle handle) const;
    // The texture's underlying image handle — standalone view creation on
    // it (e.g. per-face 2D views of a cubemap for the texture inspector).
    // Empty handle on a dead texture (generation bumped).
    ImageHandle GetTextureImageHandle(TextureHandle handle) const;
    // ---- Target heap (design-A interval-aliased target memory) ----
    // Facade forwarders; the full contracts live on the Vulkan impl
    // (vulkan_impl.h - see VulkanResourceManager's block) and the
    // canonical phase order on TargetPhase (smvk.h):
    //   CreateHeapImage: unbound VK_IMAGE_CREATE_ALIAS_BIT image (no
    //     VmaAllocation) for a later packed-range bind.
    //   ClaimTargetHeapRange: claim + bind a packed range by the
    //     declared [firstPhase, lastPhase] use window; same-shape
    //     disjoint-window targets share ranges. The returned Holder
    //     AUTO-RELEASES on destruction (see destroy(VulkanResourceManager*,
    //     TargetHeapClaimHandle)) - in target structs declare it BEFORE
    //     the image Holder so destruction releases the claim BEHIND the
    //     image's deferred destroy (the range free must land behind the
    //     destroy in the deferred FIFO).
    //   ReleaseTargetHeapClaim: the LAST user's release frees the range
    //     retirement-ordered. No-op on an empty/dead claim handle.
    [[nodiscard]] Holder<ImageHandle> CreateHeapImage(
        const ImageCreateInfo& ci);
    [[nodiscard]] Holder<TargetHeapClaimHandle> ClaimTargetHeapRange(
        ImageHandle image, TargetPhase firstPhase, TargetPhase lastPhase);
    void ReleaseTargetHeapClaim(TargetHeapClaimHandle claim);
    uint32_t GetTextureMipLevels(TextureHandle handle) const;
    // Raw Vulkan interop (opaque native handles for external UI/render
    // layers, e.g. ImGui user textures via ImGui_ImplVulkan_AddTexture):
    // the texture's SAMPLED VkImageView + a cached sampler's VkSampler.
    // nullptr on an invalid handle/index (view/sampler indices are the
    // GetOrCreateSampler cache slots). The view variant reaches a
    // STANDALONE caller-created view (e.g. a per-face 2D view of a
    // cubemap) - the texture variant only reaches the texture's own
    // default view (cube/array/3D - unusable as a 2D sample).
    const void* GetTextureVkImageView(TextureHandle handle) const;
    const void* GetImageViewVkImageView(ImageViewHandle handle) const;
    const void* GetVkSamplerOpaque(uint32_t samplerIndex) const;
    uint64_t GetBufferDeviceAddress(BufferHandle handle) const;
    // Flush batched uploads (256MB threshold, or manual at end of scene)
    void FlushUploads();
    // Flush + block until every upload batch (transfer + graphics acquire)
    // has retired. Required before destroying resources that may still be
    // referenced by unflushed streamer batches (e.g. a failed pak import
    // that drops its partial cluster).
    void WaitForUploads();
    // Sampler cache – deduplicates by description, returns stable
    // index into the global sampler descriptor array
    // (0 .. smvk::limits::MaxSamplers-1).
    // Mirrors VulkanPipelineCache: same SamplerCreateInfo → same index.
    uint32_t GetOrCreateSampler(const SamplerCreateInfo& ci);
    uint32_t GetSamplerCount() const;
    // In-place sampler replacement at a stable bindless slot (see
    // VulkanResourceManager::RecreateSampler). The renderer uses this for
    // the trilinear/aniso filtering toggle (slots 0 + 2).
    bool RecreateSampler(uint32_t index, const SamplerCreateInfo& ci);
    // True when the physical device supports 2x MSAA for both color and
    // depth framebuffer attachments (used for the Main pass resolve path).
    bool SupportsMSAA2x();
    // True when BC6H-U is usable as a sampled, linearly filtered ImageType
    // 3D with transfer-dst uploads — the dense GI layers configuration.
    // Block-compressed 3D support is NOT implied by 2D (format/filter
    // capabilities are queried per image type), so the dense-GI path gates
    // on this instead of assuming.
    bool SupportsBc6h3DSampling();
    // True when RG32F supports the full EVSM moment chain: 2x MSAA color
    // attachment + resolve, sampled + storage (async blur). When false the
    // app keeps the PCF depth path (EVSM silently unavailable).
    bool SupportsEvsm();
    // True when the device exposes a dedicated compute queue distinct from
    // graphics (EVSM blur overlaps shadow rendering; otherwise emulated).
    bool HasDedicatedComputeQueue();
    // Queue-family indices (graphics always valid; compute falls back to
    // graphics when no dedicated family exists).
    uint32_t GetComputeQueueFamily();
    uint32_t GetGraphicsQueueFamily();
    // NDC orientation: TRUE when the backend's normalized device Y axis
    // points DOWN the framebuffer (Vulkan), FALSE when it points UP
    // (D3D12, the GL convention). The engine's frame-uniform build uses
    // this to decide the projection Y-flip (proj[1][1] *= -1 — the
    // UpdateFrameUniforms compensation): apply it ONLY when this returns
    // true. Applying the Vulkan flip under a Y-up backend renders the
    // scene upside down (hit on the D3D12 port; the winding flips with it
    // too — the mirror inverts screen-space triangle orientation).
    bool NeedsProjectionYFlip() const;
    // GPU memory snapshot: one entry per Vk memory heap (VMA budgets +
    // heap sizes). Cheap (VMA throttles OS queries internally); safe to
    // call once per frame for the debug UI / console stats.
    std::vector<GpuMemoryHeap> GetGpuMemoryInfo() const;

    // Allocation ledger (see the smvk.h block): EXACT per-category
    // committed-byte sums + per-item rows. Immediate-mode pool walks
    // (render thread, the ListTextureInfos contract); zero cost when not
    // called — the bytes live on the pool entries, captured at create.
    std::array<uint64_t, kMemoryCategoryCount> GetMemoryLedger();
    std::vector<MemoryAllocInfo> GetMemoryAllocInfos();

    // Internal helpers for PIMPL firewall – used by Swapchain/RenderGraph private TUs
    struct Impl;
    Impl* GetImpl() { return impl_.get(); }
    const Impl* GetImpl() const { return impl_.get(); }
    void* GetVulkanOpaque();
    const void* GetVulkanOpaque() const;

    // Deferred task support
    void AddDeferredTask(std::packaged_task<void()>&& task, uint32_t handleIndex = 0, uint32_t handleGen = 0);
    void ProcessDeferredTasks();
    void ProcessDeferrredTasks() { ProcessDeferredTasks(); }

    // Retire-ordered destruction: queues the task until ALL graphics work
    // submitted so far has retired (backend-defined — the Vulkan timeline
    // point key; a frame-wait fence on D3D12), running it at the next
    // ProcessDeferredTasks after that point. Runs immediately when no
    // graphics submit was ever recorded (GPU idle by definition). Render
    // thread only, between frames — the suballocator range-free discipline
    // (see Cluster's heap ranges / the static vertex heap).
    void DeferDestroyTask(std::packaged_task<void()>&& task);

    // For Holder destroy – expose internal pools via Impl
    void DestroyImage(ImageHandle h);
    void DestroyImageView(ImageViewHandle h);
    void DestroyBuffer(BufferHandle h);
    void DestroyTexture(TextureHandle h);
    void DestroyBindlessImage(BindlessImageHandle h);
    void DestroySampler(SamplerHandle h);
    void DestroyDescriptorPool(DescriptorPoolHandle h);
    void DestroyShaderModule(ShaderModuleHandle h);

private:
    std::unique_ptr<Impl> impl_;
};

// Free destroy overloads – called by Holder
inline void destroy(ResourceManager* ctx, ImageHandle handle) { if(ctx && !handle.empty()) ctx->DestroyImage(handle); }
inline void destroy(ResourceManager* ctx, ImageViewHandle handle) { if(ctx && !handle.empty()) ctx->DestroyImageView(handle); }
inline void destroy(ResourceManager* ctx, BufferHandle handle) { if(ctx && !handle.empty()) ctx->DestroyBuffer(handle); }
inline void destroy(ResourceManager* ctx, TextureHandle handle) { if(ctx && !handle.empty()) ctx->DestroyTexture(handle); }
inline void destroy(ResourceManager* ctx, BindlessImageHandle handle) { if(ctx && !handle.empty()) ctx->DestroyBindlessImage(handle); }
inline void destroy(ResourceManager* ctx, SamplerHandle) {}
inline void destroy(ResourceManager* ctx, DescriptorPoolHandle handle) { if(ctx && !handle.empty()) ctx->DestroyDescriptorPool(handle); }
inline void destroy(ResourceManager* ctx, ShaderModuleHandle handle) { if(ctx && !handle.empty()) ctx->DestroyShaderModule(handle); }

} // namespace smvk
