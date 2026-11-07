// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/sdf.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <span>
#include <tuple>
#include <utility>

#include <glm/geometric.hpp>
#include <meshoptimizer.h>

namespace tour::asset {
namespace {

struct Triangle {
    glm::vec3 a, b, c;
    glm::vec3 lo, hi;
};

bool Finite(const glm::vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

float PointTriangleDistanceSquared(const glm::vec3& p, const Triangle& t) {
    // Closest-point regions from Real-Time Collision Detection.
    const glm::vec3 ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return glm::dot(ap, ap);
    const glm::vec3 bp = p - t.b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return glm::dot(bp, bp);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const glm::vec3 v = ap - (d1 / (d1 - d3)) * ab;
        return glm::dot(v, v);
    }
    const glm::vec3 cp = p - t.c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return glm::dot(cp, cp);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const glm::vec3 v = ap - (d2 / (d2 - d6)) * ac;
        return glm::dot(v, v);
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const glm::vec3 bc = t.c - t.b;
        const glm::vec3 v = bp + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * bc;
        return glm::dot(v, v);
    }
    const glm::vec3 n = glm::cross(ab, ac);
    const float dn = glm::dot(n, n);
    const float d = glm::dot(ap, n);
    return d * d / dn;
}

float AabbDistanceSquared(const glm::vec3& p, const Triangle& t) {
    const glm::vec3 d = glm::max(glm::max(t.lo - p, p - t.hi), glm::vec3(0.0f));
    return glm::dot(d, d);
}

// Parity of a ray along +X; an off-grid Y/Z perturbation avoids most
// edge/vertex ties. Only used for meshes with exactly paired edges.
bool RayHitsX(const glm::vec3& p, const Triangle& t) {
    if (p.y < t.lo.y || p.y > t.hi.y || p.z < t.lo.z || p.z > t.hi.z || t.hi.x <= p.x)
        return false;
    const glm::vec3 ab = t.b - t.a, ac = t.c - t.a;
    const glm::vec3 h(0.0f, -ac.z, ac.y);
    const float det = glm::dot(ab, h);
    if (std::abs(det) <= 1e-12f)
        return false;
    const float inv = 1.0f / det;
    const glm::vec3 s = p - t.a;
    const float u = glm::dot(s, h) * inv;
    if (u < 0.0f || u > 1.0f)
        return false;
    const glm::vec3 q = glm::cross(s, ab);
    const float v = q.x * inv;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    return glm::dot(ac, q) * inv > 0.0f;
}

bool ClosedMesh(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices) {
    // Weld by position for the test only: glTF primitive seams can
    // duplicate vertex IDs with identical coordinates (UV/normal splits).
    std::map<std::tuple<float, float, float>, uint32_t> welded;
    std::vector<uint32_t> ids(vertices.size());
    for (uint32_t i = 0; i < vertices.size(); ++i) {
        const glm::vec3& p = vertices[i];
        auto [it, inserted] =
            welded.emplace(std::make_tuple(p.x, p.y, p.z), uint32_t(welded.size()));
        ids[i] = it->second;
    }
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> edges;
    for (size_t i = 0; i < indices.size(); i += 3) {
        const uint32_t a = ids[indices[i]], b = ids[indices[i + 1]], c = ids[indices[i + 2]];
        if (a == b || b == c || a == c)
            return false;
        for (auto edge : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
            if (edge.first > edge.second)
                std::swap(edge.first, edge.second);
            if (++edges[edge] > 2)
                return false;
        }
    }
    for (const auto& [edge, count] : edges)
        if (count != 2)
            return false;
    return !edges.empty();
}

void PutU32(std::vector<uint8_t>& b, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(value >> (8 * i)));
}
void PutF32(std::vector<uint8_t>& b, float value) {
    PutU32(b, std::bit_cast<uint32_t>(value));
}
bool GetU32(std::span<const uint8_t> b, size_t& at, uint32_t& out) {
    if (at > b.size() || b.size() - at < 4)
        return false;
    out = 0;
    for (int i = 0; i < 4; ++i)
        out |= uint32_t(b[at++]) << (8 * i);
    return true;
}
bool GetF32(std::span<const uint8_t> b, size_t& at, float& out) {
    uint32_t bits;
    if (!GetU32(b, at, bits))
        return false;
    out = std::bit_cast<float>(bits);
    return std::isfinite(out);
}

} // namespace

bool BakeMeshSdf(const glm::vec3* positionData,
                 size_t positionCount,
                 const uint32_t* indexData,
                 size_t indexCount,
                 const SdfBakeOptions& options,
                 SdfVolume& out,
                 std::string& err) {
    err.clear();
    std::span<const glm::vec3> positions(positionData, positionCount);
    std::span<const uint32_t> indices(indexData, indexCount);
    if (options.resolution < 8 || options.resolution > 128 || options.padding < 1 ||
        options.padding * 2 + 2 >= options.resolution || options.bandVoxels < 1 ||
        options.bandVoxels > 128 ||
        (options.voxelRemesh && (options.remeshResolution < 4 || options.remeshResolution > 256))) {
        err = "invalid SDF resolution/padding/band/remesh settings";
        return false;
    }
    if (positions.empty() || indices.empty() || indices.size() % 3 != 0 ||
        indices.size() > 3'000'000 || positions.size() > 1'000'000) {
        err = "SDF requires a nonempty bounded triangle mesh";
        return false;
    }
    for (const glm::vec3& p : positions)
        if (!Finite(p)) {
            err = "SDF has non-finite positions";
            return false;
        }
    for (uint32_t i : indices)
        if (i >= positions.size()) {
            err = "SDF index out of bounds";
            return false;
        }

    std::vector<glm::vec3> remeshed;
    std::vector<uint32_t> remeshedIndices;
    if (options.voxelRemesh) {
        // meshoptimizer 1.3's experimental voxel REMESHER produces an
        // unindexed triangle soup, not an SDF. We still compute signed
        // distances ourselves below. Never remesh implicitly.
        std::vector<float> src;
        src.reserve(positions.size() * 3);
        for (const glm::vec3& p : positions)
            src.insert(src.end(), {p.x, p.y, p.z});
        const size_t cap = meshopt_remesh(nullptr,
                                          0,
                                          indices.data(),
                                          indices.size(),
                                          src.data(),
                                          positions.size(),
                                          3 * sizeof(float),
                                          options.remeshResolution,
                                          meshopt_RemeshSolve);
        if (!cap || cap > 1'000'000) {
            err = "SDF voxel remesher exceeded its triangle budget";
            return false;
        }
        std::vector<float> dst(cap * 9);
        const size_t n = meshopt_remesh(dst.data(),
                                        cap,
                                        indices.data(),
                                        indices.size(),
                                        src.data(),
                                        positions.size(),
                                        3 * sizeof(float),
                                        options.remeshResolution,
                                        meshopt_RemeshSolve);
        if (!n || n > cap) {
            err = "SDF voxel remesher failed";
            return false;
        }
        remeshed.reserve(n * 3);
        remeshedIndices.reserve(n * 3);
        for (size_t i = 0; i < n * 3; ++i) {
            remeshed.emplace_back(dst[i * 3], dst[i * 3 + 1], dst[i * 3 + 2]);
            remeshedIndices.push_back(uint32_t(i));
        }
        positions = remeshed;
        indices = remeshedIndices;
    }
    std::vector<Triangle> triangles;
    triangles.reserve(indices.size() / 3);
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(-std::numeric_limits<float>::max());
    for (size_t i = 0; i < indices.size(); i += 3) {
        const glm::vec3 a = positions[indices[i]], b = positions[indices[i + 1]],
                        c = positions[indices[i + 2]];
        if (glm::dot(glm::cross(b - a, c - a), glm::cross(b - a, c - a)) <= 1e-20f)
            continue; // discarded degenerates never contribute to distance
        Triangle t{a, b, c, glm::min(glm::min(a, b), c), glm::max(glm::max(a, b), c)};
        lo = glm::min(lo, t.lo);
        hi = glm::max(hi, t.hi);
        triangles.push_back(t);
    }
    if (triangles.empty()) {
        err = "SDF contains no nondegenerate triangles";
        return false;
    }
    const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    const float cell = extent / float(options.resolution - 2 * options.padding);
    if (!(cell > 0) || !std::isfinite(cell)) {
        err = "SDF extent is zero or unrepresentable";
        return false;
    }
    const float scale = cell * options.bandVoxels;
    if (!std::isfinite(scale) || scale <= 0) {
        err = "SDF band is unrepresentable";
        return false;
    }
    SdfVolume result;
    result.resolution = options.resolution;
    result.origin = lo - glm::vec3(float(options.padding) - 0.5f) * cell;
    result.voxelSize = cell;
    result.maxDistance = scale;
    // Degenerate triangles invalidate sign classification even if
    // nondegenerate survivors remain: do not claim interior on bad meshes.
    result.signedDistance =
        triangles.size() == indices.size() / 3 && ClosedMesh(positions, indices);
    const size_t r = options.resolution;
    result.samples.reserve(r * r * r);
    const float maxSq = scale * scale;
    for (size_t z = 0; z < r; ++z)
        for (size_t y = 0; y < r; ++y)
            for (size_t x = 0; x < r; ++x) {
                const glm::vec3 p = result.origin + cell * glm::vec3(float(x), float(y), float(z));
                float best = maxSq;
                for (const Triangle& t : triangles) {
                    if (AabbDistanceSquared(p, t) >= best)
                        continue;
                    best = std::min(best, PointTriangleDistanceSquared(p, t));
                }
                float dist = std::sqrt(std::max(0.0f, best));
                if (result.signedDistance) {
                    // Separate ray origin so sign is stable near a shared
                    // vertex/edge, without perturbing the measured distance.
                    const glm::vec3 ray = p + glm::vec3(0.0f, cell * 0.000137f, cell * 0.000293f);
                    unsigned hits = 0;
                    for (const Triangle& t : triangles)
                        hits += RayHitsX(ray, t);
                    if (hits & 1u)
                        dist = -dist;
                }
                result.samples.push_back(
                    int16_t(std::lround(std::clamp(dist / scale, -1.0f, 1.0f) * 32767.0f)));
            }
    out = std::move(result);
    return true;
}

std::vector<uint8_t> WriteSdfAsset(const SdfAsset& asset) {
    std::vector<uint8_t> out{'T', 'S', 'D', 'F'};
    PutU32(out, kSdfVersion);
    PutU32(out, uint32_t(asset.meshes.size()));
    for (const SdfMesh& mesh : asset.meshes) {
        const SdfVolume& v = mesh.volume;
        PutU32(out, uint32_t(mesh.fragment.size()));
        out.insert(out.end(), mesh.fragment.begin(), mesh.fragment.end());
        PutU32(out, v.resolution);
        for (float x : {v.origin.x, v.origin.y, v.origin.z})
            PutF32(out, x);
        PutF32(out, v.voxelSize);
        PutF32(out, v.maxDistance);
        PutU32(out, v.signedDistance ? 1u : 0u);
        PutU32(out, uint32_t(v.samples.size()));
        for (int16_t s : v.samples) {
            const uint16_t u = uint16_t(s);
            out.push_back(uint8_t(u));
            out.push_back(uint8_t(u >> 8));
        }
    }
    return out;
}

bool ParseSdfAsset(const uint8_t* byteData, size_t byteCount, SdfAsset& out, std::string& err) {
    err.clear();
    std::span<const uint8_t> bytes(byteData, byteCount);
    auto fail = [&](const char* why) {
        err = why;
        return false;
    };
    if (bytes.size() < 12 || bytes[0] != 'T' || bytes[1] != 'S' || bytes[2] != 'D' ||
        bytes[3] != 'F')
        return fail("bad SDF magic/header");
    size_t at = 4;
    uint32_t version = 0, n = 0;
    if (!GetU32(bytes, at, version) || version != kSdfVersion)
        return fail("unsupported SDF version");
    if (!GetU32(bytes, at, n) || n > 4096)
        return fail("invalid SDF mesh count");
    SdfAsset parsed;
    parsed.meshes.reserve(n);
    for (uint32_t m = 0; m < n; ++m) {
        uint32_t len = 0, res = 0, sign = 0, count = 0;
        if (!GetU32(bytes, at, len) || len == 0 || len > 4096 || len > bytes.size() - at)
            return fail("invalid SDF fragment");
        SdfMesh mesh;
        mesh.fragment.assign(reinterpret_cast<const char*>(bytes.data() + at), len);
        at += len;
        SdfVolume& v = mesh.volume;
        if (!GetU32(bytes, at, res) || res < 8 || res > 128)
            return fail("invalid SDF resolution");
        v.resolution = res;
        if (!GetF32(bytes, at, v.origin.x) || !GetF32(bytes, at, v.origin.y) ||
            !GetF32(bytes, at, v.origin.z) || !GetF32(bytes, at, v.voxelSize) ||
            !GetF32(bytes, at, v.maxDistance) || v.voxelSize <= 0 || v.maxDistance <= 0)
            return fail("invalid SDF transform/band");
        if (!GetU32(bytes, at, sign) || sign > 1 || !GetU32(bytes, at, count) ||
            count != res * res * res || count > (bytes.size() - at) / 2)
            return fail("invalid SDF sample count/sign");
        v.signedDistance = sign != 0;
        v.samples.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t raw = uint16_t(bytes[at]) | uint16_t(bytes[at + 1]) << 8;
            v.samples.push_back(std::bit_cast<int16_t>(raw));
            at += 2;
        }
        for (const SdfMesh& other : parsed.meshes)
            if (other.fragment == mesh.fragment)
                return fail("duplicate SDF fragment");
        parsed.meshes.push_back(std::move(mesh));
    }
    if (at != bytes.size())
        return fail("trailing SDF bytes");
    out = std::move(parsed);
    return true;
}

} // namespace tour::asset
