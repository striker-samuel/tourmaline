// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Deprecated shim – backend selection is compile-time single Vulkan build.
// Do NOT include volk.h here. Public code must include <Engine/RHI/resource_manager.h>
// and <Engine/RHI/swapchain.h> and use opaque types.
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>
namespace smvk {
	using ResourceManagerImpl = ResourceManager;
	using SwapchainImpl = Swapchain;
	using FrameBufferImpl = Framebuffer;
	// Pipeline/CommandBuffer are opaque in RenderGraph
}
