// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The DLSS Vulkan adapter's implementation - see dlss_vulkan_adapter.h.
// The translation + packaging moved VERBATIM from dlss.cpp's first
// anonymous-namespace block (the audit's section 6/P1 finding: the
// adaptation is backend code; the orchestration stays in dlss.cpp).

#include "dlss_vulkan_adapter.h"

#ifdef _WIN32

#include <cassert>

namespace smvk::dlss_vk {

VkFormat ToVkFormat(Format f) {
	switch (f) {
	case Format::RGBA16_Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case Format::RG16_Float: return VK_FORMAT_R16G16_SFLOAT;
	case Format::R32_Float: return VK_FORMAT_R32_SFLOAT;
	case Format::D32_Float: return VK_FORMAT_D32_SFLOAT;
	// Scene depth is a stencil format (findDepthFormat prefers
	// D32_SFLOAT_S8_UINT, falls back to D24_UNORM_S8_UINT). The DLSS
	// plugin validates the tagged depth format — returning UNDEFINED
	// here trips "Cannot have undefined format" in SL and, in release
	// (assert compiled out), poisons the evaluate that later aborts
	// in VK_CHECK on toggle-off. Map the real formats through.
	case Format::D32_Float_S8_UInt: return VK_FORMAT_D32_SFLOAT_S8_UINT;
	case Format::D24_UNorm_S8_UInt: return VK_FORMAT_D24_UNORM_S8_UINT;
	case Format::D16_UNorm: return VK_FORMAT_D16_UNORM;
	default: break;
	}
	assert(false && "ToVkFormat: format not handled by the DLSS module");
	return VK_FORMAT_UNDEFINED;
}

uint32_t ToVkUsage(ImageUsage u) {
	uint32_t vk = 0;
	if ((u & ImageUsage::TransferSrc) != ImageUsage::None)
		vk |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if ((u & ImageUsage::TransferDst) != ImageUsage::None)
		vk |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if ((u & ImageUsage::Sampled) != ImageUsage::None)
		vk |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if ((u & ImageUsage::Storage) != ImageUsage::None)
		vk |= VK_IMAGE_USAGE_STORAGE_BIT;
	if ((u & ImageUsage::ColorAttachment) != ImageUsage::None)
		vk |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ((u & ImageUsage::DepthStencilAttachment) != ImageUsage::None)
		vk |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	return vk;
}

TaggedResource::TaggedResource(Extent2D extent2d, Format format,
		ImageUsage usage, VkImageLayout layout, sl::BufferType type,
		sl::ResourceLifecycle lifecycle, VkImage vkImage,
		VkImageView vkView)
	: resource(sl::ResourceType::eTex2d,
		reinterpret_cast<void*>(vkImage),
		nullptr,
		reinterpret_cast<void*>(vkView),
		static_cast<uint32_t>(layout))
	, extent{ 0u, 0u, extent2d.width, extent2d.height }
{
	// Body assignment: the tag copies `extent`, which is only fully
	// initialized after the member-init list ran (declaration order).
	resource.width = extent2d.width;
	resource.height = extent2d.height;
	resource.nativeFormat = ToVkFormat(format);
	resource.mipLevels = 1;
	resource.arrayLayers = 1;
	resource.usage = usage != ImageUsage::None
		? ToVkUsage(usage) : VK_IMAGE_USAGE_SAMPLED_BIT;
	tag.resource = &resource;
	tag.type = type;
	tag.lifecycle = lifecycle;
	tag.extent = extent;
}

} // namespace smvk::dlss_vk

#endif // _WIN32
