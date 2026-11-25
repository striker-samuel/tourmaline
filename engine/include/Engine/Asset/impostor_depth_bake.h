// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace tour::impostor {

// Orthographic, object-local depth image. Depth increases towards the
// camera. A zero coverage sample has no surface; depth alone is not a mask.
struct DepthView {
    glm::vec3 origin{}, right{}, up{}, towardCamera{};
    float halfExtent = 0;
    uint32_t resolution = 0;
    std::vector<float> depth;
    std::vector<uint8_t> coverage;
};

inline bool Finite(glm::vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// CPU reference baker for opaque indexed triangles. The caller supplies
// an orthonormal camera frame. No material or alpha sampling is implied.
// Bounded resolution keeps this offline reference implementation manageable.
inline DepthView BakeDepth(std::span<const glm::vec3> positions,
                           std::span<const uint32_t> indices,
                           glm::vec3 origin, glm::vec3 right, glm::vec3 up,
                           glm::vec3 towardCamera, float halfExtent,
                           uint32_t resolution) {
    if (!Finite(origin) || !Finite(right) || !Finite(up) || !Finite(towardCamera) ||
        !std::isfinite(halfExtent) || halfExtent <= 0 || resolution < 2 ||
        resolution > 512 || indices.empty() || indices.size() % 3 != 0)
        return {};
    for (uint32_t i : indices)
        if (i >= positions.size() || !Finite(positions[i]))
            return {};
    DepthView view{origin, right, up, towardCamera, halfExtent, resolution, {}, {}};
    view.depth.resize(size_t(resolution) * resolution);
    view.coverage.resize(view.depth.size());
    const float scale = float(resolution) / (2 * halfExtent);
    auto project = [&](glm::vec3 p) {
        p -= origin;
        return glm::vec3(glm::dot(p, right) * scale + resolution * 0.5f - 0.5f,
                         glm::dot(p, up) * scale + resolution * 0.5f - 0.5f,
                         glm::dot(p, towardCamera));
    };
    auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x*b.y - a.y*b.x; };
    for (size_t i = 0; i < indices.size(); i += 3) {
        const auto a = project(positions[indices[i]]);
        const auto b = project(positions[indices[i+1]]);
        const auto c = project(positions[indices[i+2]]);
        if (!Finite(a) || !Finite(b) || !Finite(c))
            return {};
        const glm::vec2 ab = glm::vec2(b-a), ac = glm::vec2(c-a);
        const float area = cross(ab, ac);
        if (!std::isfinite(area) || std::abs(area) < 1e-8f)
            continue;
        const float last = float(resolution-1);
        const int x0 = int(std::ceil(std::clamp(std::min({a.x,b.x,c.x}),0.0f,last)));
        const int x1 = int(std::floor(std::clamp(std::max({a.x,b.x,c.x}),0.0f,last)));
        const int y0 = int(std::ceil(std::clamp(std::min({a.y,b.y,c.y}),0.0f,last)));
        const int y1 = int(std::floor(std::clamp(std::max({a.y,b.y,c.y}),0.0f,last)));
        for (int y=y0; y<=y1; ++y) for (int x=x0; x<=x1; ++x) {
            const glm::vec2 d = glm::vec2(float(x),float(y))-glm::vec2(a);
            const float u = cross(d,ac)/area, v = cross(ab,d)/area;
            if (u < 0 || v < 0 || u+v > 1)
                continue;
            const float z = a.z*(1-u-v)+b.z*u+c.z*v;
            const size_t k = size_t(y)*resolution+x;
            if (std::isfinite(z) && (!view.coverage[k] || z > view.depth[k])) {
                view.depth[k] = z;
                view.coverage[k] = 1;
            }
        }
    }
    return view;
}

struct TriangleProxy {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
};

// Reconstruct a view's depth surface into ordinary indexed triangles,
// suitable as input to a triangle BLAS after geometry upload. Do not bridge
// holes or discontinuities. Multiple views may be baked independently;
// merging them into a watertight proxy is deliberately not done here.
inline TriangleProxy ReconstructDepth(const DepthView& view, float maxDepthJump) {
    TriangleProxy proxy;
    const uint32_t n = view.resolution;
    if (n < 2 || n > 512 || view.coverage.size() != size_t(n)*n ||
        view.depth.size() != view.coverage.size() || !std::isfinite(maxDepthJump) ||
        maxDepthJump < 0 || !std::isfinite(view.halfExtent) || view.halfExtent <= 0 ||
        !Finite(view.origin) || !Finite(view.right) || !Finite(view.up) ||
        !Finite(view.towardCamera))
        return proxy;
    constexpr uint32_t invalid = UINT32_MAX;
    std::vector<uint32_t> remap(view.depth.size(), invalid);
    auto vertex = [&](uint32_t k) {
        if (remap[k] != invalid)
            return remap[k];
        const float x = ((float(k%n)+0.5f)/n*2-1)*view.halfExtent;
        const float y = ((float(k/n)+0.5f)/n*2-1)*view.halfExtent;
        const auto p = view.origin+view.right*x+view.up*y+view.towardCamera*view.depth[k];
        remap[k] = uint32_t(proxy.positions.size());
        proxy.positions.push_back(p);
        return remap[k];
    };
    auto tri = [&](uint32_t a,uint32_t b,uint32_t c) {
        if (!view.coverage[a] || !view.coverage[b] || !view.coverage[c])
            return;
        const float da=view.depth[a], db=view.depth[b], dc=view.depth[c];
        if (!std::isfinite(da) || !std::isfinite(db) || !std::isfinite(dc) ||
            std::max({da,db,dc})-std::min({da,db,dc}) > maxDepthJump)
            return;
        proxy.indices.push_back(vertex(a));
        proxy.indices.push_back(vertex(b));
        proxy.indices.push_back(vertex(c));
    };
    for (uint32_t y=0;y+1<n;++y) for (uint32_t x=0;x+1<n;++x) {
        const uint32_t a=y*n+x;
        tri(a,a+1,a+n);
        tri(a+1,a+n+1,a+n);
    }
    return proxy;
}

} // namespace tour::impostor
