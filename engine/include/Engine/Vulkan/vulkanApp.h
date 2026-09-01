#pragma once
// Deprecated shim – VulkanApp is now merged into App (PIMPL firewall).
// Kept for backward compat, do not include vulkan headers.
#include <Engine/App/app.h>
namespace smvk {
	using VulkanApp = App;
}