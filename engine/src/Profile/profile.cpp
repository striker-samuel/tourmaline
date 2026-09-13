#include <Engine/Profile/profile.h>

#if USE_OPTICK
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "volk.h"
// Getters for device / queues live in the Vulkan backend (impl.cpp).
#include "../Vulkan/detail/vulkan_impl.h"
#endif

namespace smvk::profiler {

namespace {
bool g_enabled = false;     // --use_optick (CPU capture session)
bool g_gpuEnabled = false;  // --use_optick_gpu (GPU timelines; implies enabled)
}

void SetEnabled(bool enabled) { g_enabled = enabled; }
bool Enabled() { return g_enabled; }
void SetGpuEnabled(bool enabled) {
    g_gpuEnabled = enabled;
    if (enabled)
        g_enabled = true; // --use_optick_gpu implies --use_optick
}
bool GpuEnabled() { return g_gpuEnabled; }

void InitVulkanGPU() {
#if USE_OPTICK
    if (!GpuEnabled())
        return;
    static bool initialized = false;
    if (initialized)
        return;

    VkDevice device = smvk::GetVkDevice();
    VkPhysicalDevice physicalDevice = smvk::GetVkPhysicalDevice();
    VkQueue queue = smvk::GetVkGraphicsQueue();
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || queue == VK_NULL_HANDLE)
        return;
    uint32_t family = smvk::GetVkGraphicsQueueFamily();

    // Fill Optick's dispatch table from volk-loaded entry points.
    Optick::VulkanFunctions functions{};
    functions.vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties_>(vkGetPhysicalDeviceProperties);
    functions.vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool_>(vkCreateQueryPool);
    functions.vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool_>(vkCreateCommandPool);
    functions.vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers_>(vkAllocateCommandBuffers);
    functions.vkCreateFence = reinterpret_cast<PFN_vkCreateFence_>(vkCreateFence);
    functions.vkCmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool_>(vkCmdResetQueryPool);
    functions.vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit_>(vkQueueSubmit);
    functions.vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences_>(vkWaitForFences);
    functions.vkResetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer_>(vkResetCommandBuffer);
    functions.vkCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp_>(vkCmdWriteTimestamp);
    functions.vkGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults_>(vkGetQueryPoolResults);
    functions.vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer_>(vkBeginCommandBuffer);
    functions.vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer_>(vkEndCommandBuffer);
    functions.vkResetFences = reinterpret_cast<PFN_vkResetFences_>(vkResetFences);
    functions.vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool_>(vkDestroyCommandPool);
    functions.vkDestroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool_>(vkDestroyQueryPool);
    functions.vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence_>(vkDestroyFence);
    functions.vkFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers_>(vkFreeCommandBuffers);

    OPTICK_GPU_INIT_VULKAN(&device, &physicalDevice, &queue, &family, 1, &functions);
    initialized = true;
#endif
}

void Shutdown() {
#if USE_OPTICK
    OPTICK_SHUTDOWN();
#endif
}

} // namespace smvk::profiler
