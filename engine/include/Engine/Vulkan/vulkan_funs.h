#pragma once

#include <engine/Vulkan/vulkan_setup.h>

void createSampler(
	const VkPhysicalDevice& physicalDevice,
	const VkDevice& device,
	VkSampler& sampler,
	int levelCount
) {
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;

	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);

	samplerInfo.anisotropyEnable = VK_TRUE;
	samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

	// This is a hardware/spec limit. Thats why im asserting here...
	// Maybe we can query gpu capabilities to give better paths to more powerful devices.
	assert(properties.limits.maxUniformBufferRange >= 1 << 16);

	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;

	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	//samplerInfo.maxLod = 0.0f;
	samplerInfo.maxLod = float(levelCount - 1);

	VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &sampler));

}
