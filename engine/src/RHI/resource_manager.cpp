#include <Engine/RHI/resource_manager.h>
#include <Engine/smvk.h>
#include <Engine/Renderer/spirv_reflection.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "volk.h"
#include <SDL3/SDL_video.h>
#include "../Vulkan/detail/vulkan_impl.h"
#include <ktx.h>

namespace smvk {

struct ResourceManager::Impl {
    VulkanResourceManager vulkan;
    Impl(SDL_Window* w) : vulkan({}, w) {}
    VulkanResourceManager* getVulkan() { return &vulkan; }
};

// Helpers to translate RHI -> Vk
static VkImageLayout toVkImageLayout(ImageLayout layout) {
    switch(layout) {
        case ImageLayout::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
        case ImageLayout::General: return VK_IMAGE_LAYOUT_GENERAL;
        case ImageLayout::ColorAttachmentOptimal: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ImageLayout::DepthStencilAttachmentOptimal: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case ImageLayout::DepthStencilReadOnlyOptimal: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ImageLayout::DepthReadOnlyOptimal: return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        case ImageLayout::ShaderReadOnlyOptimal: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ImageLayout::TransferSrcOptimal: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ImageLayout::TransferDstOptimal: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ImageLayout::PresentSrc: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        default: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}
static VkPipelineStageFlags2 toVkPipelineStage(PipelineStage stage) {
    if (stage == PipelineStageBit_None) return VK_PIPELINE_STAGE_2_NONE;
    VkPipelineStageFlags2 out = 0;
    // Vertex covers vertex-input fetch as well as the shader stage (must
    // match impl.cpp ToVkStage): INDEX_READ is only valid with
    // INDEX_INPUT/VERTEX_INPUT/ALL_GRAPHICS/ALL_COMMANDS in the mask
    // (VUID-VkBufferMemoryBarrier2-dstAccessMask-03901), and the upload
    // path emits e.g. IndexRead under the Vertex bit.
    if (stage & PipelineStageBit_Vertex) out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    if (stage & PipelineStageBit_Fragment) out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (stage & PipelineStageBit_Compute) out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (stage & PipelineStageBit_Geometry) out |= VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
    if (stage & PipelineStageBit_TessellationControl) out |= VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
    if (stage & PipelineStageBit_TessellationEvaluation) out |= VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
    if (stage & PipelineStageBit_Task) out |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
    if (stage & PipelineStageBit_Mesh) out |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    if (stage & PipelineStageBit_RayGeneration) out |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    // AnyHit etc map to same ray tracing bit for simplicity
    if (stage & (PipelineStageBit_AnyHit | PipelineStageBit_ClosestHit | PipelineStageBit_Miss | PipelineStageBit_Intersection | PipelineStageBit_Callable))
        out |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    if (stage & PipelineStageBit_Transfer) out |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (stage & PipelineStageBit_DrawIndirect) out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    return out ? out : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}
static VkAccessFlags2 toVkAccess(AccessFlags access) {
    return static_cast<VkAccessFlags2>(static_cast<uint32_t>(access));
}

ResourceManager::ResourceManager(SDL_Window* window) : impl_(std::make_unique<Impl>(window)) {}
ResourceManager::~ResourceManager() = default;
ResourceManager::ResourceManager(ResourceManager&&) noexcept = default;
ResourceManager& ResourceManager::operator=(ResourceManager&&) noexcept = default;

Holder<ImageHandle> ResourceManager::CreateImage(const ImageCreateInfo& ci, const void* data) {
    auto h = impl_->vulkan.CreateImage(ci, data);
    ImageHandle raw = h.release();
    return Holder<ImageHandle>{this, raw};
}
Holder<TextureHandle> ResourceManager::CreateTexture(const ImageCreateInfo& ci, const void* data) {
    auto h = impl_->vulkan.CreateTexture(ci, data);
    TextureHandle raw = h.release();
    return Holder<TextureHandle>{this, raw};
}
Holder<TextureHandle> ResourceManager::CreateTexture(ktxTexture2& texture, const char* debugName, ImageCreateInfo imageCreateInfo) {
    auto h = impl_->vulkan.CreateTexture(texture, debugName, imageCreateInfo);
    TextureHandle raw = h.release();
    return Holder<TextureHandle>{this, raw};
}
Holder<TextureHandle> ResourceManager::CreateStreamingTexture(ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount,
                                                              const char* debugName, ImageCreateInfo imageCreateInfo) {
    auto h = impl_->vulkan.CreateStreamingTexture(texture, firstMip, mipCount, debugName, imageCreateInfo);
    TextureHandle raw = h.release();
    return Holder<TextureHandle>{this, raw};
}
void ResourceManager::RetargetStreamingTexture(TextureHandle handle, ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount,
                                               const char* debugName) {
    impl_->vulkan.RetargetStreamingTexture(handle, texture, firstMip, mipCount, debugName);
}
Holder<BindlessImageHandle> ResourceManager::CreateBindlessImage(ImageViewHandle viewHandle, ImageLayout readLayout) {
    VkImageLayout vkLayout = toVkImageLayout(readLayout);
    auto h = impl_->vulkan.CreateBindlessImage(viewHandle, vkLayout);
    BindlessImageHandle raw = h.release();
    return Holder<BindlessImageHandle>{this, raw};
}
Holder<ImageViewHandle> ResourceManager::CreateImageView(ImageHandle image, const ImageViewCreateInfo& ci) {
    auto h = impl_->vulkan.CreateImageView(image, ci);
    ImageViewHandle raw = h.release();
    return Holder<ImageViewHandle>{this, raw};
}
void ResourceManager::SetImageViewVkView(ImageViewHandle handle, void* vkViewOpaque) {
    VkImageView v = reinterpret_cast<VkImageView>(vkViewOpaque);
    impl_->vulkan.SetImageViewVkView(handle, v);
}
ImageViewHandle ResourceManager::RecreateImageView(ImageViewHandle handle, ImageHandle image, const ImageViewCreateInfo& ci) {
    return impl_->vulkan.RecreateImageView(handle, image, ci);
}
ImageHandle ResourceManager::RecreateImage(ImageHandle handle, const ImageCreateInfo& ci, const void* data) {
    return impl_->vulkan.RecreateImage(handle, ci, data);
}
Holder<BufferHandle> ResourceManager::CreateBuffer(size_t size, BufferType bufferType, const char* debugName) {
    auto h = impl_->vulkan.CreateBuffer(static_cast<VkDeviceSize>(size), bufferType, debugName);
    BufferHandle raw = h.release();
    return Holder<BufferHandle>{this, raw};
}
Holder<ShaderModuleHandle> ResourceManager::CreateShaderModule(std::vector<char> code, const char* debugName) {
    auto h = impl_->vulkan.CreateShaderModule(std::move(code), debugName);
    ShaderModuleHandle raw = h.release();
    return Holder<ShaderModuleHandle>{this, raw};
}
std::span<const char> ResourceManager::GetShaderModuleCode(ShaderModuleHandle handle) const {
    return impl_->vulkan.GetShaderModuleCode(handle);
}
SpirvReflection ResourceManager::ReflectShaderModule(ShaderModuleHandle handle) const {
    return impl_->vulkan.ReflectShaderModule(handle);
}
std::vector<PushConstantRange> ResourceManager::ReflectProgramPushConstants(const ShaderProgram& program) const {
    std::vector<std::vector<PushConstantRange>> perModule;
    perModule.reserve(program.shaderCount);
    for (size_t i = 0; i < program.shaderCount; ++i) {
        auto refl = impl_->vulkan.ReflectShaderModule(program.shaders[i].module);
        if (refl.valid && !refl.pushConstants.empty())
            perModule.push_back(std::move(refl.pushConstants));
    }
    return MergePushConstantRanges(perModule);
}
Holder<DescriptorPoolHandle> ResourceManager::CreateDescriptorPool(const std::vector<std::pair<uint32_t,uint32_t>>& poolSizes, const char* debugName) {
    std::vector<VkDescriptorPoolSize> vkSizes;
    vkSizes.reserve(poolSizes.size());
    for (auto &p: poolSizes) vkSizes.push_back({static_cast<VkDescriptorType>(p.first), p.second});
    auto h = impl_->vulkan.CreateDescriptorPool(vkSizes, debugName);
    DescriptorPoolHandle raw = h.release();
    return Holder<DescriptorPoolHandle>{this, raw};
}
void* ResourceManager::MapBuffer(BufferHandle handle) { return impl_->vulkan.MapBuffer(handle); }
void ResourceManager::UnmapBuffer(BufferHandle handle) { impl_->vulkan.UnmapBuffer(handle); }
void ResourceManager::UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize,
                            PipelineStage dstStages, AccessFlags dstAccess,
                            size_t src_offset, size_t dest_offset) {
    VkPipelineStageFlags2 vkStage = toVkPipelineStage(dstStages);
    VkAccessFlags2 vkAccess = toVkAccess(dstAccess);
    impl_->vulkan.UploadDataToBuffer(buffer, data, dataSize, vkStage, vkAccess, src_offset, dest_offset);
}
bool ResourceManager::DownloadDataFromBuffer(BufferHandle src, void* out, size_t size,
                                               size_t srcOffset) {
    return impl_->vulkan.DownloadDataFromBuffer(src, out, size, srcOffset);
}
bool ResourceManager::DownloadDataFromBuffers(std::span<const BufferDownload> downloads) {
    return impl_->vulkan.DownloadDataFromBuffers(downloads);
}
uint64_t ResourceManager::SubmitAsyncDownloadDataFromBuffers(std::span<const BufferDownload> downloads) {
    return impl_->vulkan.SubmitAsyncDownloadDataFromBuffers(downloads);
}
bool ResourceManager::PollAsyncDownloadDataFromBuffers(uint64_t ticket, std::span<const BufferDownload> downloads) {
    return impl_->vulkan.PollAsyncDownloadDataFromBuffers(ticket, downloads);
}
bool ResourceManager::HasAsyncDownloadInFlight() const {
    return impl_->vulkan.HasAsyncDownloadInFlight();
}
BufferHandle ResourceManager::GetTextureFeedbackBuffer(uint32_t flightSlot) {
    return impl_->vulkan.GetTextureFeedbackBuffer(flightSlot);
}
void ResourceManager::BindTextureFeedbackSlice(uint32_t flightSlot) {
    impl_->vulkan.FlushTextureFeedback(flightSlot);
}
bool ResourceManager::DownloadDataFromImage(ImageHandle src, ImageLayout currentLayout, void* out, size_t size) {
    return impl_->vulkan.DownloadDataFromImage(src, currentLayout, out, size);
}
void ResourceManager::UploadDataToImage2D(const Image& image, const void* data,
                             PipelineStage dstStages, AccessFlags dstAccess, size_t src_offset) {
    VkPipelineStageFlags2 vkStage = toVkPipelineStage(dstStages);
    VkAccessFlags2 vkAccess = toVkAccess(dstAccess);
    impl_->vulkan.UploadDataToImage2D(image, data, vkStage, vkAccess, src_offset);
}
void ResourceManager::UpdateDescriptors(uint32_t frameIndex) { impl_->vulkan.UpdateDescriptors(frameIndex); }
uint32_t ResourceManager::GetBindlessIndex(BindlessImageHandle handle) const { return impl_->vulkan.GetBindlessIndex(handle); }
uint32_t ResourceManager::GetTextureBindlessIndex(TextureHandle handle) const { return impl_->vulkan.GetTextureBindlessIndex(handle); }
uint32_t ResourceManager::GetOrCreateSampler(const SamplerCreateInfo& ci) { return impl_->vulkan.GetOrCreateSampler(ci); }
uint32_t ResourceManager::GetSamplerCount() const { return impl_->vulkan.GetSamplerCount(); }
std::vector<GpuMemoryHeap> ResourceManager::GetGpuMemoryInfo() const {
    if (!impl_)
        return {};
    return impl_->vulkan.GetGpuMemoryInfo();
}
bool ResourceManager::SupportsMSAA2x() {
    if (!impl_ || !impl_->vulkan.core_)
        return false;
    const auto& props = impl_->vulkan.core_->GetPhysicalDeviceProperties().properties;
    constexpr VkSampleCountFlags want = VK_SAMPLE_COUNT_2_BIT;
    return (props.limits.framebufferColorSampleCounts & want) != 0 &&
        (props.limits.framebufferDepthSampleCounts & want) != 0;
}
void ResourceManager::FlushUploads() { impl_->vulkan.FlushUploads(); }
uint64_t ResourceManager::GetBufferDeviceAddress(BufferHandle handle) const {
    auto* buf = impl_->vulkan.buffers_.get(handle);
    if (!buf) return 0;
    return buf->deviceAddress;
}
void ResourceManager::AddDeferredTask(std::packaged_task<void()>&& task, uint32_t handleIndex, uint32_t handleGen) {
    CommandSubmitHandle h{nullptr, handleIndex, handleGen};
    impl_->vulkan.AddDeferredTask(std::move(task), h);
}
void ResourceManager::ProcessDeferredTasks() { impl_->vulkan.ProcessDeferrredTasks(); }

void ResourceManager::DestroyImage(ImageHandle h) { destroy(&impl_->vulkan, h); }
void ResourceManager::DestroyImageView(ImageViewHandle h) { destroy(&impl_->vulkan, h); }
void ResourceManager::DestroyBuffer(BufferHandle h) { destroy(&impl_->vulkan, h); }
void ResourceManager::DestroyTexture(TextureHandle h) { destroy(&impl_->vulkan, h); }
void ResourceManager::DestroyBindlessImage(BindlessImageHandle h) { destroy(&impl_->vulkan, h); }
void ResourceManager::DestroySampler(SamplerHandle h) { destroy(&impl_->vulkan, h); }
void ResourceManager::DestroyDescriptorPool(DescriptorPoolHandle h) { destroy(&impl_->vulkan, h); }
void ResourceManager::DestroyShaderModule(ShaderModuleHandle h) { destroy(&impl_->vulkan, h); }
void* ResourceManager::GetVulkanOpaque() { return &impl_->vulkan; }
const void* ResourceManager::GetVulkanOpaque() const { return &impl_->vulkan; }

} // namespace smvk
