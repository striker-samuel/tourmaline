// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Particle source graph: authoring-independent typed plugs and emitter compilation.
// The host supplies source points (brush strokes, heightfields, placement instances,
// or a future registered provider). No editor, terrain renderer, or ImGui dependency.
#include <Engine/Graph/graph.h>
#include <Engine/Rendergraph/meshparticles.h>
#include <Engine/Rendergraph/particles.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace tour::particles {

inline constexpr const char* kPainted = "particle.source.painted";
inline constexpr const char* kHeightfield = "particle.source.heightfield";
inline constexpr const char* kPlacement = "particle.source.placement";
inline constexpr const char* kHeightFilter = "particle.filter.height";
inline constexpr const char* kEmitter = "particle.emitter";
// Third-party providers can use any particle.source.<provider> name.
// Node semantics and source-point resolution live in the host callback.
inline bool IsSourceType(const std::string& type) {
    return type.rfind("particle.source.", 0) == 0 && type.size() > sizeof("particle.source.") - 1;
}

struct Point {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float weight = 1.0f;
};
// A sink can render billboards or compile GPU mesh-instance emitter rows.
// Mesh geometry/material bindings are supplied by the renderer's mesh table.
inline constexpr uint32_t kBillboardRender = 0;
inline constexpr uint32_t kMeshRender = 1;
struct Attributes {
    // Source selector: -1 = all strokes/terrain/placement data.
    int selector = -1;
    // Filter thresholds (inclusive), in world-space meters.
    float minY = -1000000.0f, maxY = 1000000.0f;
    // Emitter attributes. Rate is TOTAL particles/s for this sink,
    // distributed across selected sources (rather than per point).
    float rate = 18.0f, sizeStart = 0.35f, sizeEnd = 1.8f;
    float lifetime = 6.0f, speed = 0.3f, radius = 0.2f;
    uint32_t renderMode = kBillboardRender;
    uint32_t meshId = 0; // mesh-particle bin, [0, kMeshParticlesMaxBins)
    glm::vec3 color{0.58f, 0.49f, 0.36f};
};
struct Graph {
    tour::graph::Document topology;
    std::map<uint32_t, Attributes> attrs;

    static tour::graph::Node MakeNode(uint32_t id, const std::string& type) {
        using namespace tour::graph;
        Node n;
        n.id = id;
        n.type = type;
        if (IsSourceType(type)) {
            n.ports.push_back({"points", "particle.points", PortDirection::Output});
        } else if (type == kHeightFilter) {
            n.ports.push_back({"points", "particle.points", PortDirection::Input});
            n.ports.push_back({"points", "particle.points", PortDirection::Output});
        } else if (type == kEmitter) {
            n.ports.push_back({"sources", "particle.points", PortDirection::Input, true});
        }
        return n;
    }
    static Graph Default() {
        Graph g;
        g.topology.nodes.emplace(1, MakeNode(1, kPainted));
        g.topology.nodes.emplace(2, MakeNode(2, kEmitter));
        g.topology.links.emplace(1, tour::graph::Link{1, 1, 2, "points", "sources"});
        g.attrs[1] = {};
        g.attrs[2] = {};
        return g;
    }
    uint32_t AddSource(const std::string& type) {
        if (!IsSourceType(type))
            return 0;
        uint32_t id = 1;
        while (topology.nodes.contains(id))
            ++id;
        topology.nodes.emplace(id, MakeNode(id, type));
        attrs[id] = {};
        return id;
    }
    uint32_t AddFilter() {
        uint32_t id = 1;
        while (topology.nodes.contains(id))
            ++id;
        topology.nodes.emplace(id, MakeNode(id, kHeightFilter));
        attrs[id] = {};
        return id;
    }
    uint32_t Connect(uint32_t source, uint32_t destination, const std::string& input = "sources") {
        if (!topology.nodes.contains(source) || !topology.nodes.contains(destination))
            return 0;
        uint32_t id = 1;
        while (topology.links.contains(id))
            ++id;
        auto next = topology;
        next.links.emplace(id, tour::graph::Link{id, source, destination, "points", input});
        if (!tour::graph::Validate(next).ok)
            return 0;
        topology = std::move(next);
        return id;
    }
    bool Remove(uint32_t id) {
        if (!topology.nodes.erase(id))
            return false;
        attrs.erase(id);
        for (auto it = topology.links.begin(); it != topology.links.end();) {
            if (it->second.from == id || it->second.to == id)
                it = topology.links.erase(it);
            else
                ++it;
        }
        return true;
    }
};

using SourceProvider = std::function<
    bool(const std::string& type, int selector, std::vector<Point>& points, std::string& error)>;
struct Result {
    bool ok = false;
    std::string error, warning;
    std::vector<smvk::ParticleEmitterGpu> emitters;
    std::vector<smvk::MeshParticleEmitterGpu> meshEmitters;
};

// Provider-evaluation and graph compilation are deterministic by stable node ID.
// On budget overflow, evenly select representative input points. No hidden
// emitter allocation, GPU memory, or editor-global state is owned here.
inline Result Evaluate(const Graph& graph, const SourceProvider& provider, uint32_t capacity = 16) {
    Result result;
    auto validation = tour::graph::Validate(graph.topology);
    if (!validation.ok) {
        result.error = validation.error;
        return result;
    }
    if (!provider) {
        result.error = "particle source provider is missing";
        return result;
    }
    std::map<uint32_t, std::vector<Point>> values;
    bool hasSink = false;
    for (uint32_t id : validation.order) {
        const auto& node = graph.topology.nodes.at(id);
        auto at = graph.attrs.find(id);
        if (at == graph.attrs.end()) {
            result.error = "node has no attributes: " + std::to_string(id);
            return result;
        }
        const Attributes& attr = at->second;
        if (IsSourceType(node.type)) {
            if (!provider(node.type, attr.selector, values[id], result.error))
                return result;
        } else if (node.type == kHeightFilter) {
            bool input = false;
            for (const auto& [lid, link] : graph.topology.links)
                if (link.to == id) {
                    input = true;
                    for (const auto& p : values.at(link.from))
                        if (p.position.y >= attr.minY && p.position.y <= attr.maxY)
                            values[id].push_back(p);
                }
            if (!input) {
                result.error = "height filter has no input";
                return result;
            }
        } else if (node.type == kEmitter) {
            hasSink = true;
            std::vector<Point> candidates;
            for (const auto& [lid, link] : graph.topology.links)
                if (link.to == id) {
                    const auto& points = values.at(link.from);
                    candidates.insert(candidates.end(), points.begin(), points.end());
                }
            if (candidates.empty())
                continue;
            if (attr.renderMode > kMeshRender ||
                (attr.renderMode == kMeshRender && attr.meshId >= smvk::kMeshParticlesMaxBins)) {
                result.error = "invalid mesh particle renderer or mesh bin";
                return result;
            }
            const size_t used = result.emitters.size() + result.meshEmitters.size();
            const uint32_t remaining =
                capacity > used ? capacity - static_cast<uint32_t>(used) : 0u;
            if (!remaining) {
                result.warning = "particle emitter budget reached; some sources omitted";
                continue;
            }
            const uint32_t count = std::min(remaining, (uint32_t)candidates.size());
            if (count < candidates.size())
                result.warning = "particle source points downsampled to emitter budget";
            for (uint32_t i = 0; i < count; ++i) {
                const Point& point = candidates[size_t(i) * candidates.size() / count];
                const glm::vec3& pos = point.position;
                if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) ||
                    !std::isfinite(point.weight)) {
                    result.error = "non-finite particle source point";
                    return result;
                }
                if (attr.renderMode == kMeshRender) {
                    smvk::MeshParticleEmitterGpu mesh{};
                    mesh.position = glm::vec4(pos, std::max(0.0f, attr.radius));
                    mesh.direction = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f);
                    mesh.velocity = glm::vec4(
                        std::max(0.0f, attr.speed) * 0.5f, std::max(0.0f, attr.speed), 0.0f, 1.0f);
                    mesh.life = glm::vec4(std::max(0.01f, attr.lifetime * 0.75f),
                                          std::max(0.01f, attr.lifetime),
                                          std::max(0.0f, attr.rate) *
                                              std::clamp(point.weight, 0.0f, 1.0f) / count,
                                          float(attr.meshId));
                    mesh.spin.z = float(id * 4099u + i);
                    mesh.size = glm::vec4(std::max(0.001f, attr.sizeStart),
                                          std::max(0.001f, attr.sizeEnd),
                                          0.12f,
                                          0.0f);
                    mesh.color = glm::vec4(glm::max(attr.color, glm::vec3(0.0f)), 0.0f);
                    result.meshEmitters.push_back(mesh);
                    continue;
                }
                smvk::ParticleEmitterGpu e{};
                e.position = glm::vec4(pos, std::max(0.0f, attr.radius));
                e.direction = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f);
                e.color = glm::vec4(glm::max(attr.color, glm::vec3(0)), 0.0f);
                e.velocity = glm::vec4(
                    std::max(0.0f, attr.speed) * 0.5f, std::max(0.0f, attr.speed), 0.7f, 0.0f);
                e.size = glm::vec4(std::max(0.001f, attr.sizeStart),
                                   std::max(0.001f, attr.sizeEnd),
                                   0.12f,
                                   float(id * 4099u + i));
                e.life = glm::vec4(std::max(0.01f, attr.lifetime * 0.75f),
                                   std::max(0.01f, attr.lifetime),
                                   std::max(0.0f, attr.rate) *
                                       std::clamp(point.weight, 0.0f, 1.0f) / count,
                                   float(smvk::kParticleShape_Ground));
                e.fade = glm::vec4(0.25f, 0.45f, 0.1f, 0.0f);
                e.tail.y = smvk::kParticleRender_Dust;
                result.emitters.push_back(e);
            }
        } else {
            result.error = "unknown particle graph node type: " + node.type;
            return result;
        }
    }
    if (!hasSink) {
        result.error = "particle graph has no emitter sink";
        return result;
    }
    result.ok = true;
    return result;
}

// Single-line versioned node value for .tour editorSettings; links are
// serialized separately using the shared graph's stable IDs and port names.
inline std::string SerializeAttributes(const std::string& type, const Attributes& a) {
    std::ostringstream out;
    out.precision(9);
    out << "v2 " << type << " " << a.selector << " " << a.minY << " " << a.maxY << " " << a.rate
        << " " << a.sizeStart << " " << a.sizeEnd << " " << a.lifetime << " " << a.speed << " "
        << a.radius << " " << a.color.x << " " << a.color.y << " " << a.color.z << " "
        << a.renderMode << " " << a.meshId;
    return out.str();
}
inline bool ParseAttributes(const std::string& text, std::string& type, Attributes& a) {
    std::istringstream in(text);
    std::string version, extra;
    Attributes parsed;
    if (!(in >> version >> type >> parsed.selector >> parsed.minY >> parsed.maxY >> parsed.rate >>
          parsed.sizeStart >> parsed.sizeEnd >> parsed.lifetime >> parsed.speed >> parsed.radius >>
          parsed.color.x >> parsed.color.y >> parsed.color.z) ||
        (version != "v1" && version != "v2") ||
        (!IsSourceType(type) && type != kHeightFilter && type != kEmitter))
        return false;
    if (version == "v2" && !(in >> parsed.renderMode >> parsed.meshId))
        return false;
    if ((in >> extra) || parsed.renderMode > kMeshRender ||
        (parsed.renderMode == kMeshRender && parsed.meshId >= smvk::kMeshParticlesMaxBins))
        return false;
    const float values[] = {parsed.minY,
                            parsed.maxY,
                            parsed.rate,
                            parsed.sizeStart,
                            parsed.sizeEnd,
                            parsed.lifetime,
                            parsed.speed,
                            parsed.radius,
                            parsed.color.x,
                            parsed.color.y,
                            parsed.color.z};
    for (float f : values)
        if (!std::isfinite(f))
            return false;
    a = parsed;
    return true;
}
} // namespace tour::particles
