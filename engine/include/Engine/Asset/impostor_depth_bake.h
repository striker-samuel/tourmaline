// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <utility>
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
                           glm::vec3 origin,
                           glm::vec3 right,
                           glm::vec3 up,
                           glm::vec3 towardCamera,
                           float halfExtent,
                           uint32_t resolution) {
    if (!Finite(origin) || !Finite(right) || !Finite(up) || !Finite(towardCamera) ||
        !std::isfinite(halfExtent) || halfExtent <= 0 || resolution < 2 || resolution > 512 ||
        indices.empty() || indices.size() % 3 != 0)
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
    auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };
    for (size_t i = 0; i < indices.size(); i += 3) {
        const auto a = project(positions[indices[i]]);
        const auto b = project(positions[indices[i + 1]]);
        const auto c = project(positions[indices[i + 2]]);
        if (!Finite(a) || !Finite(b) || !Finite(c))
            return {};
        const glm::vec2 ab = glm::vec2(b - a), ac = glm::vec2(c - a);
        const float area = cross(ab, ac);
        if (!std::isfinite(area) || std::abs(area) < 1e-8f)
            continue;
        const float last = float(resolution - 1);
        const int x0 = int(std::ceil(std::clamp(std::min({a.x, b.x, c.x}), 0.0f, last)));
        const int x1 = int(std::floor(std::clamp(std::max({a.x, b.x, c.x}), 0.0f, last)));
        const int y0 = int(std::ceil(std::clamp(std::min({a.y, b.y, c.y}), 0.0f, last)));
        const int y1 = int(std::floor(std::clamp(std::max({a.y, b.y, c.y}), 0.0f, last)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 d = glm::vec2(float(x), float(y)) - glm::vec2(a);
                const float u = cross(d, ac) / area, v = cross(ab, d) / area;
                if (u < 0 || v < 0 || u + v > 1)
                    continue;
                const float z = a.z * (1 - u - v) + b.z * u + c.z * v;
                const size_t k = size_t(y) * resolution + x;
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
    if (n < 2 || n > 512 || view.coverage.size() != size_t(n) * n ||
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
        const float x = ((float(k % n) + 0.5f) / n * 2 - 1) * view.halfExtent;
        const float y = ((float(k / n) + 0.5f) / n * 2 - 1) * view.halfExtent;
        const auto p =
            view.origin + view.right * x + view.up * y + view.towardCamera * view.depth[k];
        remap[k] = uint32_t(proxy.positions.size());
        proxy.positions.push_back(p);
        return remap[k];
    };
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (!view.coverage[a] || !view.coverage[b] || !view.coverage[c])
            return;
        const float da = view.depth[a], db = view.depth[b], dc = view.depth[c];
        if (!std::isfinite(da) || !std::isfinite(db) || !std::isfinite(dc) ||
            std::max({da, db, dc}) - std::min({da, db, dc}) > maxDepthJump)
            return;
        proxy.indices.push_back(vertex(a));
        proxy.indices.push_back(vertex(b));
        proxy.indices.push_back(vertex(c));
    };
    for (uint32_t y = 0; y + 1 < n; ++y)
        for (uint32_t x = 0; x + 1 < n; ++x) {
            const uint32_t a = y * n + x;
            tri(a, a + 1, a + n);
            tri(a + 1, a + n + 1, a + n);
        }
    return proxy;
}

// Six orthographic views share a square object-local capture volume.
// A flattened atlas uses a 3x2 tile layout; coverage is independent of depth.
struct DepthAtlas {
    uint32_t tileResolution = 0;
    std::vector<DepthView> views;
    std::vector<float> depth;
    std::vector<uint8_t> coverage;
};

inline DepthAtlas BakeDepthAtlas(std::span<const glm::vec3> positions,
                                 std::span<const uint32_t> indices,
                                 uint32_t resolution) {
    if (positions.empty() || resolution < 2 || resolution > 512)
        return {};
    glm::vec3 mn = positions.front(), mx = mn;
    for (auto p : positions) {
        if (!Finite(p))
            return {};
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    const glm::vec3 center = mn * 0.5f + mx * 0.5f;
    const float extent = glm::length(mx - mn) * 0.5f;
    if (!std::isfinite(extent) || extent <= 0)
        return {};
    DepthAtlas atlas;
    atlas.tileResolution = resolution;
    atlas.depth.resize(size_t(resolution) * resolution * 6);
    atlas.coverage.resize(atlas.depth.size());
    for (int axis = 0; axis < 3; ++axis)
        for (int sign : {1, -1}) {
            glm::vec3 forward(0), up(0);
            forward[axis] = float(sign);
            up[(axis + 1) % 3] = 1;
            const auto right = glm::cross(up, forward);
            auto view =
                BakeDepth(positions, indices, center, right, up, forward, extent, resolution);
            if (!view.resolution)
                return {};
            const size_t tile = atlas.views.size();
            for (uint32_t y = 0; y < resolution; ++y)
                for (uint32_t x = 0; x < resolution; ++x) {
                    const size_t dst =
                        (tile / 3 * resolution + y) * 3 * resolution + tile % 3 * resolution + x;
                    const size_t src = size_t(y) * resolution + x;
                    atlas.depth[dst] = view.depth[src];
                    atlas.coverage[dst] = view.coverage[src];
                }
            atlas.views.push_back(std::move(view));
        }
    return atlas;
}

// A view-independent triangle soup for the existing triangle BLAS builder.
// Duplicate/overlapping patches are retained; this is not a closed hull.
inline TriangleProxy ReconstructAtlas(const DepthAtlas& atlas, float maxDepthJump) {
    TriangleProxy result;
    for (const auto& view : atlas.views) {
        auto patch = ReconstructDepth(view, maxDepthJump);
        if (patch.positions.size() > UINT32_MAX - result.positions.size())
            return {};
        const auto base = uint32_t(result.positions.size());
        result.positions.insert(
            result.positions.end(), patch.positions.begin(), patch.positions.end());
        for (uint32_t index : patch.indices)
            result.indices.push_back(base + index);
    }
    return result;
}

// One-call bake of the full view-independent triangle proxy: six axis views
// -> combined triangle soup. `resolution` is the per-view atlas tile (2..512);
// `maxDepthJump` rejects discontinuities in Reconstruction (in object units).
inline TriangleProxy BakeImpostorProxy(std::span<const glm::vec3> positions,
                                       std::span<const uint32_t> indices,
                                       uint32_t resolution,
                                       float maxDepthJump) {
    const DepthAtlas atlas = BakeDepthAtlas(positions, indices, resolution);
    if (atlas.tileResolution == 0)
        return {};
    return ReconstructAtlas(atlas, maxDepthJump);
}

inline constexpr uint32_t kImpostorProxyVersion = 1;

// Binary round-trip for the proxy (little-endian, bounds-checked, no struct
// padding). The caller owns the returned bytes; the payload is the sibling
// asset the runtime uploads to the position/index heaps for the BLAS.
inline std::vector<uint8_t> WriteImpostorProxy(const TriangleProxy& proxy) {
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    out.insert(out.end(), {'I', 'M', 'P', 'O'});
    put(&kImpostorProxyVersion, 4);
    const uint32_t vc = uint32_t(proxy.positions.size());
    const uint32_t ic = uint32_t(proxy.indices.size());
    put(&vc, 4);
    put(&ic, 4);
    for (const glm::vec3& p : proxy.positions) {
        float xyz[3] = {p.x, p.y, p.z};
        put(xyz, sizeof(xyz));
    }
    for (uint32_t i : proxy.indices)
        put(&i, 4);
    return out;
}

inline bool ParseImpostorProxy(const uint8_t* bytes, size_t size, TriangleProxy& out) {
    out = TriangleProxy{};
    auto read = [&](size_t& at, void* dst, size_t n) {
        if (at > size || size - at < n)
            return false;
        std::memcpy(dst, bytes + at, n);
        at += n;
        return true;
    };
    if (size < 16 || bytes[0] != 'I' || bytes[1] != 'M' || bytes[2] != 'P' || bytes[3] != 'O')
        return false;
    size_t at = 4;
    uint32_t version = 0, vc = 0, ic = 0;
    if (!read(at, &version, 4) || version != kImpostorProxyVersion || !read(at, &vc, 4) ||
        !read(at, &ic, 4))
        return false;
    if (vc > 1'000'000 || ic > 3'000'000 || ic % 3 != 0 || ic > size / 4 || vc > size / 12)
        return false;
    out.positions.resize(vc);
    for (uint32_t i = 0; i < vc; ++i) {
        float xyz[3];
        if (!read(at, xyz, sizeof(xyz)) || !std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) ||
            !std::isfinite(xyz[2]))
            return false;
        out.positions[i] = glm::vec3(xyz[0], xyz[1], xyz[2]);
    }
    out.indices.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        uint32_t idx;
        if (!read(at, &idx, 4) || idx >= vc)
            return false;
        out.indices[i] = idx;
    }
    return at == size;
}

} // namespace tour::impostor
