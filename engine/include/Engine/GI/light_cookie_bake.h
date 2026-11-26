// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Fixture light-cookie baking: the geometric half of the editor's
// fixture-cookie workflow (see docs/light_cookie_baking.md). Resolves a
// LightCookieBake recipe against a live editor World and ray-samples an
// opaque-coverage cookie texture through an occluder-only Embree BVH. The
// pixel payload (a .texture asset + its source image, then a setLightAttr
// cookie bind) is the CALLER's job — the editor's asset library (smvk::assets)
// lives in source_import, which the engine does not link.
//
// Scope: spot lights only, opaque binary coverage only (colored transmission
// and point-light cubemap cookies are explicitly out of scope until the
// renderer/shader grow those paths).

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace tour {
struct World;
namespace asset {
struct LightCookieBake;
} // namespace asset
} // namespace tour

namespace tour::gi {

// Result of baking one fixture self-occlusion cookie for a spot light.
struct LightCookieBakeResult {
    uint32_t resolution = 0;
    // resolution * resolution * 4, row-major, top-to-bottom, linear RGBA8.
    // rgb = opaque coverage (255 = unoccluded, 0 = blocked), a = 255. The
    // image V axis aligns with the runtime SpotConeUV projection (row 0 is
    // the +v edge, matching a direct .Sample() of the cookie texture).
    std::vector<uint8_t> rgba;
    glm::vec3 position{0.0f};               // resolved light position (world)
    glm::vec3 direction{0.0f, 0.0f, -1.0f}; // resolved aim (world, normalized)
    float outerAngle = 0.0f;                // resolved outer cone angle (rad)
    uint32_t occluderTriangles = 0;         // occluder BVH triangle count
};

// Bake a spot cookie from a live editor World: resolve the light and its
// occluder nodes by stable UUID, build an occluder-only TriangleBvh, and
// ray-sample opaque coverage (opaque AnyHit) from jittered emitter positions
// through the cone, averaged in linear space. Spot lights only; point/
// directional/other are rejected. Returns false + err on failure (a missing
// light/occluder, a non-spot light, or an empty occluder set).
bool BakeSpotLightCookie(World& world,
                         const tour::asset::LightCookieBake& recipe,
                         LightCookieBakeResult& out,
                         std::string& err);

} // namespace tour::gi

// COMPAT (the namespace/RHI boundary audit): the smvk::gi aliases keep
// existing call sites compiling through the consumer migration.
namespace smvk::gi {
using ::tour::gi::BakeSpotLightCookie;
using ::tour::gi::LightCookieBakeResult;
} // namespace smvk::gi
