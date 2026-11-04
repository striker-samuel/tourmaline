// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Optional CGAL Polygon Mesh Processing backend for offline editor tooling.
// See docs/intersection_geometry_tools.md for licensing and prerequisites.
// Never included in the engine runtime unless explicitly enabled.
#include <Engine/Geometry/intersection_curves.h>

#ifdef TOURMALINE_EDITOR_WITH_CGAL
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/intersection_polylines.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <boost/graph/graph_traits.hpp>
#include <iterator>
#include <map>
#include <tuple>

namespace smvk::geometry {
// Extracts mesh/mesh curves with CGAL, preserving discontinuities. Intended
// for static triangle soups. Deduplicates shared positions into a proper
// indexed Surface_mesh before intersection; rejects nonmanifold topology.
inline bool ExtractMeshIntersectionsCGAL(const TriangleSoup& a,
    const TriangleSoup& b, CurveSet& out, std::string& error) {
    using K = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Point = K::Point_3;
    using Mesh = CGAL::Surface_mesh<Point>;
    auto build = [&](const TriangleSoup& src, Mesh& mesh) -> bool {
        std::map<std::tuple<double,double,double>,Mesh::Vertex_index> vertices;
        auto vertex = [&](const glm::vec3& p) -> Mesh::Vertex_index {
            auto key = std::make_tuple(double(p.x),double(p.y),double(p.z));
            auto it = vertices.find(key);
            if (it != vertices.end()) return it->second;
            auto id = mesh.add_vertex(Point(p.x,p.y,p.z));
            vertices.emplace(key,id);
            return id;
        };
        for (size_t i=0;i+2<src.triangles.size();i+=3) {
            auto x=vertex(src.triangles[i]), y=vertex(src.triangles[i+1]),
                 z=vertex(src.triangles[i+2]);
            if (x==y || y==z || z==x) continue;
            if (mesh.add_face(x,y,z) == Mesh::null_face()) return false;
        }
        return mesh.number_of_faces() > 0;
    };
    Mesh first, second;
    if (!build(a,first) || !build(b,second)) {
        error = "CGAL input contains inconsistent face winding, "
                "nonmanifold topology or no valid triangles";
        return false;
    }
    try {
        if (CGAL::Polygon_mesh_processing::does_self_intersect(first) ||
            CGAL::Polygon_mesh_processing::does_self_intersect(second)) {
            error = "CGAL requires input meshes without self-intersections";
            return false;
        }
        std::vector<std::vector<Point>> raw;
        CGAL::Polygon_mesh_processing::intersection_polylines(
            first,second,std::back_inserter(raw));
        CurveSet result;
        for (const auto& p : raw) {
            Polyline curve;
            for (const auto& point : p)
                curve.points.emplace_back(
                    float(CGAL::to_double(point.x())),
                    float(CGAL::to_double(point.y())),
                    float(CGAL::to_double(point.z())));
            if (curve.points.size()>2 &&
                glm::distance(curve.points.front(),curve.points.back()) < 1e-5f) {
                curve.points.pop_back();
                curve.closed = true;
            }
            if (curve.points.size()>1)
                result.curves.push_back(std::move(curve));
        }
        out = std::move(result);
        return true;
    } catch (const std::exception& e) {
        error = std::string("CGAL intersection failed: ") + e.what();
        return false;
    }
}
} // namespace smvk::geometry
#endif
