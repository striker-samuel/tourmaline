#include <Engine/RHI/swapchain.h>
#include "../Vulkan/detail/vulkan_impl.h"
#include <Engine/RHI/resource_manager.h>

namespace smvk {
struct Swapchain::Impl {
    std::unique_ptr<VulkanSwapchain> vulkan;
};

Swapchain::Swapchain(ResourceManager& ctx, ::GLFWwindow* window) : impl_(std::make_unique<Impl>()) {
    auto* vm = static_cast<VulkanResourceManager*>(ctx.GetVulkanOpaque());
    impl_->vulkan = std::make_unique<VulkanSwapchain>(*vm, window);
}
Swapchain::~Swapchain() = default;
void Swapchain::Create() { impl_->vulkan->Create(); }
void Swapchain::Recreate() { impl_->vulkan->Recreate(); }
Extent2D Swapchain::GetExtent() const {
    VkExtent2D e = impl_->vulkan->GetExtent();
    return {e.width, e.height};
}
uint32_t Swapchain::GetImageCount() const { return impl_->vulkan->GetImageCount(); }
uint32_t Swapchain::GetMinImageCount() const { return impl_->vulkan->GetMinImageCount(); }
ImageViewHandle Swapchain::GetImageViewHandle(uint32_t i) const { return impl_->vulkan->GetImageViewHandle(i); }
ImageViewHandle Swapchain::GetDepthImageViewHandle() const {
    // depthImageView_ is Holder<ImageViewHandle>
    return impl_->vulkan->depthImageView_;
}
Format Swapchain::GetSurfaceFormat() const { return impl_->vulkan->surfaceFormat; }
Format Swapchain::GetDepthFormat() const { return impl_->vulkan->depthFormat; }
bool Swapchain::FramebufferResized() const { return impl_->vulkan->FramebufferResized(); }
void Swapchain::ClearFramebufferResized() { impl_->vulkan->ClearFramebufferResized(); }
void Swapchain::SetFramebufferResized(bool v) { impl_->vulkan->framebufferResized = v; }
uint32_t Swapchain::GetCurrentImageIndex() const { return impl_->vulkan->GetCurrentImageIndex(); }
uint32_t Swapchain::GetCurrentFrameIndex() const { return impl_->vulkan->GetCurrentFrameIndex(); }
uint32_t Swapchain::framesInFlight() const { return impl_->vulkan->framesInFlight_; }
int Swapchain::GetWindowWidth() const { return impl_->vulkan->windowWidth; }
int Swapchain::GetWindowHeight() const { return impl_->vulkan->windowHeight; }

Framebuffer Swapchain::AcquireFramebuffer() {
    VulkanFrameBuffer vfb = impl_->vulkan->AcquireFramebuffer();
    Framebuffer fb;
    fb.frameIndex = vfb.frameIndex;
    fb.imageIndex = vfb.imageIndex;
    fb.commandBufferOpaque = reinterpret_cast<void*>(vfb.commandBuffer);
    fb.swapchain = this;
    return fb;
}
void Swapchain::EndAndPresentFramebuffer(Framebuffer& fb) {
    VulkanFrameBuffer vfb{fb.frameIndex, fb.imageIndex, reinterpret_cast<VkCommandBuffer>(fb.commandBufferOpaque), *impl_->vulkan};
    impl_->vulkan->EndAndPresentFramebuffer(vfb);
}
Swapchain::Impl* Swapchain::GetImpl() { return impl_.get(); }
const Swapchain::Impl* Swapchain::GetImpl() const { return impl_.get(); }

} // namespace smvk
