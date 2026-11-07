// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Engine-level, UI-free graph topology/document model.
// Domain compilers (placement, ecotope, audio, state machines) own
// semantics and evaluation. This layer only understands node identities,
// typed named ports, connections, cardinality and cycle policies.
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tour::graph {

enum class PortDirection : uint8_t { Input, Output };
struct Port {
    std::string name;
    std::string type; // domain-defined semantic type, e.g. "field"
    PortDirection direction = PortDirection::Input;
    bool multiple = false; // only meaningful on input ports
};
struct Node {
    uint32_t id = 0;  // stable within authored document; zero invalid
    std::string type; // domain-defined node schema name
    std::vector<Port> ports;
};
struct Link {
    uint32_t id = 0;
    uint32_t from = 0, to = 0;
    std::string output, input;
};
struct Document {
    std::map<uint32_t, Node> nodes; // sorted => deterministic validation
    std::map<uint32_t, Link> links;

    [[nodiscard]] const Port* FindPort(uint32_t id,
                                       std::string_view name,
                                       PortDirection direction) const {
        auto n = nodes.find(id);
        if (n == nodes.end())
            return nullptr;
        for (const Port& p : n->second.ports)
            if (p.name == name && p.direction == direction)
                return &p;
        return nullptr;
    }
};
struct ValidationPolicy {
    bool allowCycles = false; // state machines permit cycles; placement does not
    bool allowSelfLinks = false;
};
struct ValidationResult {
    bool ok = false;
    std::string error;
    std::vector<uint32_t> order; // stable topological order when acyclic
};

// Structural consistency is domain-independent. Reject malformed ids,
// unknown ports, channel/type mismatches, multiple wires into a single
// input and cycles (unless explicitly allowed by the consumer).
inline ValidationResult Validate(const Document& doc, ValidationPolicy policy = {}) {
    ValidationResult result;
    std::map<uint32_t, uint32_t> indegrees;
    std::map<uint32_t, std::vector<uint32_t>> outgoing;
    for (const auto& [id, node] : doc.nodes) {
        if (!id || node.id != id || node.type.empty()) {
            result.error = "invalid graph node identity/type";
            return result;
        }
        std::set<std::pair<PortDirection, std::string>> names;
        for (const Port& p : node.ports)
            if (p.name.empty() || p.type.empty() || !names.emplace(p.direction, p.name).second) {
                result.error = "duplicate or empty graph port";
                return result;
            }
        indegrees[id] = 0;
    }
    std::set<std::pair<uint32_t, std::string>> occupiedInputs;
    for (const auto& [id, link] : doc.links) {
        if (!id || id != link.id || !link.from || !link.to ||
            (!policy.allowSelfLinks && link.from == link.to)) {
            result.error = "invalid graph link identity or self-link";
            return result;
        }
        const Port* src = doc.FindPort(link.from, link.output, PortDirection::Output);
        const Port* dst = doc.FindPort(link.to, link.input, PortDirection::Input);
        if (!src || !dst || src->type != dst->type) {
            result.error = "dangling or incompatible graph ports";
            return result;
        }
        if (!dst->multiple && !occupiedInputs.emplace(link.to, link.input).second) {
            result.error = "input port has more than one link";
            return result;
        }
        ++indegrees[link.to];
        outgoing[link.from].push_back(link.to);
    }
    // Kahn's algorithm, with node-id tie breaking for deterministic
    // cooked traversal independent of insertion/undo history.
    std::set<uint32_t> ready;
    for (const auto& [id, count] : indegrees)
        if (!count)
            ready.insert(id);
    while (!ready.empty()) {
        const uint32_t id = *ready.begin();
        ready.erase(ready.begin());
        result.order.push_back(id);
        for (uint32_t downstream : outgoing[id])
            if (--indegrees[downstream] == 0)
                ready.insert(downstream);
    }
    if (result.order.size() != doc.nodes.size()) {
        result.order.clear();
        if (!policy.allowCycles) {
            result.error = "cycle in graph";
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace tour::graph
