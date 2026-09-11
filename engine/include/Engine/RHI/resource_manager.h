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

class ResourceManager {
public:
    // Creates Vulkan core + allocator. window may be nullptr for headless.
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
    // FlushUploads() afterwards to kick the re-upload.
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
    // Texture-streaming feedback ring (Skull & Bones style GPU readback):
    // per-flight-slice device buffer, one uint per bindless slot.
    BufferHandle GetTextureFeedbackBuffer(uint32_t flightSlot);
    // Rewrites the set-0 feedback binding to the given flight slice. Call
    // once per record before passes execute.
    void BindTextureFeedbackSlice(uint32_t flightSlot);
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

    void UpdateDescriptors(uint32_t frameIndex = 0);
    uint32_t GetBindlessIndex(BindlessImageHandle handle) const;
    // Bindless descriptor-array slot of a TextureHandle (its sampled view).
    uint32_t GetTextureBindlessIndex(TextureHandle handle) const;
    uint64_t GetBufferDeviceAddress(BufferHandle handle) const;
    // Flush batched uploads (256MB threshold, or manual at end of scene)
    void FlushUploads();
    // Sampler cache – deduplicates by description, returns stable
    // index into the global sampler descriptor array (0 .. MAX_SAMPLERS-1).
    // Mirrors VulkanPipelineCache: same SamplerCreateInfo → same index.
    uint32_t GetOrCreateSampler(const SamplerCreateInfo& ci);
    uint32_t GetSamplerCount() const;
    // True when the physical device supports 2x MSAA for both color and
    // depth framebuffer attachments (used for the Main pass resolve path).
    bool SupportsMSAA2x();
    // GPU memory snapshot: one entry per Vk memory heap (VMA budgets +
    // heap sizes). Cheap (VMA throttles OS queries internally); safe to
    // call once per frame for the debug UI / console stats.
    std::vector<GpuMemoryHeap> GetGpuMemoryInfo() const;

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
