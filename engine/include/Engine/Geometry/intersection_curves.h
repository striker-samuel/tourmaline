// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Offline geometry tools shared by the engine and editor. These functions
// never depend on Vulkan, renderer state, or a CGAL installation.
#include <Engine/Placement/placement_domain.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace smvk::geometry {
struct TriangleSoup {
    std::vector<glm::vec3> triangles; // world-space, three vertices per face
};
struct Polyline {
    std::vector<glm::vec3> points;
    bool closed = false;
};
struct CurveSet {
    std::vector<Polyline> curves;
    uint32_t ambiguous = 0;
    uint32_t skipped = 0;
};
struct RibbonVertex {
    glm::vec3 position{};
    glm::vec3 normal{0, 1, 0};
    glm::vec2 uv{};
    float coverage = 0;
};
struct RibbonMesh {
    std::vector<RibbonVertex> vertices;
    std::vector<uint32_t> indices;
};
struct BakeSettings {
    float width = 1.0f;
    float step = 0.25f;
    float lift = 0.015f;
    float weldTolerance = 0.025f;
    float skirtDepth = 0.5f;
    uint32_t maxEdgeSteps = 16384;
};

// OBJ is a simple editor interchange. Vertex positions are read in the
// caller's coordinate system (Y-up, metres); all output is triangulated.
// Supports positive and negative indices, v/vt/vn, and polygon fans.
inline bool LoadObjTriangles(const std::string& file, TriangleSoup& out,
                            std::string& error) {
    std::ifstream in(file);
    if (!in) { error = "Cannot open mesh: " + file; return false; }
    std::vector<glm::vec3> positions;
    TriangleSoup loaded;
    std::string line;
    uint64_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            glm::vec3 p{};
            if (!(ls >> p.x >> p.y >> p.z) ||
                !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                error = "Invalid vertex on line " + std::to_string(lineNo);
                return false;
            }
            positions.push_back(p);
        } else if (tag == "f") {
            std::vector<size_t> face;
            std::string entry;
            while (ls >> entry) {
                if (entry[0] == '#') break;
                const auto slash = entry.find('/');
                const std::string idx = entry.substr(0, slash);
                int parsed = 0;
                try {
                    size_t used = 0;
                    parsed = std::stoi(idx, &used);
                    if (used != idx.size() || parsed == 0)
                        throw std::invalid_argument("bad index");
                } catch (...) {
                    error = "Invalid face index on line " + std::to_string(lineNo);
                    return false;
                }
                const int64_t index = parsed > 0
                    ? int64_t(parsed) - 1 : int64_t(positions.size()) + parsed;
                if (index < 0 || index >= int64_t(positions.size())) {
                    error = "Face index outside vertex range on line " +
                        std::to_string(lineNo);
                    return false;
                }
                face.push_back(size_t(index));
            }
            for (size_t j = 2; j < face.size(); ++j) {
                loaded.triangles.push_back(positions[face[0]]);
                loaded.triangles.push_back(positions[face[j - 1]]);
                loaded.triangles.push_back(positions[face[j]]);
            }
        }
    }
    if (loaded.triangles.empty()) {
        error = "OBJ has no triangle faces: " + file;
        return false;
    }
    out = std::move(loaded);
    return true;
}

namespace detail {
inline void AddUnique(std::vector<glm::vec3>& out, glm::vec3 p, float tol) {
    for (auto& q : out)
        if (glm::dot(p - q, p - q) <= tol * tol) return;
    out.push_back(p);
}
inline float SignedHeight(const ::tour::placement::HeightfieldData& f,
                          glm::vec3 p) {
    return p.y - f.sampleHeight(p.x, p.z);
}
inline void EdgeTerrainContacts(const ::tour::placement::HeightfieldData& f,
    glm::vec3 a, glm::vec3 b, const BakeSettings& opt,
    std::vector<glm::vec3>& points) {
    const float len = glm::length(glm::vec2(b.x - a.x, b.z - a.z));
    const uint32_t n = std::clamp(uint32_t(std::ceil(len / std::max(opt.step, .001f))),
        1u, std::max(opt.maxEdgeSteps, 1u));
    // Reject tangencies except at exact endpoints: they do not form
    // ordinary crossing segments, and need a separate topology policy.
    const float epsilon = 1e-5f;
    float prev = SignedHeight(f, a);
    if (std::abs(prev) <= epsilon) AddUnique(points, a, opt.weldTolerance);
    for (uint32_t j = 1; j <= n; ++j) {
        float t = float(j) / float(n);
        glm::vec3 p = a + (b - a) * t;
        const float d = SignedHeight(f, p);
        if (std::abs(d) <= epsilon) AddUnique(points, p, opt.weldTolerance);
        if ((prev < -epsilon && d > epsilon) ||
            (prev > epsilon && d < -epsilon)) {
            float lo = float(j - 1) / float(n), hi = t;
            const bool positiveLo = prev > 0;
            for (int k = 0; k < 24; ++k) {
                const float m = (lo + hi) * .5f;
                if ((SignedHeight(f, a + (b - a) * m) > 0) == positiveLo)
                    lo = m;
                else hi = m;
            }
            AddUnique(points, a + (b - a) * ((lo + hi) * .5f),
                      opt.weldTolerance);
        }
        prev = d;
    }
}
using Key = std::array<int64_t, 3>;
inline Key Quantized(glm::vec3 v, float tol) {
    return { int64_t(std::llround(double(v.x) / tol)),
             int64_t(std::llround(double(v.y) / tol)),
             int64_t(std::llround(double(v.z) / tol)) };
}
} // namespace detail

// Endpoint snapping + graph tracing. Distinct simple paths are preserved;
// degree >2 junctions become curve endpoints, not arbitrary angle joins.
// Deterministic sorted keys permit stable recook output.
inline CurveSet StitchSegments(
    const std::vector<std::pair<glm::vec3, glm::vec3>>& segments,
    float weldTolerance = .025f) {
    CurveSet result;
    if (!(weldTolerance > 0)) return result;
    struct Vertex { glm::vec3 sum{}; uint32_t count = 0; };
    std::map<detail::Key, uint32_t> ids;
    std::vector<Vertex> verts;
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    auto intern = [&](glm::vec3 p) -> uint32_t {
        auto key = detail::Quantized(p, weldTolerance);
        auto it = ids.find(key);
        if (it != ids.end()) {
            auto& v = verts[it->second]; v.sum += p; ++v.count;
            return it->second;
        }
        uint32_t id = uint32_t(verts.size());
        ids.emplace(key, id);
        verts.push_back({p, 1});
        return id;
    };
    for (auto [a,b] : segments) {
        const uint32_t x = intern(a), y = intern(b);
        if (x == y) { ++result.skipped; continue; }
        if (x > y) edges.emplace_back(y,x);
        else edges.emplace_back(x,y);
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<std::vector<uint32_t>> adjacency(verts.size());
    for (uint32_t i = 0; i < edges.size(); ++i) {
        adjacency[edges[i].first].push_back(i);
        adjacency[edges[i].second].push_back(i);
    }
    std::vector<bool> visited(edges.size(), false);
    auto follow = [&](uint32_t start, uint32_t firstEdge) {
        Polyline line;
        uint32_t at = start, edge = firstEdge;
        line.points.push_back(verts[at].sum / float(verts[at].count));
        while (!visited[edge]) {
            visited[edge] = true;
            const auto [a,b] = edges[edge];
            at = (at == a) ? b : a;
            line.points.push_back(verts[at].sum / float(verts[at].count));
            if (at == start) {
                line.closed = true;
                line.points.pop_back();
                break;
            }
            if (adjacency[at].size() != 2) break;
            const uint32_t next = adjacency[at][0] == edge
                ? adjacency[at][1] : adjacency[at][0];
            edge = next;
        }
        if (line.points.size() > 1) result.curves.push_back(std::move(line));
    };
    for (uint32_t v = 0; v < adjacency.size(); ++v)
        if (adjacency[v].size() != 2)
            for (auto e : adjacency[v])
                if (!visited[e]) follow(v,e);
    for (uint32_t e = 0; e < edges.size(); ++e)
        if (!visited[e]) follow(edges[e].first,e);
    return result;
}

inline CurveSet ExtractTerrainIntersection(
    const ::tour::placement::HeightfieldData& f, const TriangleSoup& mesh,
    const BakeSettings& opt = {}) {
    CurveSet result;
    if (!f.valid() || opt.step <= 0 || opt.weldTolerance <= 0) return result;
    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    for (size_t i = 0; i + 2 < mesh.triangles.size(); i += 3) {
        const glm::vec3* t = mesh.triangles.data() + i;
        std::vector<glm::vec3> p;
        detail::EdgeTerrainContacts(f,t[0],t[1],opt,p);
        detail::EdgeTerrainContacts(f,t[1],t[2],opt,p);
        detail::EdgeTerrainContacts(f,t[2],t[0],opt,p);
        if (p.size() == 2) {
            segments.emplace_back(p[0],p[1]);
        } else if (!p.empty()) {
            ++result.ambiguous;
        }
    }
    CurveSet stitched = StitchSegments(segments,opt.weldTolerance);
    stitched.ambiguous += result.ambiguous;
    return stitched;
}

// Optional bounded corner-cutting of extracted polylines. This operates
// on the derivative bake curve only; exact CGAL/heightfield source curves
// remain available for diagnostics and re-baking. "strength" is capped so
// two adjacent new points remain close to their original segment.
inline Polyline SmoothPolyline(const Polyline& input, int passes = 1,
                                float strength = .2f) {
    Polyline result = input;
    strength = std::clamp(strength,0.0f,.25f);
    passes = std::clamp(passes,0,4);
    for (int pass=0;pass<passes && strength>0;++pass) {
        const size_t n=result.points.size();
        if (n<3 || n>200000) break;
        Polyline next; next.closed=result.closed;
        if (!next.closed) next.points.push_back(result.points.front());
        const size_t edges=next.closed ? n : n-1;
        for (size_t i=0;i<edges;++i) {
            const glm::vec3 a=result.points[i];
            const glm::vec3 b=result.points[(i+1)%n];
            next.points.push_back(a*(1-strength)+b*strength);
            next.points.push_back(a*strength+b*(1-strength));
        }
        if (!next.closed) next.points.push_back(result.points.back());
        result=std::move(next);
    }
    return result;
}

// Polyline resampling preserves original endpoints and does NOT smooth
// intersections away from the actual source meshes.
inline Polyline Resample(const Polyline& c, float maxStep) {
    Polyline out;
    if (c.points.size() < 2 || maxStep <= 0) return c;
    out.closed = c.closed;
    const size_t edges = c.closed ? c.points.size() : c.points.size() - 1;
    for (size_t i = 0; i < edges; ++i) {
        const glm::vec3 a = c.points[i];
        const glm::vec3 b = c.points[(i + 1) % c.points.size()];
        const uint32_t steps = std::max(1u,
            uint32_t(std::ceil(glm::length(b - a) / maxStep)));
        for (uint32_t j = 0; j < steps; ++j)
            out.points.push_back(a + (b - a) * (float(j) / float(steps)));
    }
    if (!c.closed) out.points.push_back(c.points.back());
    return out;
}

inline RibbonMesh BakeTerrainRibbon(const CurveSet& curves,
    const ::tour::placement::HeightfieldData& terrain,
    const BakeSettings& opt = {}) {
    RibbonMesh out;
    if (!terrain.valid() || opt.width <= 0 || opt.step <= 0) return out;
    for (const auto& raw : curves.curves) {
        auto c = Resample(raw,opt.step);
        const size_t n = c.points.size();
        if (n < 2) continue;
        const uint32_t base = uint32_t(out.vertices.size());
        float u = 0;
        for (size_t i = 0; i < n; ++i) {
            glm::vec3 before = c.points[(i == 0) ? (c.closed ? n-1 : 0) : i-1];
            glm::vec3 after = c.points[(i+1<n) ? i+1 : (c.closed ? 0 : n-1)];
            glm::vec2 tangent(after.x-before.x,after.z-before.z);
            const float l = glm::length(tangent);
            if (l > 1e-6f) tangent /= l;
            else tangent = glm::vec2(1,0);
            const glm::vec2 side(-tangent.y,tangent.x);
            if (i > 0) u += glm::length(c.points[i]-c.points[i-1]);
            for (int j = 0; j < 3; ++j) {
                const float v = float(j) * .5f;
                glm::vec2 xz(c.points[i].x,c.points[i].z);
                xz += side * ((v-.5f)*opt.width);
                out.vertices.push_back({
                    glm::vec3(xz.x,terrain.sampleHeight(xz.x,xz.y)+opt.lift,xz.y),
                    terrain.sampleNormal(xz.x,xz.y),
                    glm::vec2(u,v),j==1 ? 1.0f : 0.0f
                });
            }
        }
        const size_t count = c.closed ? n : n-1;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t a = base + uint32_t(i)*3;
            const uint32_t b = base + uint32_t((i+1)%n)*3;
            for (uint32_t j = 0; j < 2; ++j)
                out.indices.insert(out.indices.end(), {
                    a+j,a+j+1,b+j,b+j,a+j+1,b+j+1
                });
        }
    }
    return out;
}

// A vertical skirt is useful for hole edges, foundations and hiding seams.
// Normals face laterally; no heightfield or deferred buffer required.
inline RibbonMesh BakeVerticalSkirt(const CurveSet& curves,
                                    const BakeSettings& opt = {}) {
    RibbonMesh out;
    if (opt.skirtDepth <= 0 || opt.step <= 0) return out;
    for (const auto& raw : curves.curves) {
        const Polyline c = Resample(raw,opt.step);
        if (c.points.size() < 2) continue;
        const uint32_t base = uint32_t(out.vertices.size());
        float u = 0;
        const size_t n = c.points.size();
        for (size_t i = 0; i < n; ++i) {
            if (i) u += glm::length(c.points[i]-c.points[i-1]);
            glm::vec3 prev = c.points[i ? i-1 : (c.closed ? n-1 : 0)];
            glm::vec3 next = c.points[i+1<n ? i+1 : (c.closed ? 0 : n-1)];
            glm::vec3 t = next-prev;
            glm::vec3 normal = glm::cross(t,glm::vec3(0,1,0));
            float mag = glm::length(normal);
            normal = mag > 1e-6f ? normal/mag : glm::vec3(1,0,0);
            out.vertices.push_back({c.points[i],normal,glm::vec2(u,0),1});
            out.vertices.push_back({c.points[i]-glm::vec3(0,opt.skirtDepth,0),
                normal,glm::vec2(u,1),1});
        }
        for (size_t i = 0; i < (c.closed ? n : n-1); ++i) {
            uint32_t a = base + uint32_t(i)*2;
            uint32_t b = base + uint32_t((i+1)%n)*2;
            out.indices.insert(out.indices.end(),{a,a+1,b,b,a+1,b+1});
        }
    }
    return out;
}

inline bool SaveRibbonObj(const std::string& path, const RibbonMesh& mesh,
                          std::string& error) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = "No ribbon geometry to save"; return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "Cannot create output: " + path; return false; }
    out << "# Tourmaline offline seam geometry. Vertex color grayscale is coverage.\n";
    out << std::setprecision(9);
    for (const auto& v : mesh.vertices)
        out << "v " << v.position.x << ' ' << v.position.y << ' '
            << v.position.z << ' ' << v.coverage << ' ' << v.coverage
            << ' ' << v.coverage << '\n';
    for (const auto& v : mesh.vertices)
        out << "vt " << v.uv.x << ' ' << v.uv.y << '\n';
    for (const auto& v : mesh.vertices)
        out << "vn " << v.normal.x << ' ' << v.normal.y << ' '
            << v.normal.z << '\n';
    for (size_t i = 0; i+2 < mesh.indices.size(); i+=3) {
        out << "f";
        for (int j=0;j<3;++j) {
            const uint32_t v = mesh.indices[i+j] + 1;
            out << ' ' << v << '/' << v << '/' << v;
        }
        out << '\n';
    }
    if (!out.good()) { error = "Failed writing OBJ: " + path; return false; }
    return true;
}

inline bool SaveCurvesObj(const std::string& path, const CurveSet& curves,
                          std::string& error) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if (!out) { error = "Cannot create polyline OBJ: " + path; return false; }
    uint64_t offset=1;
    out << std::setprecision(9);
    for (const auto& c : curves.curves) {
        if (c.points.size()<2) continue;
        for (const auto& p : c.points)
            out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
        out << "l";
        for (size_t i=0;i<c.points.size();++i) out << ' ' << offset+i;
        if (c.closed) out << ' ' << offset;
        out << '\n';
        offset += c.points.size();
    }
    if (!out.good()) { error = "Failed writing polyline OBJ"; return false; }
    return true;
}
} // namespace smvk::geometry
