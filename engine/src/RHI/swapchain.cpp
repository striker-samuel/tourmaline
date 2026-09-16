#include <Engine/RHI/swapchain.h>
#include "../Vulkan/detail/vulkan_impl.h"
#include <Engine/RHI/resource_manager.h>

namespace smvk {

namespace {
VkPresentModeKHR ToVkPresentMode(PresentMode mode) {
    switch (mode) {
    case PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
    case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case PresentMode::FifoRelaxed: return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    case PresentMode::Fifo:
    default: return VK_PRESENT_MODE_FIFO_KHR;
    }
}
PresentMode FromVkPresentMode(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return PresentMode::Mailbox;
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return PresentMode::Immediate;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return PresentMode::FifoRelaxed;
    case VK_PRESENT_MODE_FIFO_KHR:
    default: return PresentMode::Fifo;
    }
}
} // namespace

struct Swapchain::Impl {
    std::unique_ptr<VulkanSwapchain> vulkan;
};

Swapchain::Swapchain(ResourceManager& ctx, SDL_Window* window) : impl_(std::make_unique<Impl>()) {
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
    fb.splitCommandBufferOpaque = nullptr;
    fb.splitWaitSemaphoreOpaque = nullptr;
    fb.splitWaitValue = 0;
    fb.splitWaitSemaphore2Opaque = nullptr;
    fb.splitWaitValue2 = 0;
    return fb;
}
void Swapchain::EndAndPresentFramebuffer(Framebuffer& fb) {
    VulkanFrameBuffer vfb{fb.frameIndex, fb.imageIndex, reinterpret_cast<VkCommandBuffer>(fb.commandBufferOpaque), *impl_->vulkan,
        reinterpret_cast<VkCommandBuffer>(fb.splitCommandBufferOpaque),
        reinterpret_cast<VkSemaphore>(fb.splitWaitSemaphoreOpaque), fb.splitWaitValue,
        reinterpret_cast<VkSemaphore>(fb.splitWaitSemaphore2Opaque), fb.splitWaitValue2};
    impl_->vulkan->EndAndPresentFramebuffer(vfb);
}
void Swapchain::SetPresentMode(PresentMode mode) {
    impl_->vulkan->SetRequestedPresentMode(ToVkPresentMode(mode));
}
PresentMode Swapchain::GetPresentMode() const {
    return FromVkPresentMode(impl_->vulkan->GetRequestedPresentMode());
}
PresentMode Swapchain::GetActivePresentMode() const {
    return FromVkPresentMode(impl_->vulkan->GetActivePresentMode());
}
std::vector<PresentMode> Swapchain::GetAvailablePresentModes() const {
    std::vector<PresentMode> out;
    for (VkPresentModeKHR m : impl_->vulkan->GetAvailablePresentModes()) {
        // Only surface the modes the overlay offers; others (e.g.
        // FIFO_RELAXED) still work as automatic fallbacks.
        if (m == VK_PRESENT_MODE_FIFO_KHR || m == VK_PRESENT_MODE_MAILBOX_KHR ||
            m == VK_PRESENT_MODE_IMMEDIATE_KHR || m == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
            out.push_back(FromVkPresentMode(m));
    }
    return out;
}
double Swapchain::GetGpuFrameMs() const { return impl_->vulkan->GetLastGpuMs(); }
bool Swapchain::HasGpuTiming() const { return impl_->vulkan->HasGpuTiming(); }
double Swapchain::GetPresentWaitMs() const { return impl_->vulkan->GetLastPresentWaitMs(); }
Swapchain::Impl* Swapchain::GetImpl() { return impl_.get(); }
const Swapchain::Impl* Swapchain::GetImpl() const { return impl_.get(); }
void* Swapchain::GetVulkanOpaque() { return impl_->vulkan.get(); }
const void* Swapchain::GetVulkanOpaque() const { return impl_->vulkan.get(); }

} // namespace smvk
