// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Asset/navmesh.h>
#include <Engine/Asset/state_machine.h>
#include <Engine/Asset/state_machine_writer.h>
#include <Engine/Graph/graph.h>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;
static void Check(bool value, const char* label) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
static void StateTests() {
    tour::asset::StateMachineAsset graph;
    std::string err;
    const char* doc = R"(statemachine 1
transition idle move speed > 0.1
transition move idle speed <= 0.1
transition * dead die fired
entry idle
state idle
state move
state dead
param speed float 0
param die trigger
)";
    Check(tour::asset::ParseStateMachineText(doc, graph, err), "state.parse");
    if (!err.empty()) {
        std::cerr << err << '\n';
        return;
    }
    Check(graph.states.size() == 3, "state.states");
    tour::asset::StateMachineInstance a(graph);
    tour::asset::StateMachineInstance b(graph);
    Check(a.CurrentName() == "idle", "state.entry");
    Check(a.SetFloat("speed", 0.4), "state.setfloat");
    auto change = a.Tick();
    Check(change && change->from == 0 && change->to == 1, "state.transition");
    Check(a.CurrentName() == "move" && b.CurrentName() == "idle", "state.independent");
    Check(a.Fire("die"), "state.fire");
    change = a.Tick();
    Check(change && a.CurrentName() == "dead", "state.any");
    Check(!a.Tick(), "state.trigger consumed");
    Check(!a.SetBool("speed", true), "state.typed-param");
    Check(!a.SetFloat("speed", std::numeric_limits<double>::infinity()), "state.nan");
    // The editor emits this canonical text before writing it. Reparse
    // through the real runtime parser, including bool/int/trigger types.
    graph.parameters.push_back({"on", tour::asset::StateParamKind::Bool, 1});
    graph.parameters.push_back({"lives", tour::asset::StateParamKind::Integer, 3});
    graph.transitions.push_back({0, 2, 2, tour::asset::StateCompare::Equal, 1});
    graph.transitions.push_back({1, 0, 3, tour::asset::StateCompare::LessEqual, 2});
    const std::string authored = tour::asset::SerializeStateMachineText(graph);
    tour::asset::StateMachineAsset restored;
    Check(!authored.empty() && tour::asset::ParseStateMachineText(authored, restored, err) &&
              restored.states == graph.states && restored.entry == graph.entry &&
              restored.transitions.size() == graph.transitions.size() &&
              restored.parameters.size() == graph.parameters.size() &&
              restored.parameters[2].kind == tour::asset::StateParamKind::Bool &&
              restored.parameters[3].kind == tour::asset::StateParamKind::Integer &&
              restored.transitions.back().threshold == 2,
          "state.editor-authoring-roundtrip");
    if (restored.states.size() == 3) {
        tour::asset::StateMachineInstance instance(restored);
        Check(instance.CurrentName() == "idle" && instance.SetFloat("speed", 0.8) &&
                  instance.Tick().has_value() && instance.CurrentName() == "move",
              "state.editor-output-runtime");
    }
    // Layout is stored as comments, not a second asset format.
    restored = {};
    Check(tour::asset::ParseStateMachineText(
              authored + "# editor-pos idle 12.5 -8\n", restored, err) &&
              restored.states.size() == 3,
          "state.editor-layout-comment");

    auto reject = [&](const char* source) {
        Check(!tour::asset::ParseStateMachineText(source, graph, err) && !err.empty() &&
                  graph.states.size() == 3,
              "state.bad");
    };
    reject("statemachine 1\nstate idle\nstate idle\nentry idle\n");
    reject("statemachine 1\nstate idle\nentry other\n");
    reject("statemachine 1\nstate idle\nentry idle\nparam x float nan\n");
    reject("statemachine 1\nstate idle\nentry idle\ntransition idle idle x > 1\n");
    reject("statemachine 1\nstate idle\nentry idle\nparam t trigger\ntransition idle idle t > 1\n");
}
static void GraphTests() {
    using namespace tour::graph;
    Document graph;
    Node a;
    a.id = 1;
    a.type = "source";
    a.ports.push_back({"out", "field", PortDirection::Output});
    Node b;
    b.id = 2;
    b.type = "filter";
    b.ports.push_back({"in", "field", PortDirection::Input});
    b.ports.push_back({"out", "field", PortDirection::Output});
    graph.nodes.emplace(1, a);
    graph.nodes.emplace(2, b);
    graph.links.emplace(1, Link{1, 1, 2, "out", "in"});
    auto v = Validate(graph);
    Check(v.ok && v.order.size() == 2 && v.order[0] == 1, "graph.topology");
    graph.links.emplace(2, Link{2, 1, 2, "out", "in"});
    Check(!Validate(graph).ok, "graph.single-input");
    graph.links.erase(2);
    graph.nodes[1].ports.push_back({"in", "field", PortDirection::Input});
    graph.links.emplace(2, Link{2, 2, 1, "out", "in"});
    Check(!Validate(graph).ok && Validate(graph).error == "cycle in graph", "graph.cycle");
    Check(Validate(graph, ValidationPolicy{true, false}).ok, "graph.allow-cycle");
    graph.links[2].input = "missing";
    Check(!Validate(graph, ValidationPolicy{true, false}).ok, "graph.dangling");
}
static void NavTests() {
    tour::asset::NavMeshAsset nav;
    std::string err;
    const char* doc = R"(navmesh 1
vertex 0 0 0
vertex 1 0 0
vertex 1 0 1
vertex 0 0 1
triangle 0 1 2
triangle 0 2 3 2.0
)";
    Check(tour::asset::ParseNavMeshText(doc, nav, err), "nav.parse");
    Check(nav.triangles.size() == 2 && nav.triangles[0].neighbors == std::vector<uint32_t>{1},
          "nav.adjacency");
    std::vector<tour::asset::NavPoint> path;
    Check(tour::asset::FindNavPath(nav, {0.9, 0, 0.1}, {0.1, 0, 0.9}, path) && path.size() == 4,
          "nav.path");
    Check(!tour::asset::FindNavPath(nav, {4, 0, 4}, {0.1, 0, 0.9}, path), "nav.outside");
    auto reject = [&](const char* source) {
        Check(!tour::asset::ParseNavMeshText(source, nav, err) && !err.empty() &&
                  nav.triangles.size() == 2,
              "nav.bad");
    };
    reject("navmesh 1\nvertex 0 0 0\nvertex 1 0 0\ntriangle 0 1 2\n");
    reject("navmesh 1\nvertex 0 0 0\nvertex 1 0 0\nvertex 2 0 0\ntriangle 0 1 2\n");
    reject("navmesh 1\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 0 1\ntriangle 0 1 2 0\n");
    reject("navmesh 1\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 0 1\ntriangle 0 1 2\ntriangle 0 1 "
           "2\ntriangle 0 1 2\n");
}
int main() {
    StateTests();
    GraphTests();
    NavTests();
    if (failures)
        std::cerr << failures << " failures\n";
    else
        std::cout << "GameplayAssetTests passed\n";
    return failures ? 1 : 0;
}
