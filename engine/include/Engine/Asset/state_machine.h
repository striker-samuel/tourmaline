// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Stateless authored graph + caller-owned mutable instance. No gameplay
// callbacks, input bindings, animation dependencies, or global registry.
#include <Engine/Graph/graph.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tour::asset {

enum class StateParamKind : uint8_t { Bool, Integer, Float, Trigger };
enum class StateCompare : uint8_t { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, Fired };

struct StateParameter {
    std::string name;
    StateParamKind kind = StateParamKind::Bool;
    double initial = 0.0;
};
struct StateTransition {
    // from = UINT32_MAX means any state (including the entry state).
    uint32_t from = UINT32_MAX, to = 0, parameter = 0;
    StateCompare compare = StateCompare::Equal;
    double threshold = 0.0;
};
struct StateMachineAsset {
    std::vector<std::string> states;
    std::vector<StateParameter> parameters;
    std::vector<StateTransition> transitions; // authored priority order
    uint32_t entry = UINT32_MAX;

    [[nodiscard]] uint32_t State(std::string_view name) const {
        for (uint32_t i = 0; i < states.size(); ++i)
            if (states[i] == name) return i;
        return UINT32_MAX;
    }
    [[nodiscard]] uint32_t Parameter(std::string_view name) const {
        for (uint32_t i = 0; i < parameters.size(); ++i)
            if (parameters[i].name == name) return i;
        return UINT32_MAX;
    }
};

// Optional structural projection for shared graph inspectors/tools. A
// state machine is a directed graph that intentionally ALLOWS cycles;
// wildcard transitions are represented by an implicit "any" node.
inline ::tour::graph::Document StateMachineTopology(
    const StateMachineAsset& asset) {
    ::tour::graph::Document graph;
    for (uint32_t i = 0; i < asset.states.size(); ++i) {
        ::tour::graph::Node n;
        n.id = i + 1;
        n.type = asset.states[i];
        n.ports.push_back({"in", "flow",
            ::tour::graph::PortDirection::Input, true});
        n.ports.push_back({"out", "flow",
            ::tour::graph::PortDirection::Output});
        graph.nodes.emplace(n.id, std::move(n));
    }
    bool any = false;
    for (const StateTransition& tr : asset.transitions)
        if (tr.from == UINT32_MAX) any = true;
    const uint32_t anyId = static_cast<uint32_t>(asset.states.size()) + 1;
    if (any) {
        ::tour::graph::Node n;
        n.id = anyId;
        n.type = "any";
        n.ports.push_back({"out", "flow",
            ::tour::graph::PortDirection::Output});
        graph.nodes.emplace(n.id, std::move(n));
    }
    for (size_t i = 0; i < asset.transitions.size(); ++i) {
        const auto& tr = asset.transitions[i];
        ::tour::graph::Link link;
        link.id = static_cast<uint32_t>(i + 1);
        link.from = tr.from == UINT32_MAX ? anyId : tr.from + 1;
        link.to = tr.to + 1;
        link.output = "out";
        link.input = "in";
        graph.links.emplace(link.id, std::move(link));
    }
    return graph;
}

namespace state_detail {
inline bool Name(std::string_view value) {
    if (value.empty()) return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}
inline bool Number(const std::string& text, double& out) {
    std::istringstream input(text);
    input >> out;
    return bool(input) && input.eof() && std::isfinite(out);
}
inline bool Integer(const std::string& text, double& out) {
    int32_t value = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
        return false;
    out = value;
    return true;
}
inline bool Comparison(const std::string& text, StateCompare& out) {
    if (text == "==") out = StateCompare::Equal;
    else if (text == "!=") out = StateCompare::NotEqual;
    else if (text == "<") out = StateCompare::Less;
    else if (text == "<=") out = StateCompare::LessEqual;
    else if (text == ">") out = StateCompare::Greater;
    else if (text == ">=") out = StateCompare::GreaterEqual;
    else return false;
    return true;
}
} // namespace state_detail

// Grammar:
//   statemachine 1
//   param speed float 0
//   param dead bool false
//   param jump trigger
//   state idle
//   state move
//   entry idle
//   transition idle move speed > 0.1
//   transition * idle jump fired
// First eligible transition wins; at most one transition per Tick.
// An 'any' transition uses '*'. All triggers expire at the end of a tick.
inline bool ParseStateMachineText(std::string_view source,
    StateMachineAsset& out, std::string& err) {
    err.clear();
    StateMachineAsset parsed;
    struct Pending {
        std::string from, to, param, op, value;
        uint32_t line = 0;
    };
    std::vector<Pending> pending;
    bool version = false, entered = false;
    uint32_t lineNumber = 0;
    std::istringstream lines{std::string(source)};
    std::string line;
    auto fail = [&](std::string message) {
        err = "statemachine line " + std::to_string(lineNumber) + ": " + message;
        return false;
    };
    while (std::getline(lines, line)) {
        ++lineNumber;
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream fields(line);
        std::vector<std::string> t;
        for (std::string word; fields >> word;) t.push_back(std::move(word));
        if (t.empty()) continue;
        if (!version) {
            if (t.size() != 2 || t[0] != "statemachine" || t[1] != "1")
                return fail("expected statemachine 1");
            version = true;
        } else if (t[0] == "state") {
            if (t.size() != 2 || !state_detail::Name(t[1])
                || parsed.State(t[1]) != UINT32_MAX || parsed.states.size() >= 65535)
                return fail("invalid or duplicate state");
            parsed.states.push_back(t[1]);
        } else if (t[0] == "param") {
            if ((t.size() != 3 && t.size() != 4)
                || !state_detail::Name(t[1])
                || parsed.Parameter(t[1]) != UINT32_MAX
                || parsed.parameters.size() >= 65535)
                return fail("invalid or duplicate parameter");
            StateParameter param;
            param.name = t[1];
            if (t[2] == "bool") param.kind = StateParamKind::Bool;
            else if (t[2] == "int") param.kind = StateParamKind::Integer;
            else if (t[2] == "float") param.kind = StateParamKind::Float;
            else if (t[2] == "trigger") param.kind = StateParamKind::Trigger;
            else return fail("unknown parameter type");
            if (param.kind == StateParamKind::Trigger) {
                if (t.size() != 3) return fail("trigger cannot have a default");
            } else {
                if (t.size() != 4) return fail("parameter requires a default");
                if (param.kind == StateParamKind::Bool) {
                    if (t[3] != "true" && t[3] != "false")
                        return fail("bool default must be true or false");
                    param.initial = t[3] == "true" ? 1 : 0;
                } else if (!(param.kind == StateParamKind::Integer
                        ? state_detail::Integer(t[3], param.initial)
                        : state_detail::Number(t[3], param.initial)))
                    return fail("invalid numeric default");
            }
            parsed.parameters.push_back(std::move(param));
        } else if (t[0] == "entry") {
            if (entered || t.size() != 2 || !state_detail::Name(t[1]))
                return fail("invalid or duplicate entry");
            // Resolve after parsing; states may be forward declared.
            pending.push_back({"", t[1], "", "entry", "", lineNumber});
            entered = true;
        } else if (t[0] == "transition") {
            if ((t.size() != 5 && t.size() != 6)
                || (t[1] != "*" && !state_detail::Name(t[1]))
                || !state_detail::Name(t[2])
                || !state_detail::Name(t[3]) || pending.size() >= 65535)
                return fail("transition syntax: from to param op [value]");
            pending.push_back({t[1], t[2], t[3], t[4],
                t.size() == 6 ? t[5] : "", lineNumber});
        } else return fail("unknown directive: " + t[0]);
    }
    if (!version || !entered || parsed.states.empty())
        return fail("missing version, entry or states");
    for (const auto& p : pending) {
        lineNumber = p.line;
        const uint32_t to = parsed.State(p.to);
        if (to == UINT32_MAX) return fail("unknown target state: " + p.to);
        if (p.op == "entry") {
            parsed.entry = to;
            continue;
        }
        const uint32_t from = p.from == "*" ? UINT32_MAX : parsed.State(p.from);
        const uint32_t param = parsed.Parameter(p.param);
        if ((p.from != "*" && from == UINT32_MAX) || param == UINT32_MAX)
            return fail("unknown source state or parameter");
        StateTransition tr;
        tr.from = from;
        tr.to = to;
        tr.parameter = param;
        const StateParamKind kind = parsed.parameters[param].kind;
        if (kind == StateParamKind::Trigger) {
            if (p.op != "fired" || !p.value.empty())
                return fail("trigger requires 'fired' with no value");
            tr.compare = StateCompare::Fired;
        } else {
            if (p.value.empty() || !state_detail::Comparison(p.op, tr.compare))
                return fail("transition requires a comparison and value");
            if (kind == StateParamKind::Bool) {
                if ((p.op != "==" && p.op != "!=")
                    || (p.value != "true" && p.value != "false"))
                    return fail("bool comparison requires == or != and true/false");
                tr.threshold = p.value == "true" ? 1 : 0;
            } else if (!(kind == StateParamKind::Integer
                    ? state_detail::Integer(p.value, tr.threshold)
                    : state_detail::Number(p.value, tr.threshold)))
                return fail("invalid transition threshold");
        }
        parsed.transitions.push_back(tr);
    }
    const auto topologyCheck = ::tour::graph::Validate(
        StateMachineTopology(parsed), {true, true});
    if (!topologyCheck.ok) return fail("invalid state topology: "
        + topologyCheck.error);
    out = std::move(parsed); // failed loads leave caller's asset intact
    return true;
}

struct StateChange {
    uint32_t from = UINT32_MAX, to = UINT32_MAX;
};
class StateMachineInstance {
public:
    explicit StateMachineInstance(const StateMachineAsset& graph)
        : graph_(&graph), current_(graph.entry) {
        for (const auto& p : graph.parameters)
            values_.push_back(p.initial);
    }
    [[nodiscard]] uint32_t Current() const { return current_; }
    [[nodiscard]] std::string_view CurrentName() const {
        return current_ < graph_->states.size() ? graph_->states[current_] : "";
    }
    [[nodiscard]] bool SetBool(std::string_view name, bool value) {
        return Set(name, StateParamKind::Bool, value ? 1.0 : 0.0);
    }
    [[nodiscard]] bool SetInt(std::string_view name, int32_t value) {
        return Set(name, StateParamKind::Integer, double(value));
    }
    [[nodiscard]] bool SetFloat(std::string_view name, double value) {
        return std::isfinite(value) && Set(name, StateParamKind::Float, value);
    }
    [[nodiscard]] bool Fire(std::string_view name) {
        return Set(name, StateParamKind::Trigger, 1.0);
    }
    // No callbacks are run: the caller interprets the returned transition.
    [[nodiscard]] std::optional<StateChange> Tick() {
        std::optional<StateChange> change;
        for (const auto& t : graph_->transitions) {
            if (t.from != UINT32_MAX && t.from != current_) continue;
            const double value = values_[t.parameter];
            bool matches = false;
            switch (t.compare) {
            case StateCompare::Equal: matches = value == t.threshold; break;
            case StateCompare::NotEqual: matches = value != t.threshold; break;
            case StateCompare::Less: matches = value < t.threshold; break;
            case StateCompare::LessEqual: matches = value <= t.threshold; break;
            case StateCompare::Greater: matches = value > t.threshold; break;
            case StateCompare::GreaterEqual: matches = value >= t.threshold; break;
            case StateCompare::Fired: matches = value != 0.0; break;
            }
            if (!matches) continue;
            change = StateChange{current_, t.to};
            current_ = t.to;
            break;
        }
        for (size_t i = 0; i < graph_->parameters.size(); ++i)
            if (graph_->parameters[i].kind == StateParamKind::Trigger)
                values_[i] = 0.0;
        return change;
    }
private:
    bool Set(std::string_view name, StateParamKind kind, double value) {
        const uint32_t index = graph_->Parameter(name);
        if (index == UINT32_MAX || graph_->parameters[index].kind != kind)
            return false;
        values_[index] = value;
        return true;
    }
    const StateMachineAsset* graph_ = nullptr; // borrowed; asset outlives instance
    uint32_t current_ = UINT32_MAX;
    std::vector<double> values_;
};

} // namespace tour::asset
