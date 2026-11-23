// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/GI/light_cookie_bake.h>

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/tour.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/triangle_bvh.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>

#include <glm/glm.hpp>
#include <gltf_import/gltf.h>

#include <algorithm>
#include <cmath>

namespace tour::gi {

namespace {

// One sample of the emitter area: a point on the bulb's disc of radius
// `emitterRadius`, centered on `origin`, in the s/u plane (the cone's
// right/up basis perpendicular to the aim axis). Uniform-area disc sampling
// gives the soft penumbrae the emitter radius is meant to model.
inline glm::vec3 EmitterJitter(const glm::vec3& origin,
                               const glm::vec3& s,
                               const glm::vec3& u,
                               float emitterRadius,
                               Pcg32& rng) {
    if (!(emitterRadius > 0.0f))
        return origin;
    const float r = emitterRadius * std::sqrt(rng.Float());
    const float a = 6.283185307f * rng.Float();
    return origin + s * (r * std::cos(a)) + u * (r * std::sin(a));
}

} // namespace

bool BakeSpotLightCookie(World& world,
                         const tour::asset::LightCookieBake& recipe,
                         LightCookieBakeResult& out,
                         std::string& err) {
    out = LightCookieBakeResult{};
    // The .tour parser already bounds these (resolution 16..4096 pow2,
    // samples 1..4096, captureDistance > 0), but the recipe may also be
    // constructed programmatically — guard the baker's own invariants.
    if (recipe.lightUuid.empty()) {
        err = "light cookie bake: recipe carries no light uuid";
        return false;
    }
    if (recipe.resolution < 16 || recipe.resolution > 4096 ||
        (recipe.resolution & (recipe.resolution - 1))) {
        err = "light cookie bake: resolution must be a power of two in 16..4096";
        return false;
    }
    if (recipe.samples == 0 || recipe.samples > 4096) {
        err = "light cookie bake: samples must be in 1..4096";
        return false;
    }
    if (!(recipe.captureDistance > 0.0f)) {
        err = "light cookie bake: capture distance must be positive";
        return false;
    }

    ::TransformHierarchy& scene = world.scene;
    recalculateGlobalTransforms(scene);

    // Resolve the light node by stable uuid (node reorder-safe).
    const uint32_t nodeCount = uint32_t(scene.hierarchy.size());
    uint32_t lightNode = 0;
    bool foundLight = false;
    for (uint32_t i = 0; i < nodeCount; ++i) {
        if (!IsNodeAlive(scene, i))
            continue;
        if (GetNodeUuid(scene, i) == recipe.lightUuid) {
            lightNode = i;
            foundLight = true;
            break;
        }
    }
    if (!foundLight) {
        err = "light cookie bake: light uuid '" + recipe.lightUuid + "' not found";
        return false;
    }
    const LightRow* lrow = FindLight(scene, lightNode);
    if (!lrow || lrow->light >= world.lightLocations.size()) {
        err = "light cookie bake: light node carries no cooked light";
        return false;
    }
    const World::LightLocation& ll = world.lightLocations[lrow->light];
    const SceneLight* light = ll.cluster ? ll.cluster->lights.get(ll.handle) : nullptr;
    if (!light) {
        err = "light cookie bake: light handle invalid";
        return false;
    }
    if (light->type != SceneLight::Type::Spot) {
        err = "light cookie bake: light is not a spot (point/directional unsupported)";
        return false;
    }

    out.position = glm::vec3(scene.globalTransform[lightNode][3]);
    glm::vec3 aim =
        glm::vec3(scene.globalTransform[lightNode] * glm::vec4(GltfPunctualAimAxis(0), 0.0f));
    const float aimLen = glm::length(aim);
    if (!(aimLen > 1e-6f)) {
        err = "light cookie bake: degenerate light aim";
        return false;
    }
    aim /= aimLen;
    out.direction = aim;
    out.outerAngle = light->outerConeAngle;

    // Occluder geometry: world-space triangles from the recipe's occluder
    // nodes ONLY (bulb mounts / large arms excluded by authoring). A
    // selected node without a mesh is tolerated (it may be a bare grouping
    // node), but a recipe uuid that resolves to NO node is a stale recipe —
    // fail loudly rather than bake a silently-partial cookie.
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    std::vector<bool> matched(recipe.occluderUuids.size(), false);
    for (uint32_t i = 0; i < nodeCount; ++i) {
        if (!IsNodeAlive(scene, i))
            continue;
        const std::string& uuid = GetNodeUuid(scene, i);
        if (uuid.empty())
            continue;
        const auto it = std::find(recipe.occluderUuids.begin(), recipe.occluderUuids.end(), uuid);
        if (it == recipe.occluderUuids.end())
            continue;
        matched[size_t(std::distance(recipe.occluderUuids.begin(), it))] = true;

        const MeshRow* mrow = FindMesh(scene, i);
        if (!mrow || mrow->mesh >= world.meshLocations.size())
            continue;
        const World::MeshLocation& ml = world.meshLocations[mrow->mesh];
        const MeshData* upload = ml.cluster ? ml.cluster->GetRetainedUpload(ml.handle) : nullptr;
        if (!upload || upload->vertices.empty() || upload->indices.empty())
            continue;

        const uint32_t base = uint32_t(positions.size());
        const glm::mat4& G = scene.globalTransform[i];
        for (const MeshVertex& v : upload->vertices) {
            const glm::vec4 wp = G * glm::vec4(v.pos, 1.0f);
            positions.push_back(glm::vec3(wp));
        }
        for (const uint32_t k : upload->indices)
            indices.push_back(base + k);
    }
    for (size_t i = 0; i < matched.size(); ++i) {
        if (!matched[i]) {
            err = "light cookie bake: occluder uuid '" + recipe.occluderUuids[i] + "' not found";
            return false;
        }
    }
    if (indices.empty()) {
        err = "light cookie bake: occluder nodes carry no geometry";
        return false;
    }

    TriangleBvh bvh;
    bvh.Build(positions, indices);
    out.occluderTriangles = uint32_t(indices.size() / 3);
    if (bvh.Empty()) {
        err = "light cookie bake: empty occluder BVH";
        return false;
    }

    // The cone frame: axis = aim, s/u = the runtime SpotConeUV basis (the
    // exact same upRef branch as light.hlsli), so cookie texels line up with
    // the shader's gobo projection.
    const glm::vec3 upRef =
        std::abs(aim.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 s = glm::normalize(glm::cross(aim, upRef));
    const glm::vec3 u = glm::cross(s, aim);
    const float tanOuter = std::tan(std::min(out.outerAngle, 1.5f));
    const float maxDist = recipe.captureDistance;

    const uint32_t res = recipe.resolution;
    out.resolution = res;
    out.rgba.assign(size_t(res) * res * 4, 0);

    // Deterministic per-recipe seed (FNV-1a over the light uuid) so a rebake
    // of an unchanged scene reproduces the identical cookie.
    uint64_t seed = 1469598103934665603ull;
    for (const char c : recipe.lightUuid) {
        seed ^= uint8_t(c);
        seed *= 1099511628211ull;
    }
    Pcg32 rng(seed);

    const uint32_t samples = recipe.samples;
    const float invSamples = 1.0f / float(samples);

    for (uint32_t py = 0; py < res; ++py) {
        // Image row 0 = top = spotUV.v 0 (the +u edge): the direction basis
        // below maps (px, py) straight to SpotConeUV's (s, u) and the .Sample
        // reads it back without a flip.
        const float v = (float(py) + 0.5f) / float(res);
        const float ndcU = v * 2.0f - 1.0f;
        for (uint32_t px = 0; px < res; ++px) {
            const float h = (float(px) + 0.5f) / float(res);
            const float ndcS = h * 2.0f - 1.0f;
            // Inverse of SpotConeUV: ndc = (dot(s,dir), dot(u,dir)) /
            // (dot(axis,dir) * tan(outer)), so dir = axis + s*(ndcS*tan) +
            // u*(ndcU*tan) normalized.
            const glm::vec3 dir =
                glm::normalize(aim + s * (ndcS * tanOuter) + u * (ndcU * tanOuter));
            uint32_t open = 0;
            for (uint32_t k = 0; k < samples; ++k) {
                const glm::vec3 origin =
                    EmitterJitter(out.position, s, u, recipe.emitterRadius, rng);
                if (!bvh.AnyHit(origin, dir, maxDist))
                    ++open;
            }
            const float coverage = float(open) * invSamples;
            const uint8_t cov = uint8_t(std::min(255.0f, std::round(coverage * 255.0f)));
            const size_t o = (size_t(py) * res + size_t(px)) * 4;
            out.rgba[o + 0] = cov;
            out.rgba[o + 1] = cov;
            out.rgba[o + 2] = cov;
            out.rgba[o + 3] = 255;
        }
    }
    return true;
}

} // namespace tour::gi
