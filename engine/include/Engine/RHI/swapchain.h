#pragma once
#include <Engine/smvk.h>
#include <Engine/RHI/resource_manager.h>
#include <memory>

// SDL3 windowing (forward-declared; .cpp includes <SDL3/SDL_video.h>).
struct SDL_Window;

namespace smvk {

struct Framebuffer {
    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    void* commandBufferOpaque = nullptr; // opaque VkCommandBuffer
    class Swapchain* swapchain = nullptr;
    // Async-split second graphics part (G2, true async compute): set by
    // RenderGraph::Execute when the frame splits G1/C/G2 (null = single
    // submit). EndAndPresent ends + submits it, waiting on the compute
    // timeline below before presenting.
    void* splitCommandBufferOpaque = nullptr;
    void* splitWaitSemaphoreOpaque = nullptr; // timeline semaphore (C -> G2)
    uint64_t splitWaitValue = 0;
    // G1 -> G2 explicit ordering (async split): G2 waits G1's timeline too,
    // so G1's acquire wait + swapchain layout barrier are inherited by
    // construction (same-queue submissions may overlap per spec).
    void* splitWaitSemaphore2Opaque = nullptr;
    uint64_t splitWaitValue2 = 0;
};

class Swapchain {
public:
    Swapchain(ResourceManager& ctx, SDL_Window* window);
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

    // VSync / presentation-mode control (takes effect on next Recreate).
    void SetPresentMode(PresentMode mode);
    PresentMode GetPresentMode() const;
    PresentMode GetActivePresentMode() const;
    std::vector<PresentMode> GetAvailablePresentModes() const;

    // GPU frame time (ms) of the last retired frame, for the CPU/GPU
    // split overlay. False until a full frame has retired.
    double GetGpuFrameMs() const;
    bool HasGpuTiming() const;
    // GPU stage timings (ms) of the last retired frame — the shadow-path
    // instrumentation for BOTH shadow representations (see GpuStageTimer
    // in smvk.h: PCF raster / EVSM casters / EVSM resolve / EVSM blur).
    // Fills up to count entries of outMs/outValid (index = stage); a
    // stage reports false until it has run + retired at least once.
    // False when the device lacks cross-queue timestamps.
    bool GetGpuStageTimings(double* outMs, bool* outValid, uint32_t count) const;
    // Last vkQueuePresentKHR wall time (ms): FIFO VSync throttle, i.e.
    // wait rather than CPU work.
    double GetPresentWaitMs() const;

    // Exposed for compatibility – preferred via GetExtent/GetImageCount
    uint32_t framesInFlight() const;
    int GetWindowWidth() const;
    int GetWindowHeight() const;

    // Internal impl accessor
    struct Impl;
    Impl* GetImpl();
    const Impl* GetImpl() const;
    // Opaque VulkanSwapchain for the rendergraph's async-compute split
    // (RenderGraph::Execute submits G1/C and records G2 through it).
    void* GetVulkanOpaque();
    const void* GetVulkanOpaque() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace smvk
