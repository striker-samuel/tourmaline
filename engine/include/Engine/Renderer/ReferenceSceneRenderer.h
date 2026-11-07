// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// GPU-independent, deterministic camera path tracer over a *finalized*
// GiTraceScene snapshot. Geometry intersection is performed by its Embree
// backend. This API intentionally owns neither a World nor an RHI device:
// editors, test fixtures and offline tools can all render the same snapshot.
//
// Limitations: this is a reference for the built-in metallic/roughness model,
// analytic sky and direct sun / point / spot lights. It is not a spectral
// ground truth, and does not reproduce GPU post-processing, transparent
// refraction, normal textures, arbitrary project shader hooks or IBL probes.
#include <atomic>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace tour::gi {
class GiTraceScene;
}

namespace tour::renderer {

struct ReferenceCamera {
    glm::vec3 position{0.0f, 1.0f, 5.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float fovYDeg = 50.0f;
    // 0 = perspective. Positive = orthographic camera half-height.
    float orthoHalfHeight = 0.0f;
};

struct ReferenceRenderSettings {
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t samplesPerPixel = 64;
    uint32_t maxBounces = 3;
    uint32_t seed = 1;
    uint32_t threads = 0; // 0 = available hardware threads
    // Exposure applies only to LDR conversion. linearRadiance is unexposed.
    float exposure = 0.001f;
};

struct ReferenceImage {
    uint32_t width = 0, height = 0;
    // Linear, scene-referred HDR, top-to-bottom, RGB interleaved by pixel.
    std::vector<glm::vec3> linearRadiance;

    // ACES-fit filmic mapping and correct linear-to-sRGB transfer.
    // Output is tightly packed RGBA8, with alpha = 255.
    std::vector<uint8_t> ToRgba8(float exposure) const;

    // Portable float-map HDR export (little-endian, bottom-to-top).
    bool WritePfm(const std::string& filename) const;
};

class ReferenceSceneRenderer {
  public:
    // scene must be finalized before Render() and remain immutable until
    // return. The texture store, materials and Embree BVH are read-only.
    // Cancel may be flipped from another thread; a cancelled image is
    // deliberately returned empty, never a partially complete artifact.
    // completedRows is optional and can be polled during an async render.
    static ReferenceImage Render(const gi::GiTraceScene& scene,
                                 const ReferenceCamera& camera,
                                 const ReferenceRenderSettings& settings,
                                 const std::atomic_bool* cancel = nullptr,
                                 std::atomic<uint32_t>* completedRows = nullptr);
};

} // namespace tour::renderer
