// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Bridge the existing per-frame RenderViewSet to GPUScene's origin-relative
// culling and indexed-raster view ABI. A view owns its own visibility result;
// shadow views must never reuse the main camera's culled work list because
// off-screen geometry can cast visible shadows.

#include <Engine/Renderer/gpu_scene_validation.h>
#include <Engine/Renderer/gpu_scene_raster.h>
#include <Engine/Renderer/renderview.h>

#include <array>
#include <cmath>
#include <cstdint>

#include <glm/gtc/matrix_transform.hpp>

namespace tour::gpu_scene {

struct GpuSceneRenderView {
    InstanceCullGpuView cull{};
    RasterViewData raster{};
    uint32_t sourceViewIndex = smvk::RenderViewSet::kInvalidIndex;
    uint32_t flags = smvk::RenderViewFlagBit_None;
    bool valid = false;
};

// Use a shared main-camera origin even for light views. The shadow VP and
// the GPUScene model position must be evaluated in the SAME relative frame.
// Compute VP * translation(origin) in double before downcasting: directly
// translating a float32 matrix at large coordinates loses cancellation bits.
// This adapter is raster-consistent with today's float32 RenderView; it
// cannot restore precision already lost by the source camera or scene.
[[nodiscard]] inline GpuSceneRenderView MakeGpuSceneRenderView(
    const smvk::RenderView& source,
    uint32_t sourceIndex,
    glm::dvec3 renderOrigin) {
    GpuSceneRenderView out{};
    out.sourceViewIndex = sourceIndex;
    out.flags = source.m_renderViewFlags;
    const auto cull = LegacyRelativeFrustum(source, renderOrigin);
    if (!PackInstanceCullView(cull, out.cull))
        return out;

    glm::dmat4 shift(1.0);
    shift[3] = glm::dvec4(renderOrigin, 1.0);
    const glm::dmat4 relativeVP = glm::dmat4(source.m_viewProjectionMatrix) * shift;
    for (uint32_t column = 0; column < 4; ++column)
        for (uint32_t row = 0; row < 4; ++row)
            if (!std::isfinite(relativeVP[column][row]) ||
                std::abs(relativeVP[column][row]) > double(3.402823466e38f))
                return out;
    out.raster.relativeViewProjection = glm::mat4(relativeVP);
    out.raster.origin = math::SplitDoubleFloat(renderOrigin);
    out.valid = true;
    return out;
}

// Upload-ready, index-preserving snapshot of ALL allocated RenderViews.
// Main/CSM/spot/point/custom views share one ABI, while culling and indirect
// work output remain independent per view. Reject invalid views rather than
// inventing all-visible shadow-caster work.
struct GpuSceneRenderViewSet {
    std::array<GpuSceneRenderView, smvk::kMaxRenderViews> views{};
    uint32_t count = 0;

    [[nodiscard]] bool Build(const smvk::RenderViewSet& source, glm::dvec3 renderOrigin) {
        count = 0;
        if (source.Count() > views.size())
            return false;
        for (uint32_t i = 0; i < source.Count(); ++i) {
            views[i] = MakeGpuSceneRenderView(source.Get(i), i, renderOrigin);
            if (!views[i].valid) {
                count = 0;
                return false;
            }
            ++count;
        }
        return true;
    }
};

} // namespace tour::gpu_scene
