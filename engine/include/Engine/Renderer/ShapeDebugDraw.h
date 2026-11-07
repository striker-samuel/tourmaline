// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// CPU bridge from authored .shape geometry to the WorldRenderer's native
// point-list and line-list GPU draw stream. Caller supplies world transform;
// the renderer consumes the result through SetDebugDrawSource().
#include <Engine/Asset/shape_asset.h>
#include <Engine/Renderer/DebugDraw.h>

#include <glm/glm.hpp>
#include <string>

namespace tour::renderer {

inline bool AppendShapeDebug(const asset::ShapeAsset& shape,
                             const glm::mat4& worldFromLocal,
                             const glm::vec3& color,
                             tour::DebugDrawList& draw,
                             std::string& error,
                             tour::DebugDepthMode depth = tour::DebugDepthMode::Tested) {
    asset::ShapeGeometry geometry;
    if (!asset::TessellateShape(shape, geometry, error))
        return false;
    if (geometry.topology == asset::ShapeGeometry::Topology::Triangles) {
        error = "triangle shapes require the mesh renderer (or OBJ export)";
        return false;
    }
    // Do not partially append huge datasets when the debug stream budget
    // cannot accommodate them.
    const size_t count = geometry.topology == asset::ShapeGeometry::Topology::Points
                             ? geometry.vertices.size()
                             : geometry.indices.size() / 2;
    const size_t available = geometry.topology == asset::ShapeGeometry::Topology::Points
                                 ? tour::DebugDrawList::MaxPoints - draw.PointCount()
                                 : tour::DebugDrawList::MaxLines - draw.Size();
    if (count > available) {
        error = "shape exceeds available debug drawing capacity";
        return false;
    }
    auto transform = [&](const asset::ShapePoint& p) {
        return glm::vec3(worldFromLocal * glm::vec4(p.x, p.y, p.z, 1.0f));
    };
    if (geometry.topology == asset::ShapeGeometry::Topology::Points) {
        for (const auto& vertex : geometry.vertices)
            draw.Point(transform(vertex), color, depth);
    } else {
        for (size_t i = 0; i + 1 < geometry.indices.size(); i += 2)
            draw.Line(transform(geometry.vertices[geometry.indices[i]]),
                      transform(geometry.vertices[geometry.indices[i + 1]]),
                      color,
                      depth);
    }
    error.clear();
    return true;
}

} // namespace tour::renderer
