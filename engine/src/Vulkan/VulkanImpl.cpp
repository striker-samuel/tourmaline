//// Volk implementation
//#define VK_NO_PROTOTYPES
//#define VOLK_IMPLEMENTATION
//#include <volk.h>
//
//// VMA implementation
//// (VMA needs Vulkan types included first, so include volk before vk_mem_alloc)
//#define VMA_IMPLEMENTATION
//#include "vk_mem_alloc.h"
//

#define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
#define VMA_DEBUG_DETECT_CORRUPTION 1
#define VMA_ASSERT(expr) assert(expr)
# define ENGINE_NAME "VulkanToy"

#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_VULKAN_VERSION 1003800

// This is because we use volk, so vma shouldnt manage dynamic load. 
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "vk_mem_alloc.h"

