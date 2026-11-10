// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Keep this independent of Vulkan headers: implicit layers can be injected
// before vkCreateInstance, even when enabledLayerCount is zero.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace smvk::vulkan_layers {

struct BlockedLayer {
    std::string_view name;
    const char* disableEnvironment;
};

// Source: https://www.mattstevens.co.uk/posts/bad-vulkan-layers/
// These are the ten documented crash-prone layers, not the optional Steam,
// AMD-switchable-graphics, or EOS layers listed separately in that article.
inline constexpr std::array<BlockedLayer, 10> kBlockedLayers{{
    {"VK_LAYER_Twitch_Overlay", "DISABLE_TWITCH_VULKAN_OVERLAY"},
    {"VK_LAYER_OW_OVERLAY", "DISABLE_VULKAN_OW_OVERLAY_LAYER"},
    {"VK_LAYER_OW_OBS_HOOK", "DISABLE_VULKAN_OW_OBS_CAPTURE"},
    {"VK_LAYER_OBS_HOOK", "DISABLE_VULKAN_OBS_CAPTURE"},
    {"VK_LAYER_bandicam_helper", "VK_LAYER_bandicam_helper_DEBUG_1"},
    {"VK_LAYER_fpsmon", "DISABLE_FPSMON_LAYER"},
    {"VK_LAYER_playclaw", "DISABLE_PLAYCLAW_LAYER"},
    {"VK_LAYER_reshade", "DISABLE_VK_LAYER_reshade_1"},
    {"MangoHud", "DISABLE_MANGOHUD"},
    {"VK_LAYER_RTSS", "DISABLE_RTSS_LAYER"},
}};

// The Vulkan loader accepts a comma-separated list of exact-name globs.
// Retain the caller's existing filters (including ~implicit~ / ~all~).
inline std::string BuildLoaderDisableFilter(const char* existing) {
    std::string filter = existing ? existing : "";
    for (const auto& layer : kBlockedLayers) {
        if (!filter.empty() && filter.back() != ',')
            filter += ',';
        filter += layer.name;
    }
    return filter;
}

inline bool SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

// Must run before the first Vulkan-loader call. Per-manifest disable switches
// work with older loaders; VK_LOADER_LAYERS_DISABLE covers newer loaders and
// manifests that do not honor their advertised switch.
inline void DisableKnownBadLayers() {
    const char* allow = std::getenv("TOURMALINE_ALLOW_VULKAN_LAYERS");
    if (allow && std::strcmp(allow, "1") == 0)
        return;

    for (const auto& layer : kBlockedLayers) {
        if (!SetEnvironment(layer.disableEnvironment, "1"))
            std::fprintf(stderr, "[Vulkan] unable to disable layer %.*s\n",
                         static_cast<int>(layer.name.size()), layer.name.data());
    }

    const std::string filter =
        BuildLoaderDisableFilter(std::getenv("VK_LOADER_LAYERS_DISABLE"));
    if (!SetEnvironment("VK_LOADER_LAYERS_DISABLE", filter.c_str()))
        std::fprintf(stderr, "[Vulkan] unable to configure Vulkan layer disable filter\n");
}

} // namespace smvk::vulkan_layers
