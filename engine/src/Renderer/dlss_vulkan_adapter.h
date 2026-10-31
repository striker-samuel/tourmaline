// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE DLSS VULKAN ADAPTER (the namespace/RHI boundary audit's P1, todo
// item 8.6): the Streamline VULKAN resource adaptation - the smvk
// Format/ImageUsage -> VkFormat/usage-flag translation and the
// sl::Resource packaging (TaggedResource) - lives HERE, a
// backend-specific implementation file behind a private seam. dlss.cpp
// (the settings + frame orchestration) consumes this surface; its own
// code spells no VkFormat of its own. The DX12 port
// (feature/dx2-rhi) writes its own adapter against the same seam.
//
// PRIVATE (src/-internal, never a public include) + Windows-only by
// design: the SL runtime ships Windows binaries and the Linux build
// compiles the dlss.cpp stubs. This header may spell Vk*/sl::* types -
// it IS the backend boundary.
#ifdef _WIN32

#include <Engine/Rendergraph/dlss.h>

#include <cstdint>

#include "sl.h"
#include <vulkan/vulkan_core.h>

namespace smvk::dlss_vk {

// smvk::Format -> VkFormat (the DLSS module's subset - the plugin
// validates the tagged native format; UNDEFINED trips it).
[[nodiscard]] VkFormat ToVkFormat(Format f);

// smvk::ImageUsage -> the VkImageUsageFlags bits.
[[nodiscard]] uint32_t ToVkUsage(ImageUsage u);

// One sl::Resource + tag pair the evaluate call submits. The caller
// owns it BY VALUE and keeps it alive across slDLSSEvaluate (the tag
// points into the struct).
struct TaggedResource {
	sl::Resource resource;
	sl::ResourceTag tag;
	sl::Extent extent;

	// `layout` must be the image layout at the time SL consumes the
	// resource (the rendergraph barriers images to exactly these
	// layouts via the pass image hints before recording): depth
	// DEPTH_STENCIL_READ_ONLY_OPTIMAL, inputs SHADER_READ_ONLY_OPTIMAL,
	// the storage output GENERAL.
	TaggedResource(Extent2D extent2d, Format format, ImageUsage usage,
		VkImageLayout layout, sl::BufferType type,
		sl::ResourceLifecycle lifecycle,
		VkImage vkImage, VkImageView vkView);
};

} // namespace smvk::dlss_vk

#endif // _WIN32
