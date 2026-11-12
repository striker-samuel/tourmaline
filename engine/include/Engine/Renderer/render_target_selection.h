// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Game-facing selection and configuration vocabulary for future offscreen
// render targets. This header is CPU-only; it allocates NO GPU resources.
#include <Engine/Scene/scene_node_id.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace tour::render {

enum class RenderTargetFormat : uint8_t { Rgba8, Rgba16Float };
enum class RenderTargetUpdate : uint8_t { EveryFrame, OnDemand };
enum class RenderSelectionMode : uint8_t { All, LayerMask, ExplicitNodes };

// Render layers are independent of the existing light-linking channels.
// Explicit handles are WORLD-owned, generation checked at selection time.
// Inclusion overrides the base mode for All/LayerMask; exclusion always wins.
// For ExplicitNodes, only included nodes are eligible.
struct RenderSelection {
    RenderSelectionMode mode = RenderSelectionMode::All;
    uint32_t layerMask = UINT32_MAX;
    std::vector<scene::NodeHandle> include;
    std::vector<scene::NodeHandle> exclude;
    bool includeDescendants = false; // Reserved: requires hierarchy-aware expansion.

    // A caller must pass a validated, live World NodeHandle. A stale handle
    // MUST NOT be treated as a match merely because its slot was recycled.
    bool Selects(scene::NodeHandle handle, uint32_t nodeLayers) const {
        const auto contains = [handle](const std::vector<scene::NodeHandle>& nodes) {
            return std::find(nodes.begin(), nodes.end(), handle) != nodes.end();
        };
        if (handle == scene::kNodeInvalidHandle || contains(exclude))
            return false;
        const bool explicitlyIncluded = contains(include);
        switch (mode) {
        case RenderSelectionMode::All:
            return true;
        case RenderSelectionMode::LayerMask:
            return (nodeLayers & layerMask) != 0 || explicitlyIncluded;
        case RenderSelectionMode::ExplicitNodes:
            return explicitlyIncluded;
        }
        return false;
    }
};

struct RenderTargetDesc {
    uint32_t width = 512;
    uint32_t height = 512;
    RenderTargetFormat format = RenderTargetFormat::Rgba8;
    RenderTargetUpdate update = RenderTargetUpdate::EveryFrame;
    bool depth = true;
    bool generateMipmaps = false;
    RenderSelection selection = {};
};

// Opaque, generation-checked future renderer-owned handle.
// No GPU image or per-frame RenderViewSet index is exposed to gameplay.
struct RenderTargetHandle {
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
    friend bool operator==(const RenderTargetHandle&, const RenderTargetHandle&) = default;
    explicit operator bool() const { return slot != UINT32_MAX; }
};

inline bool ValidRenderTargetDesc(const RenderTargetDesc& desc) {
    return desc.width > 0 && desc.height > 0;
}

} // namespace tour::render
