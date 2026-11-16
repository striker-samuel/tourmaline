// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Pure point-array graph foundation. One input array -> one output array.
// Nodes describe values, not mutable scene objects or GPU commands.
// This CPU reference is deliberately independent of the Placement GPU ABI;
// a future backend can lower the validated plan into GPU kernels.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <Engine/Graph/graph.h>

namespace tour::placement::point_graph {

struct Point {
    glm::vec3 position{0.0f};
    glm::vec4 color{1.0f};
    glm::vec3 velocity{0.0f};
    uint32_t prototype = 0; // index into an external instancer mesh/prototype table
    uint64_t id = 0;        // source-point identity survives filtering and transforms
};

using PointArray = std::vector<Point>;

// Every operation consumes an array and returns an array. No operation
// changes its input, a scene node, or an external resource.
enum class Op : uint8_t {
    Input,
    Translate,
    Scale,
    SetColor,
    SetVelocity,
    SetPrototype,
    KeepEveryNth
};

struct Node {
    uint32_t id = 0;     // nonzero and unique
    Op op = Op::Input;
    uint32_t input = 0;  // parent node ID; Input requires 0
    glm::vec4 value{0.0f}; // XYZ, RGBA, or scale XYZ, depending on op
    uint32_t index = 0;  // prototype index / N for KeepEveryNth
};

struct Graph {
    std::vector<Node> nodes;
    uint32_t output = 0;
};

struct Program {
    std::vector<Node> steps; // ordered live nodes; no unreachable graph branches
};

// Compile performs structural/type validation, cycle detection and
// dead-branch elimination. Failed compilation leaves 'out' unchanged.
// The Input node is an explicit source and must be the only live root.
inline bool Compile(const Graph& graph, Program& out, std::string& error) {
    std::unordered_map<uint32_t, const Node*> byId;
    for (const Node& node : graph.nodes) {
        if (node.id == 0 || !byId.emplace(node.id, &node).second) {
            error = "point graph has a zero or duplicate node ID";
            return false;
        }
    }
    if (graph.output == 0 || byId.find(graph.output) == byId.end()) {
        error = "point graph has no valid output";
        return false;
    }

    // Reuse the ENGINE's canonical graph document, typed ports and
    // deterministic validator. This domain only supplies node semantics.
    tour::graph::Document topology;
    for (const Node& node : graph.nodes) {
        tour::graph::Node gn;
        gn.id = node.id;
        gn.type = node.op == Op::Input ? "point.input" : "point.transform";
        if (node.op != Op::Input)
            gn.ports.push_back({"in", "point_array", tour::graph::PortDirection::Input});
        gn.ports.push_back({"out", "point_array", tour::graph::PortDirection::Output});
        topology.nodes.emplace(node.id, std::move(gn));
    }
    uint32_t linkId = 1;
    for (const Node& node : graph.nodes) {
        if (node.op == Op::Input) {
            if (node.input != 0) {
                error = "input node cannot consume another array";
                return false;
            }
            continue;
        }
        if (node.input == 0) {
            error = "transformation has no input";
            return false;
        }
        topology.links.emplace(linkId, tour::graph::Link{linkId, node.input, node.id, "out", "in"});
        ++linkId;
    }
    const auto validation = tour::graph::Validate(topology);
    if (!validation.ok) {
        error = validation.error;
        return false;
    }
    // The array program has one selected output. Prune unrelated branches
    // while retaining the shared graph's dependency order.
    std::unordered_set<uint32_t> live;
    uint32_t cursor = graph.output;
    while (cursor != 0) {
        if (!live.insert(cursor).second) {
            error = "point graph contains a cycle";
            return false;
        }
        const Node& node = *byId.at(cursor);
        cursor = node.input;
    }
    Program compiled;
    uint32_t sourceCount = 0;
    for (uint32_t id : validation.order) {
        if (!live.count(id))
            continue;
        const Node& node = *byId.at(id);
        if (node.op == Op::Input) {
            ++sourceCount;
        } else {
            if (node.op != Op::KeepEveryNth && node.op != Op::SetPrototype &&
                (!std::isfinite(node.value.x) || !std::isfinite(node.value.y) ||
                 !std::isfinite(node.value.z) || !std::isfinite(node.value.w))) {
                error = "point graph has non-finite transformation parameters";
                return false;
            }
            if (node.op == Op::KeepEveryNth && node.index == 0) {
                error = "KeepEveryNth requires N >= 1";
                return false;
            }
            if (node.op != Op::Translate && node.op != Op::Scale &&
                node.op != Op::SetColor && node.op != Op::SetVelocity &&
                node.op != Op::SetPrototype && node.op != Op::KeepEveryNth) {
                error = "point graph has an unknown operation";
                return false;
            }
        }
        compiled.steps.push_back(node);
    }
    if (sourceCount != 1) {
        error = "point graph requires one reachable input array";
        return false;
    }
    out = std::move(compiled);
    error.clear();
    return true;
}

// Reference evaluator: input never changes, and output is replaced only on
// success. The compiler guarantees a single-input path to the output.
// 'input' may be any size; there is no arbitrary per-tile 65536 point cap.
inline bool Evaluate(const Program& program,
                     std::span<const Point> input,
                     PointArray& out,
                     std::string& error) {
    if (program.steps.empty() || program.steps.front().op != Op::Input) {
        error = "point graph program has no input";
        return false;
    }
    PointArray current(input.begin(), input.end());
    for (size_t i = 1; i < program.steps.size(); ++i) {
        const Node& node = program.steps[i];
        PointArray next;
        next.reserve(current.size());
        for (const Point& src : current) {
            Point dst = src;
            switch (node.op) {
            case Op::Translate:
                dst.position += glm::vec3(node.value);
                break;
            case Op::Scale:
                dst.position *= glm::vec3(node.value);
                break;
            case Op::SetColor:
                dst.color = node.value;
                break;
            case Op::SetVelocity:
                dst.velocity = glm::vec3(node.value);
                break;
            case Op::SetPrototype:
                dst.prototype = node.index;
                break;
            case Op::KeepEveryNth:
                if (node.index == 0) {
                    error = "invalid KeepEveryNth divisor";
                    return false;
                }
                if (dst.id % node.index != 0)
                    continue;
                break;
            default:
                error = "invalid operation in compiled point graph";
                return false;
            }
            next.push_back(dst);
        }
        current.swap(next);
    }
    out = std::move(current);
    error.clear();
    return true;
}

} // namespace tour::placement::point_graph
