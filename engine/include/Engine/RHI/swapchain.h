#pragma once
#include <GLFW/glfw3.h>
#include <Engine/smvk.h>
#include <Engine/RHI/resource_manager.h>
#include <memory>

namespace smvk {

struct Framebuffer {
    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    void* commandBufferOpaque = nullptr; // opaque VkCommandBuffer
    class Swapchain* swapchain = nullptr;
};

class Swapchain {
public:
    Swapchain(ResourceManager& ctx, ::GLFWwindow* window);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&&) = delete;
    Swapchain& operator=(Swapchain&&) = delete;

    void Create();
    void Recreate();

    Extent2D GetExtent() const;
    uint32_t GetImageCount() const;
    uint32_t GetMinImageCount() const;
    ImageViewHandle GetImageViewHandle(uint32_t index) const;
    ImageViewHandle GetDepthImageViewHandle() const;
    Format GetSurfaceFormat() const;
    Format GetDepthFormat() const;

    bool FramebufferResized() const;
    void ClearFramebufferResized();
    void SetFramebufferResized(bool v);
    uint32_t GetCurrentImageIndex() const;
    uint32_t GetCurrentFrameIndex() const;

    // Frame acquisition
    Framebuffer AcquireFramebuffer();
    void EndAndPresentFramebuffer(Framebuffer& fb);

    // Exposed for compatibility – preferred via GetExtent/GetImageCount
    uint32_t framesInFlight() const;
    int GetWindowWidth() const;
    int GetWindowHeight() const;

    // Internal impl accessor
    struct Impl;
    Impl* GetImpl();
    const Impl* GetImpl() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace smvk
