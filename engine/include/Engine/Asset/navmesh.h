// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// CPU-only 2.5D navigation surface. XZ is the walk plane; Y is elevation.
// An authored .navmesh is a precomputed triangle mesh, not a geometry baker.
// WorldPartition can stream whole meshes/tiles through its provider seam.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tour::asset {

struct NavPoint {
    double x = 0.0, y = 0.0, z = 0.0;
};
struct NavTriangle {
    std::array<uint32_t, 3> vertices{};
    double cost = 1.0; // positive traversal multiplier
    std::vector<uint32_t> neighbors;
};
struct NavMeshAsset {
    std::vector<NavPoint> vertices;
    std::vector<NavTriangle> triangles;
};

namespace nav_detail {
inline bool Number(const std::string& s, double& out) {
    std::istringstream in(s);
    in >> out;
    return bool(in) && in.eof() && std::isfinite(out);
}
inline bool Index(const std::string& s, uint32_t& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
inline double Area2(NavPoint a, NavPoint b, NavPoint c) {
    return (b.x - a.x) * (c.z - a.z)
        - (b.z - a.z) * (c.x - a.x);
}
inline NavPoint Center(const NavMeshAsset& mesh, const NavTriangle& t) {
    const auto& a = mesh.vertices[t.vertices[0]];
    const auto& b = mesh.vertices[t.vertices[1]];
    const auto& c = mesh.vertices[t.vertices[2]];
    return {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3,
        (a.z + b.z + c.z) / 3};
}
inline double DistXZ(NavPoint a, NavPoint b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}
inline std::optional<double> HeightAt(NavPoint a, NavPoint b,
    NavPoint c, NavPoint p) {
    const double denom = Area2(a, b, c);
    if (std::abs(denom) <= 1e-12) return std::nullopt;
    const double u = Area2(p, b, c) / denom;
    const double v = Area2(a, p, c) / denom;
    const double w = 1.0 - u - v;
    constexpr double tolerance = 1e-8;
    if (u < -tolerance || v < -tolerance || w < -tolerance)
        return std::nullopt;
    return u * a.y + v * b.y + w * c.y;
}
} // namespace nav_detail

// Grammar (independent of .tour and external navigation libraries):
//   navmesh 1
//   vertex 0 0 0
//   vertex 10 0 0
//   vertex 0 0 10
//   triangle 0 1 2 1.0    # optional traversal cost, default 1
//
// Input must be PREBAKED walkable triangles. Reject non-manifold edges,
// invalid/degenerate XZ polygons, non-finite positions, invalid indices,
// and malformed syntax. No side effects if parsing fails.
inline bool ParseNavMeshText(std::string_view source, NavMeshAsset& out,
    std::string& err) {
    err.clear();
    NavMeshAsset parsed;
    bool version = false;
    uint32_t lineNumber = 0;
    std::istringstream lines{std::string(source)};
    std::string line;
    auto fail = [&](const std::string& message) {
        err = "navmesh line " + std::to_string(lineNumber) + ": " + message;
        return false;
    };
    while (std::getline(lines, line)) {
        ++lineNumber;
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream fields(line);
        std::vector<std::string> tok;
        for (std::string t; fields >> t;) tok.push_back(std::move(t));
        if (tok.empty()) continue;
        if (!version) {
            if (tok.size() != 2 || tok[0] != "navmesh" || tok[1] != "1")
                return fail("expected navmesh 1");
            version = true;
        } else if (tok[0] == "vertex") {
            if (tok.size() != 4 || parsed.vertices.size() >= 1000000)
                return fail("vertex requires x y z (max 1000000 vertices)");
            NavPoint v;
            if (!nav_detail::Number(tok[1], v.x)
                || !nav_detail::Number(tok[2], v.y)
                || !nav_detail::Number(tok[3], v.z))
                return fail("vertex coordinates must be finite");
            parsed.vertices.push_back(v);
        } else if (tok[0] == "triangle") {
            if ((tok.size() != 4 && tok.size() != 5)
                || parsed.triangles.size() >= 1000000)
                return fail("triangle requires i j k [cost]");
            NavTriangle tri;
            for (size_t i = 0; i < 3; ++i)
                if (!nav_detail::Index(tok[i + 1], tri.vertices[i]))
                    return fail("triangle indices must be uint32");
            if (tok.size() == 5 && !nav_detail::Number(tok[4], tri.cost))
                return fail("invalid traversal cost");
            if (tri.cost <= 0) return fail("traversal cost must be positive");
            parsed.triangles.push_back(std::move(tri));
        } else return fail("unknown directive: " + tok[0]);
    }
    if (!version || parsed.triangles.empty())
        return fail("missing version or triangles");
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
    for (uint32_t ti = 0; ti < parsed.triangles.size(); ++ti) {
        const auto& tri = parsed.triangles[ti];
        const auto& v = tri.vertices;
        if (v[0] >= parsed.vertices.size()
            || v[1] >= parsed.vertices.size()
            || v[2] >= parsed.vertices.size()
            || v[0] == v[1] || v[1] == v[2] || v[2] == v[0])
            return fail("out-of-range or duplicate vertex index");
        const double area = nav_detail::Area2(parsed.vertices[v[0]],
            parsed.vertices[v[1]], parsed.vertices[v[2]]);
        if (!std::isfinite(area) || std::abs(area) <= 1e-10)
            return fail("triangle is degenerate in XZ");
        for (size_t i = 0; i < 3; ++i) {
            auto key = std::minmax(v[i], v[(i + 1) % 3]);
            auto& owners = edges[{key.first, key.second}];
            owners.push_back(ti);
            if (owners.size() > 2)
                return fail("non-manifold edge used by more than two triangles");
        }
    }
    for (const auto& [edge, owners] : edges) {
        (void)edge;
        if (owners.size() != 2) continue;
        if (owners[0] == owners[1])
            return fail("duplicate triangle edge");
        parsed.triangles[owners[0]].neighbors.push_back(owners[1]);
        parsed.triangles[owners[1]].neighbors.push_back(owners[0]);
    }
    for (auto& t : parsed.triangles) {
        std::sort(t.neighbors.begin(), t.neighbors.end());
        t.neighbors.erase(std::unique(t.neighbors.begin(), t.neighbors.end()),
            t.neighbors.end());
    }
    out = std::move(parsed);
    return true;
}

// Select the surface nearest the caller's height when floors overlap in XZ.
// A point outside every triangle returns nullopt; callers decide snapping.
inline std::optional<uint32_t> LocateNavTriangle(const NavMeshAsset& mesh,
    NavPoint position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y)
        || !std::isfinite(position.z))
        return std::nullopt;
    std::optional<uint32_t> found;
    double best = std::numeric_limits<double>::infinity();
    for (uint32_t i = 0; i < mesh.triangles.size(); ++i) {
        const auto& t = mesh.triangles[i].vertices;
        auto height = nav_detail::HeightAt(mesh.vertices[t[0]],
            mesh.vertices[t[1]], mesh.vertices[t[2]], position);
        if (!height) continue;
        const double delta = std::abs(*height - position.y);
        if (delta < best) { best = delta; found = i; }
    }
    return found;
}

// Dijkstra over triangle adjacency with positive per-face costs.
// Returns a VALID (not smoothed) corridor through triangle centroids.
// The application owns steering, avoidance, agents, and mesh lifetime.
// No path exists for disconnected triangles or off-mesh endpoints.
inline bool FindNavPath(const NavMeshAsset& mesh, NavPoint start,
    NavPoint goal, std::vector<NavPoint>& out) {
    const auto first = LocateNavTriangle(mesh, start);
    const auto last = LocateNavTriangle(mesh, goal);
    if (!first || !last) return false;
    if (*first == *last) {
        out = {start, goal};
        return true;
    }
    const size_t count = mesh.triangles.size();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> best(count, inf);
    std::vector<uint32_t> parent(count, UINT32_MAX);
    using QueueEntry = std::pair<double, uint32_t>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>,
        std::greater<QueueEntry>> queue;
    best[*first] = 0;
    queue.push({0, *first});
    while (!queue.empty()) {
        auto [cost, current] = queue.top();
        queue.pop();
        if (cost > best[current]) continue;
        if (current == *last) break;
        const NavPoint a = nav_detail::Center(mesh, mesh.triangles[current]);
        for (uint32_t next : mesh.triangles[current].neighbors) {
            const NavPoint b = nav_detail::Center(mesh, mesh.triangles[next]);
            const double step = nav_detail::DistXZ(a, b)
                * (mesh.triangles[current].cost
                    + mesh.triangles[next].cost) * 0.5;
            const double alternative = cost + step;
            if (alternative >= best[next]) continue;
            best[next] = alternative;
            parent[next] = current;
            queue.push({alternative, next});
        }
    }
    if (!std::isfinite(best[*last])) return false;
    std::vector<uint32_t> chain;
    for (uint32_t i = *last; i != *first; i = parent[i])
        chain.push_back(i);
    chain.push_back(*first);
    std::reverse(chain.begin(), chain.end());
    std::vector<NavPoint> result;
    result.reserve(chain.size() + 2);
    result.push_back(start);
    for (size_t i = 1; i + 1 < chain.size(); ++i)
        result.push_back(nav_detail::Center(mesh, mesh.triangles[chain[i]]));
    // Include the first and last triangle centers for correct corridor
    // traversal: start/end can lie near opposite corners of a tile.
    if (chain.size() > 1) {
        result.insert(result.begin() + 1,
            nav_detail::Center(mesh, mesh.triangles[*first]));
        result.push_back(nav_detail::Center(mesh, mesh.triangles[*last]));
    }
    result.push_back(goal);
    out = std::move(result);
    return true;
}

} // namespace tour::asset
