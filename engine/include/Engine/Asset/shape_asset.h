// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// CPU-only authored shape assets. This is a source representation, not a
// .geometry file or a new cooked-pak kind. Call TessellateShape to obtain
// positions/indices for an importer or editor preview.
#include <algorithm>
#include <array>
#include <utility>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace tour::asset {

struct ShapePoint {
    float x = 0, y = 0, z = 0;
};

enum class ShapeKind { Spline, PointCloud, Plane, Box, Sphere, Cylinder, Cone };

struct ShapeAsset {
    ShapeKind kind = ShapeKind::Spline;
    std::vector<ShapePoint> points;
    bool closed = false;             // spline only
    uint32_t segments = 16;          // spline samples per span / radial segments
    float radius = 1.0f;             // sphere / cylinder / cone
    float height = 2.0f;             // cylinder / cone
};

struct ShapeGeometry {
    enum class Topology { Lines, Points, Triangles };
    Topology topology = Topology::Triangles;
    std::vector<ShapePoint> vertices;
    std::vector<uint32_t> indices;
};

inline const char* ShapeKindName(ShapeKind kind) {
    switch (kind) {
    case ShapeKind::Spline: return "spline";
    case ShapeKind::PointCloud: return "point_cloud";
    case ShapeKind::Plane: return "plane";
    case ShapeKind::Box: return "box";
    case ShapeKind::Sphere: return "sphere";
    case ShapeKind::Cylinder: return "cylinder";
    case ShapeKind::Cone: return "cone";
    }
    return "";
}

inline bool ParseShapeKind(const std::string& value, ShapeKind& kind) {
    for (ShapeKind candidate : {ShapeKind::Spline, ShapeKind::PointCloud,
         ShapeKind::Plane, ShapeKind::Box, ShapeKind::Sphere,
         ShapeKind::Cylinder, ShapeKind::Cone}) {
        if (value == ShapeKindName(candidate)) {
            kind = candidate;
            return true;
        }
    }
    return false;
}

inline bool ValidShape(const ShapeAsset& shape, std::string& error) {
    if (shape.segments < 1 || shape.segments > 256) {
        error = "segments must be between 1 and 256";
        return false;
    }
    if (!std::isfinite(shape.radius) || shape.radius <= 0 ||
        !std::isfinite(shape.height) || shape.height <= 0) {
        error = "radius and height must be positive finite values";
        return false;
    }
    if (shape.points.size() > 100000) {
        error = "too many control points";
        return false;
    }
    for (const auto& p : shape.points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            !std::isfinite(p.z)) {
            error = "non-finite point coordinate";
            return false;
        }
    }
    if (shape.kind == ShapeKind::Spline &&
        shape.points.size() < (shape.closed ? 3u : 2u)) {
        error = "spline needs at least 2 points (3 when closed)";
        return false;
    }
    if (shape.kind == ShapeKind::PointCloud && shape.points.empty()) {
        error = "point cloud needs at least one point";
        return false;
    }
    if ((shape.kind == ShapeKind::Sphere || shape.kind == ShapeKind::Cylinder ||
         shape.kind == ShapeKind::Cone) && shape.segments < 3) {
        error = "round primitives need at least 3 segments";
        return false;
    }
    return true;
}

// Strict, versioned, line-based authoring format; a bad row never partially
// replaces the caller's document. Unknown keys are rejected (not ignored).
inline bool ParseShapeAsset(const std::string& source, ShapeAsset& out,
                           std::string& error) {
    ShapeAsset parsed;
    std::istringstream input(source);
    std::string line;
    bool version = false, kind = false;
    size_t lineNo = 0;
    while (std::getline(input, line)) {
        ++lineNo;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream row(line);
        std::string key;
        if (!(row >> key)) continue;
        std::string extra;
        bool ok = true;
        if (key == "shape") {
            int v = 0;
            ok = !version && bool(row >> v) && v == 1;
            version = true;
        } else if (key == "kind") {
            std::string name;
            ok = !kind && bool(row >> name) && ParseShapeKind(name, parsed.kind);
            kind = true;
        } else if (key == "closed") {
            int v = -1;
            ok = bool(row >> v) && (v == 0 || v == 1);
            if (ok) parsed.closed = v == 1;
        } else if (key == "segments") {
            unsigned int v = 0;
            ok = bool(row >> v) && v >= 1 && v <= 256;
            if (ok) parsed.segments = v;
        } else if (key == "radius") {
            ok = bool(row >> parsed.radius);
        } else if (key == "height") {
            ok = bool(row >> parsed.height);
        } else if (key == "point") {
            ShapePoint p;
            ok = bool(row >> p.x >> p.y >> p.z) &&
                 parsed.points.size() < 100000;
            if (ok) parsed.points.push_back(p);
        } else {
            ok = false;
        }
        if (!ok || (row >> extra)) {
            error = "invalid shape row at line " + std::to_string(lineNo);
            return false;
        }
    }
    if (!version || !kind) {
        error = "missing shape version or kind";
        return false;
    }
    if (!ValidShape(parsed, error)) return false;
    out = std::move(parsed);
    error.clear();
    return true;
}

inline std::string SerializeShapeAsset(const ShapeAsset& shape) {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<float>::max_digits10)
        << "shape 1\nkind " << ShapeKindName(shape.kind)
        << "\nclosed " << (shape.closed ? 1 : 0)
        << "\nsegments " << shape.segments
        << "\nradius " << shape.radius
        << "\nheight " << shape.height << '\n';
    for (const auto& p : shape.points)
        out << "point " << p.x << ' ' << p.y << ' ' << p.z << '\n';
    return out.str();
}

inline ShapeAsset DefaultShapeAsset(ShapeKind kind) {
    ShapeAsset result;
    result.kind = kind;
    if (kind == ShapeKind::Spline)
        result.points = {{-1, 0, 0}, {0, 0, 1}, {1, 0, 0}};
    if (kind == ShapeKind::PointCloud)
        result.points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    return result;
}

inline ShapePoint ShapeLerp(const ShapePoint& a, const ShapePoint& b, float t) {
    return {a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t};
}

// Catmull-Rom with endpoint duplication for open curves. An explicit
// final segment closes loops; sampling never duplicates its first vertex.
inline ShapePoint SampleSpline(const ShapeAsset& shape, size_t span, float t) {
    const size_t n = shape.points.size();
    auto get = [&](long long i) -> const ShapePoint& {
        if (shape.closed) {
            const long long count = static_cast<long long>(n);
            return shape.points[static_cast<size_t>((i % count + count) % count)];
        }
        return shape.points[static_cast<size_t>(std::clamp<long long>(
            i, 0, static_cast<long long>(n) - 1))];
    };
    const auto& a = get(static_cast<long long>(span)-1);
    const auto& b = get(static_cast<long long>(span));
    const auto& c = get(static_cast<long long>(span)+1);
    const auto& d = get(static_cast<long long>(span)+2);
    auto coord = [t](float p0, float p1, float p2, float p3) {
        return 0.5f * ((2*p1) + (-p0+p2)*t +
            (2*p0-5*p1+4*p2-p3)*t*t +
            (-p0+3*p1-3*p2+p3)*t*t*t);
    };
    return {coord(a.x,b.x,c.x,d.x), coord(a.y,b.y,c.y,d.y),
            coord(a.z,b.z,c.z,d.z)};
}

inline bool TessellateShape(const ShapeAsset& shape, ShapeGeometry& out,
                            std::string& error) {
    if (!ValidShape(shape, error)) return false;
    ShapeGeometry mesh;
    auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        mesh.indices.insert(mesh.indices.end(), {a,b,c});
    };
    switch (shape.kind) {
    case ShapeKind::PointCloud:
        mesh.topology = ShapeGeometry::Topology::Points;
        mesh.vertices = shape.points;
        break;
    case ShapeKind::Spline: {
        mesh.topology = ShapeGeometry::Topology::Lines;
        const size_t spans = shape.points.size() - (shape.closed ? 0u : 1u);
        for (size_t i = 0; i < spans; ++i)
            for (uint32_t j = 0; j < shape.segments; ++j)
                mesh.vertices.push_back(SampleSpline(shape, i,
                    float(j)/float(shape.segments)));
        if (!shape.closed) mesh.vertices.push_back(shape.points.back());
        for (uint32_t i = 1; i < mesh.vertices.size(); ++i)
            mesh.indices.insert(mesh.indices.end(), {i-1, i});
        if (shape.closed)
            mesh.indices.insert(mesh.indices.end(),
                {uint32_t(mesh.vertices.size()-1), 0u});
        break;
    }
    case ShapeKind::Plane:
        mesh.vertices = {{-1,0,-1},{1,0,-1},{1,0,1},{-1,0,1}};
        triangle(0,2,1); triangle(0,3,2);
        break;
    case ShapeKind::Box: {
        mesh.vertices = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                         {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
        for (const auto& face : {std::array<uint32_t,4>{0,3,2,1},
             {4,5,6,7},{0,1,5,4},{3,7,6,2},{0,4,7,3},{1,2,6,5}}) {
            triangle(face[0],face[1],face[2]);
            triangle(face[0],face[2],face[3]);
        }
        break;
    }
    case ShapeKind::Sphere: {
        const uint32_t rings = shape.segments / 2 + 1;
        constexpr float pi = 3.14159265358979323846f;
        for (uint32_t y = 0; y <= rings; ++y) {
            const float theta = pi*float(y)/float(rings);
            for (uint32_t x = 0; x < shape.segments; ++x) {
                const float phi = 2*pi*float(x)/float(shape.segments);
                mesh.vertices.push_back({shape.radius*std::sin(theta)*std::cos(phi),
                    shape.radius*std::cos(theta),
                    shape.radius*std::sin(theta)*std::sin(phi)});
            }
        }
        for (uint32_t y = 0; y < rings; ++y)
            for (uint32_t x = 0; x < shape.segments; ++x) {
                const uint32_t a = y*shape.segments+x;
                const uint32_t b = y*shape.segments+(x+1)%shape.segments;
                triangle(a,b,a+shape.segments);
                triangle(b,b+shape.segments,a+shape.segments);
            }
        break;
    }
    case ShapeKind::Cylinder:
    case ShapeKind::Cone: {
        constexpr float pi = 3.14159265358979323846f;
        const uint32_t n = shape.segments;
        const bool cone = shape.kind == ShapeKind::Cone;
        for (uint32_t i=0;i<n;++i) {
            const float angle = 2*pi*float(i)/float(n);
            const float x = shape.radius*std::cos(angle);
            const float z = shape.radius*std::sin(angle);
            mesh.vertices.push_back({x,-shape.height/2,z});
            mesh.vertices.push_back(cone ? ShapePoint{0,shape.height/2,0}
                                         : ShapePoint{x,shape.height/2,z});
        }
        const uint32_t bottom = uint32_t(mesh.vertices.size());
        mesh.vertices.push_back({0,-shape.height/2,0});
        const uint32_t top = uint32_t(mesh.vertices.size());
        if (!cone) mesh.vertices.push_back({0,shape.height/2,0});
        for (uint32_t i=0;i<n;++i) {
            const uint32_t next=(i+1)%n;
            triangle(bottom,2*next,2*i);
            triangle(2*i,2*next,2*i+1);
            if (!cone) {
                triangle(2*i+1,2*next,2*next+1);
                triangle(top,2*i+1,2*next+1);
            }
        }
        break;
    }
    }
    out = std::move(mesh);
    error.clear();
    return true;
}
// Export triangle primitives as a standard Wavefront OBJ text mesh so the
// existing OBJ source-import path can consume procedural geometry without
// changing .geometry, meshlets or pak formats. Point/line topology is
// deliberately rejected: the mesh importer is triangle-oriented.
inline bool ShapeToObj(const ShapeAsset& shape, std::string& output,
                       std::string& error) {
    ShapeGeometry geometry;
    if (!TessellateShape(shape, geometry, error)) return false;
    if (geometry.topology != ShapeGeometry::Topology::Triangles) {
        error = "OBJ mesh export supports triangle primitives only";
        return false;
    }
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10);
    stream << "# Tourmaline procedural shape: " << ShapeKindName(shape.kind) << '\n';
    stream << "o procedural_" << ShapeKindName(shape.kind) << '\n';
    for (const auto& p : geometry.vertices)
        stream << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
    for (size_t i = 0; i + 2 < geometry.indices.size(); i += 3)
        stream << "f " << geometry.indices[i] + 1 << ' '
               << geometry.indices[i + 1] + 1 << ' '
               << geometry.indices[i + 2] + 1 << '\n';
    output = stream.str();
    error.clear();
    return true;
}

} // namespace tour::asset
