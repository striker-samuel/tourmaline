#pragma once

#include "volk.h"
#include "vk_mem_alloc.h"

#include <Engine/Memory/handle.h>

#include <ktxvulkan.h>


struct Pipeline {
	struct Layout {
		VkPipelineLayout layout = VK_NULL_HANDLE;
		// Hardcoded max of 8
		std::array<VkDescriptorSetLayout, 8> sets{};
	} layout;
	ShaderModuleHandle vert;
	ShaderModuleHandle frag;
	VkPipeline pipeline = VK_NULL_HANDLE;
};

struct DescriptorSet {
	VkDescriptorSetLayout layout = {};
	VkDescriptorSet set = VK_NULL_HANDLE;
};

struct Image {
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};

	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageUsageFlags usage = 0;

	VkExtent3D extent{};
	uint32_t arrayLayers = 0;
	uint32_t levels = 0;

	VkImageView sampleView = VK_NULL_HANDLE;
	VkImageView renderView = VK_NULL_HANDLE;
	VkImageView arrayView[6]{
		VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
		VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE
	};

	// TODO: remove this hacky way!
	bool owns_sampler = false;
	VkSampler sampler = VK_NULL_HANDLE;

	// This is unused now
	//bool is_tex = false;


	// THIS IS WRONG. THIS IS ONLY HERE TO REMIND MYSELF 
	// ON LAYOUT TRACKING. LACYOUT TRACKING IS NOT NEEDED ANYMORE JUST RAWDOW UNDEFINED ANYWAYS
	//https://www.youtube.com/watch?v=0rqWe1M2HiE&t=433s
	//VkLayout current_layout;
	
	
	
	// Lets do rfc so the reource mangare can free unused textures
	// this is only applicable to texutes tho
	//uint32_t reference_counter = 0;

	// Maybe to stream out?
	//uint32_t last_frame_seen;

};

struct DeviceBuffer {
	VkBuffer buffer;
	VmaAllocation alloc;
	VmaAllocationInfo allocInfo;
	VkBufferUsageFlags usage;
	VkMemoryPropertyFlags memory_flags;

	bool is_mapable() const {
		return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}
	bool operator()(const DeviceBuffer& a, const DeviceBuffer& b) const {
        return std::tie(a.buffer, a.alloc, a.usage, a.memory_flags)
             < std::tie(b.buffer, b.alloc, b.usage, b.memory_flags);
    }
};




