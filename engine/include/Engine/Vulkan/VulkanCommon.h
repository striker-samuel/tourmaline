// VulkanCommon.h
#pragma once

// If you're using Volk to load function pointers, you typically want no Vulkan prototypes.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <volk.h>            // Provides Vulkan types + function pointer declarations

// VMA expects Vulkan types to be available before including it
#include "vk_mem_alloc.h"    // Include only; implementation in one .cpp

