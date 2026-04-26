#pragma once

#include <stdexcept>
#include <vector>
#include <cstdint>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include <Engine/Renderer/context.h>


class VulkanSwapchain {

public:
    int current_width, current_height = 0;

    VulkanSwapchain(
        Context& context,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkSurfaceKHR surface,
        VkQueue presentQueue,
        GLFWwindow* window,
        uint32_t framesInFlight,
        uint32_t desiredImageCount
    )
        : ctx_(context),
          physicalDevice_(physicalDevice),
          device_(device),
          surface_(surface),
          presentQueue_(presentQueue),
          window_(window),
          framesInFlight_(framesInFlight),
          desiredImageCount_(desiredImageCount) {
    }

    ~VulkanSwapchain() {
        DestroySyncObjects();
        CleanupSwapchain();
    }

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    void Create() {
        CreateSwapchain();
        CreateImageViews();
        CreateDepthResources();
        CreateSyncObjects();
    }

    void Recreate() {
        int width = 0;
        int height = 0;

        glfwGetFramebufferSize(window_, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window_, &width, &height);
            glfwWaitEvents();
        }

        extent_.width = width;
        extent_.height = height;

        vkDeviceWaitIdle(device_);

        CleanupSwapchain();

        CreateSwapchain();
        CreateImageViews();
        CreateDepthResources();

        currentImageIndex_ = 0;
    }

    // Getters so it can be marked as const on callee site?. ..Dont quite like it.
    VkSwapchainKHR GetHandle() const { return swapchain_; }
    VkFormat GetImageFormat() const { return surfaceFormat_; }
    VkExtent2D GetExtent() const { return extent_; }

    uint32_t GetImageCount() const {
        return static_cast<uint32_t>(images_.size());
    }

    VkImage GetImage(uint32_t index) const {
        return images_.at(index);
    }

    VkImageView GetImageView(uint32_t index) const {
        return imageViews_.at(index);
    }

    VkImage GetCurrentImage() const {
        return images_.at(currentImageIndex_);
    }

    VkImageView GetCurrentImageView() const {
        return imageViews_.at(currentImageIndex_);
    }

    uint32_t GetCurrentImageIndex() const {
        return currentImageIndex_;
    }

    uint32_t GetCurrentFrameIndex() const {
        return currentFrame_;
    }

    VkSemaphore GetImageAvailableSemaphore() const {
        return imageAvailableSemaphores_.at(currentFrame_);
    }

    VkFence& GetInFlightFence() /*const*/ {
        return inFlightFences_.at(currentFrame_);
    }

    VkSemaphore GetRenderFinishedSemaphore(uint32_t imageIndex) const {
        return renderFinishedSemaphores_.at(imageIndex);
    }

    ImageHandle GetDepthImage() const {
        return depthImage_;
    }

    void AdvanceFrame() {
        currentFrame_ = (currentFrame_ + 1) % framesInFlight_;


        // BAD BAD BAD
        SetCurrentImageIndex(currentFrame_);
    }

    void SetCurrentImageIndex(uint32_t imageIndex) {
        currentImageIndex_ = imageIndex;
    }

    Context& ctx_;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    GLFWwindow* window_ = nullptr;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;


    //VkImage depthImage_;
    ImageHandle depthImage_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;

    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkFence> inFlightFences_;

    VkFormat surfaceFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};

    uint32_t framesInFlight_ = 0;
    uint32_t desiredImageCount_ = 0;

    uint32_t currentImageIndex_ = 0;
    uint32_t currentFrame_ = 0;

    void CreateSwapchain() {
        SwapChainSupportDetails support =
            querySwapChainSupport(physicalDevice_, surface_);

        VkSurfaceFormatKHR surfaceFormat =
            chooseSwapSurfaceFormat(support.formats);

        VkPresentModeKHR presentMode =
            chooseSwapPresentMode(support.presentModes);

        VkExtent2D swapExtent =
            chooseSwapExtent(support.capabilities, window_);

        uint32_t imageCount = desiredImageCount_;
        if (imageCount < support.capabilities.minImageCount) {
            imageCount = support.capabilities.minImageCount;
        }

        if (support.capabilities.maxImageCount > 0 &&
            imageCount > support.capabilities.maxImageCount) {
            imageCount = support.capabilities.maxImageCount;
        }

        QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
        uint32_t queueFamilyIndices[] = {
            indices.graphicsFamily.value(),
            indices.presentFamily.value()
        };

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = swapExtent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        VkResult result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan swapchain.");
        }

        uint32_t actualImageCount = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, nullptr);

        images_.resize(actualImageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, images_.data());

        imageViews_.resize(actualImageCount);
        renderFinishedSemaphores_.resize(actualImageCount, VK_NULL_HANDLE);

        surfaceFormat_ = surfaceFormat.format;
        extent_ = swapExtent;
    }

    void CreateImageViews() {
        for (size_t i = 0; i < images_.size(); ++i) {
            VkImageView imageView = VK_NULL_HANDLE;

            ctx_.CreateImageView(
                images_[i],
                surfaceFormat_,
                VK_IMAGE_ASPECT_COLOR_BIT,
                1, // no mips is still 1 levle
                0,
                1,
                VK_IMAGE_VIEW_TYPE_2D,
                imageView
            );

            imageViews_[i] = imageView;
        }
    }

    void CreateDepthResources() {
        VkFormat depthFormat = findDepthFormat(physicalDevice_);

        depthImage_ = ctx_.CreateImage(
            extent_.width,
            extent_.height,
            depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            1,
            1
        );

        auto obj_ = ctx_.images_.get(depthImage_);
        ctx_.CreateImageView(
            obj_->image,
            obj_->format,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            1,
            0,
            1,
            VK_IMAGE_VIEW_TYPE_2D,
            obj_->renderView
        );
        obj_->sampleView = obj_->renderView;

        ctx_.TransitionImageLayout(
            depthImage_,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        );
    }

    void CreateSyncObjects() {
        imageAvailableSemaphores_.resize(framesInFlight_, VK_NULL_HANDLE);
        inFlightFences_.resize(framesInFlight_, VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < renderFinishedSemaphores_.size(); ++i) {
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create render-finished semaphore.");
            }
        }

        for (size_t i = 0; i < framesInFlight_; ++i) {
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
                vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create per-frame sync objects.");
            }
        }
    }

    void DestroySyncObjects() {
        for (VkSemaphore semaphore : renderFinishedSemaphores_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
        renderFinishedSemaphores_.clear();

        for (VkSemaphore semaphore : imageAvailableSemaphores_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
        imageAvailableSemaphores_.clear();

        for (VkFence fence : inFlightFences_) {
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(device_, fence, nullptr);
            }
        }
        inFlightFences_.clear();
    }

    void CleanupSwapchain() {

        if (depthImage_) {
            destroy(&ctx_, depthImage_);
            depthImage_ = {};
        }

        for (VkImageView view : imageViews_) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, view, nullptr);
            }
        }

        imageViews_.clear();
        images_.clear();

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }
};
