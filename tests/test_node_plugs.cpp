// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless Maya-style typed node/component plug + DG topology tests.
#include <Engine/Scene/node_plugs.h>
#include <cassert>
#include <cmath>

using namespace tour::scene;
namespace plugs = tour::scene::plugs;

int main() {
    Scene scene;
    const uint32_t a = uint32_t(addNode(scene, -1, 0));
    const uint32_t b = uint32_t(addNode(scene, -1, 0));
    const uint32_t c = uint32_t(addNode(scene, -1, 0));
    assert(SetPhysicsBody(scene, a, ScenePhysicsBody{}));
    assert(SetPhysicsBody(scene, b, ScenePhysicsBody{}));
    assert(SetPhysicsBody(scene, c, ScenePhysicsBody{}));
    assert(SetTriggerVolume(scene, b, SceneTriggerVolume{}));
    assert(SetGiVolume(scene, a, SceneGiVolume{}));
    assert(plugs::Find("physics.mass")->kind == plugs::Kind::Number);
    assert(plugs::Find("trigger.tag")->kind == plugs::Kind::String);
    assert(!plugs::Find("no.such.attribute"));
    assert(!plugs::ValidValue("physics.mass", plugs::Value::Bool(true)));
    assert(!plugs::ValidValue("physics.mass", plugs::Value::Number(NAN)));
    assert(plugs::Write(scene,a,"physics.mass",plugs::Value::Number(17.5)));
    plugs::Value value;
    assert(plugs::Read(scene,a,"physics.mass",value));
    assert(value == plugs::Value::Number(17.5));
    assert(!plugs::Write(scene,b,"physics.mass",plugs::Value::Bool(false)));
    assert(plugs::Read(scene,b,"physics.mass",value) &&
        value == plugs::Value::Number(1.0));
    assert(plugs::Write(scene,b,"trigger.tag",plugs::Value::String("door A")));
    assert(plugs::Read(scene,b,"trigger.tag",value) &&
        value == plugs::Value::String("door A"));
    assert(!plugs::Write(scene,c,"trigger.tag",plugs::Value::String("not attached")));
    assert(plugs::Write(scene,a,"gi.spacing",plugs::Value::Number(0.75)));
    assert(plugs::Read(scene,a,"gi.spacing",value) && value.number == 0.75);
    assert(plugs::Write(scene,a,"node.static",plugs::Value::Bool(true)));
    assert(plugs::Read(scene,a,"node.static",value) && value.boolean);
    assert(plugs::Write(scene,a,"node.static",plugs::Value::Bool(false)));

    // Per-instance endpoints: one destination source, type safety,
    // downstream dependency propagation, and cycle prevention.
    plugs::Graph graph;
    assert(graph.Connect({{a,"physics.mass"},{b,"physics.mass"}}));
    assert(graph.Connect({{b,"physics.mass"},{c,"physics.mass"}}));
    assert(!graph.Connect({{c,"physics.mass"},{a,"physics.mass"}}));
    assert(!graph.Connect({{a,"physics.mass"},{b,"physics.sensor"}}));
    assert(!graph.Connect({{a,"physics.mass"},{a,"physics.mass"}}));
    assert(graph.Connections().size() == 2);
    assert(graph.Evaluate(scene));
    assert(plugs::Read(scene,c,"physics.mass",value) &&
        value == plugs::Value::Number(17.5));

    // Reconnect replaces exactly one destination, leaving other links
    // untouched; disconnect only severs that destination plug.
    assert(graph.Connect({{a,"physics.mass"},{c,"physics.mass"}}));
    assert(graph.Connections().size() == 2);
    graph.Disconnect({c,"physics.mass"});
    assert(graph.Connections().size() == 1);
    assert(graph.Evaluate(scene));
    return 0;
}
