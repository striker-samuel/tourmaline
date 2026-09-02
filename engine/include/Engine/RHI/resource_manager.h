#pragma once
#include <GLFW/glfw3.h>
#include <Engine/smvk.h>
#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>
#include <memory>
#include <vector>
#include <span>

struct ktxTexture2;

namespace smvk {

class ResourceManager {
public:
    // Creates Vulkan core + allocator. window may be nullptr for headless.
    explicit ResourceManager(::GLFWwindow* window = nullptr);
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;
    ResourceManager(ResourceManager&&) noexcept;
    ResourceManager& operator=(ResourceManager&&) noexcept;

    // Resource creation (RHI-only types, no Vk)
    Holder<ImageHandle> CreateImage(const ImageCreateInfo& ci, const void* data = nullptr);
    Holder<TextureHandle> CreateTexture(const ImageCreateInfo& ci, const void* data = nullptr);
     Holder<TextureHandle> CreateTexture(ktxTexture2& texture, const char* debugName = "Texture", ImageCreateInfo imageCreateInfo = {});
    Holder<BindlessImageHandle> CreateBindlessImage(ImageViewHandle viewHandle, ImageLayout readLayout);
    Holder<ImageViewHandle> CreateImageView(ImageHandle image, const ImageViewCreateInfo& ci);
    // Recreate helpers (kept for swapchain)
    void SetImageViewVkView(ImageViewHandle handle, void* vkViewOpaque);
    ImageViewHandle RecreateImageView(ImageViewHandle handle, ImageHandle image, const ImageViewCreateInfo& ci);
    ImageHandle RecreateImage(ImageHandle handle, const ImageCreateInfo& ci, const void* data = nullptr);

    Holder<BufferHandle> CreateBuffer(size_t size, BufferType bufferType, const char* debugName = "Buffer");
    Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code, const char* debugName = "ShaderModule");
    Holder<DescriptorPoolHandle> CreateDescriptorPool(const std::vector<std::pair<uint32_t,uint32_t>>& poolSizes, const char* debugName = "DescriptorPool");

    void* MapBuffer(BufferHandle handle);
    void UnmapBuffer(BufferHandle handle);

    // RHI barrier enums (no Vk)
    void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize,
                            PipelineStage dstStages, AccessFlags dstAccess,
                            size_t src_offset = 0, size_t dest_offset = 0);
    void UploadDataToImage2D(const struct Image& image, const void* data,
                             PipelineStage dstStages, AccessFlags dstAccess, size_t src_offset);

    void UpdateDescriptors(uint32_t frameIndex = 0);
    uint32_t GetBindlessIndex(BindlessImageHandle handle) const;
    uint64_t GetBufferDeviceAddress(BufferHandle handle) const;
    // Flush batched uploads (256MB threshold, or manual at end of scene)
    void FlushUploads();
    // Sampler cache – deduplicates by description, returns stable
    // index into the global sampler descriptor array (0 .. MAX_SAMPLERS-1).
    // Mirrors VulkanPipelineCache: same SamplerCreateInfo → same index.
    uint32_t GetOrCreateSampler(const SamplerCreateInfo& ci);
    uint32_t GetSamplerCount() const;

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
