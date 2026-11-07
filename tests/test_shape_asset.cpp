// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Asset/shape_asset.h>
#include <cassert>
#include <limits>
#include <string>
using namespace tour::asset;
int main() {
    std::string error;
    for (ShapeKind kind : {ShapeKind::Spline,
                           ShapeKind::PointCloud,
                           ShapeKind::Plane,
                           ShapeKind::Box,
                           ShapeKind::Sphere,
                           ShapeKind::Cylinder,
                           ShapeKind::Cone}) {
        const ShapeAsset initial = DefaultShapeAsset(kind);
        ShapeAsset parsed;
        assert(ParseShapeAsset(SerializeShapeAsset(initial), parsed, error));
        assert(parsed.kind == kind);
        ShapeGeometry mesh;
        assert(TessellateShape(parsed, mesh, error));
        assert(!mesh.vertices.empty());
        if (kind == ShapeKind::Spline) {
            assert(mesh.topology == ShapeGeometry::Topology::Lines);
            assert(mesh.indices.size() == 2 * 2 * initial.segments);
        } else if (kind == ShapeKind::PointCloud) {
            assert(mesh.topology == ShapeGeometry::Topology::Points);
            assert(mesh.vertices.size() == initial.points.size());
        } else {
            assert(mesh.topology == ShapeGeometry::Topology::Triangles);
            assert(!mesh.indices.empty() && mesh.indices.size() % 3 == 0);
        }
    }
    {
        std::string obj;
        auto box = DefaultShapeAsset(ShapeKind::Box);
        assert(ShapeToObj(box, obj, error));
        assert(obj.find("o procedural_box") != std::string::npos);
        assert(obj.find("v -1 -1 -1") != std::string::npos);
        assert(obj.find("f ") != std::string::npos);
        assert(!ShapeToObj(DefaultShapeAsset(ShapeKind::PointCloud), obj, error));
        assert(!ShapeToObj(DefaultShapeAsset(ShapeKind::Spline), obj, error));
    }
    ShapeAsset loop = DefaultShapeAsset(ShapeKind::Spline);
    loop.closed = true;
    loop.segments = 5;
    ShapeGeometry curve;
    assert(TessellateShape(loop, curve, error));
    assert(curve.vertices.size() == 15 && curve.indices.size() == 30);
    assert(curve.indices.back() == 0);
    ShapeAsset result = DefaultShapeAsset(ShapeKind::Box);
    assert(!ParseShapeAsset("shape 2\nkind box\n", result, error));
    assert(result.kind == ShapeKind::Box);
    assert(!ParseShapeAsset("shape 1\nkind box\nsegments 10000000\n", result, error));
    assert(!ParseShapeAsset("shape 1\nkind unknown\n", result, error));
    assert(!ParseShapeAsset("shape 1\nkind box\nunknown 0\n", result, error));
    ShapeAsset oversized = DefaultShapeAsset(ShapeKind::Spline);
    oversized.points.resize(2000);
    oversized.segments = 256;
    ShapeGeometry capped;
    assert(!TessellateShape(oversized, capped, error));
    assert(capped.vertices.empty());
    ShapeAsset invalid = DefaultShapeAsset(ShapeKind::PointCloud);
    invalid.points[0].x = std::numeric_limits<float>::infinity();
    ShapeGeometry untouched;
    assert(!TessellateShape(invalid, untouched, error));
}
