// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Authoring-side, typed DAG plugs. Stable plug paths are API keys, never
// display labels. A connection is a directed source->destination plug edge;
// it is not an editor-widget link or a renderer resource handle.
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tour::scene::plugs {

enum class Kind : uint8_t { Boolean, Number, String };
struct Value {
    Kind kind = Kind::Number;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    static Value Bool(bool v) {
        Value x;
        x.kind = Kind::Boolean;
        x.boolean = v;
        return x;
    }
    static Value Number(double v) {
        Value x;
        x.kind = Kind::Number;
        x.number = v;
        return x;
    }
    static Value String(std::string v) {
        Value x;
        x.kind = Kind::String;
        x.text = std::move(v);
        return x;
    }
};
inline bool operator==(const Value& a, const Value& b) {
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
    case Kind::Boolean:
        return a.boolean == b.boolean;
    case Kind::Number:
        return a.number == b.number;
    case Kind::String:
        return a.text == b.text;
    }
    return false;
}
inline bool operator!=(const Value& a, const Value& b) {
    return !(a == b);
}

struct Descriptor {
    std::string_view path;
    Kind kind;
    std::string_view nodeType;
};
// This table is the source of truth for plugs that have an actual
// scene-table read/write implementation. Do not advertise stubs here.
inline constexpr Descriptor kDescriptors[] = {
    {"node.visibility", Kind::Boolean, "transform"},
    {"node.static", Kind::Boolean, "transform"},
    {"node.lightmap", Kind::Boolean, "transform"},
    {"node.noLightmap", Kind::Boolean, "transform"},
    {"node.ditherCloseup", Kind::Boolean, "transform"},
    {"node.volumetricShadow", Kind::Boolean, "transform"},
    {"node.material", Kind::Number, "transform"},
    {"gi.spacing", Kind::Number, "giVolume"},
    {"gi.priority", Kind::Number, "giVolume"},
    {"gi.insideFade", Kind::Number, "giVolume"},
    {"gi.outsideFade", Kind::Number, "giVolume"},
    {"cubemap.radius", Kind::Number, "cubemap"},
    {"audio.gain", Kind::Number, "audioSource"},
    {"audio.spatial", Kind::Boolean, "audioSource"},
    {"audio.playOnLoad", Kind::Boolean, "audioSource"},
    {"audio.minDistance", Kind::Number, "audioSource"},
    {"audio.maxDistance", Kind::Number, "audioSource"},
    {"audioRoom.enabled", Kind::Boolean, "audioRoom"},
    {"audioRoom.priority", Kind::Number, "audioRoom"},
    {"audioRoom.wet", Kind::Number, "audioRoom"},
    {"audioRoom.rt60", Kind::Number, "audioRoom"},
    {"audioRoom.damping", Kind::Number, "audioRoom"},
    {"physics.motion", Kind::Number, "physicsBody"},
    {"physics.shape", Kind::Number, "physicsBody"},
    {"physics.mass", Kind::Number, "physicsBody"},
    {"physics.friction", Kind::Number, "physicsBody"},
    {"physics.restitution", Kind::Number, "physicsBody"},
    {"physics.radius", Kind::Number, "physicsBody"},
    {"physics.height", Kind::Number, "physicsBody"},
    {"physics.sensor", Kind::Boolean, "physicsBody"},
    {"trigger.enabled", Kind::Boolean, "triggerVolume"},
    {"trigger.once", Kind::Boolean, "triggerVolume"},
    {"trigger.tag", Kind::String, "triggerVolume"},
};
inline const Descriptor* Find(std::string_view path) {
    for (const auto& d : kDescriptors)
        if (d.path == path)
            return &d;
    return nullptr;
}
inline bool ValidValue(std::string_view path, const Value& v) {
    const Descriptor* d = Find(path);
    return d && d->kind == v.kind && (v.kind != Kind::Number || std::isfinite(v.number));
}
inline bool Read(const Scene& scene, uint32_t node, std::string_view key, Value& out) {
    if (node >= scene.hierarchy.size() || !Find(key))
        return false;
    auto boolean = [&](bool v) {
        out = Value::Bool(v);
        return true;
    };
    auto number = [&](double v) {
        out = Value::Number(v);
        return true;
    };
    if (key == "node.visibility")
        return boolean(node >= scene.hidden.size() || scene.hidden[node] == 0);
    if (key == "node.static")
        return boolean(node < scene.staticRoot.size() && scene.staticRoot[node] != 0);
    if (key == "node.lightmap")
        return boolean(node < scene.lightmap.size() && scene.lightmap[node] != 0);
    if (key == "node.noLightmap")
        return boolean(node < scene.noLightmap.size() && scene.noLightmap[node] != 0);
    if (key == "node.ditherCloseup")
        return boolean(node < scene.ditherCloseup.size() && scene.ditherCloseup[node] != 0);
    if (key == "node.volumetricShadow")
        return boolean(IsCastsVolumetricShadowNode(scene, int(node)));
    if (key == "node.material") {
        const auto* row = FindMaterial(scene, node);
        return number(row ? row->material : UINT32_MAX);
    }
    if (const auto* row = FindGiVolume(scene, node)) {
        const auto& x = row->volume;
        if (key == "gi.spacing")
            return number(x.spacing);
        if (key == "gi.priority")
            return number(x.priority);
        if (key == "gi.insideFade")
            return number(x.insideFade);
        if (key == "gi.outsideFade")
            return number(x.outsideFade);
    }
    if (const auto* row = FindCubemap(scene, node)) {
        if (key == "cubemap.radius")
            return number(row->cubemap.radius);
    }
    if (const auto* row = FindAudioSource(scene, node)) {
        const auto& x = row->source;
        if (key == "audio.gain")
            return number(x.volume);
        if (key == "audio.spatial")
            return boolean(x.spatial);
        if (key == "audio.playOnLoad")
            return boolean(x.playOnLoad);
        if (key == "audio.minDistance")
            return number(x.minDistance);
        if (key == "audio.maxDistance")
            return number(x.maxDistance);
    }
    if (const auto* row = FindAudioRoom(scene, node)) {
        const auto& x = row->room;
        if (key == "audioRoom.enabled")
            return boolean(x.enabled);
        if (key == "audioRoom.priority")
            return number(x.priority);
        if (key == "audioRoom.wet")
            return number(x.wet);
        if (key == "audioRoom.rt60")
            return number(x.rt60);
        if (key == "audioRoom.damping")
            return number(x.damping);
    }
    if (const auto* row = FindPhysicsBody(scene, node)) {
        const auto& x = row->body;
        if (key == "physics.motion")
            return number(x.motion);
        if (key == "physics.shape")
            return number(x.shape);
        if (key == "physics.mass")
            return number(x.mass);
        if (key == "physics.friction")
            return number(x.friction);
        if (key == "physics.restitution")
            return number(x.restitution);
        if (key == "physics.radius")
            return number(x.radius);
        if (key == "physics.height")
            return number(x.height);
        if (key == "physics.sensor")
            return boolean(x.sensor);
    }
    if (const auto* row = FindTriggerVolume(scene, node)) {
        const auto& x = row->trigger;
        if (key == "trigger.enabled")
            return boolean(x.enabled);
        if (key == "trigger.once")
            return boolean(x.once);
        if (key == "trigger.tag") {
            out = Value::String(x.tag);
            return true;
        }
    }
    return false;
}
inline bool Write(Scene& scene, uint32_t node, std::string_view key, const Value& v) {
    if (node >= scene.hierarchy.size() || !ValidValue(key, v))
        return false;
    if (key == "node.visibility") {
        if (v.boolean)
            ClearHidden(scene, int(node));
        else
            MarkHidden(scene, int(node));
        return true;
    }
    if (key == "node.static") {
        if (v.boolean)
            MarkSubtreeStatic(scene, int(node));
        else
            ClearSubtreeStatic(scene, int(node));
        return true;
    }
    if (key == "node.lightmap") {
        if (v.boolean)
            MarkLightmap(scene, int(node));
        else
            ClearLightmap(scene, int(node));
        return true;
    }
    if (key == "node.noLightmap") {
        if (v.boolean)
            MarkNoLightmap(scene, int(node));
        else
            ClearNoLightmap(scene, int(node));
        return true;
    }
    if (key == "node.ditherCloseup") {
        if (v.boolean)
            MarkDitherCloseup(scene, int(node));
        else
            ClearDitherCloseup(scene, int(node));
        return true;
    }
    if (key == "node.volumetricShadow") {
        if (v.boolean)
            MarkCastsVolumetricShadow(scene, int(node));
        else
            ClearCastsVolumetricShadow(scene, int(node));
        return true;
    }
    if (key == "node.material") {
        if (v.number < 0 || v.number > double(UINT32_MAX) || std::floor(v.number) != v.number)
            return false;
        const uint32_t id = uint32_t(v.number);
        return id == UINT32_MAX ? (RemoveMaterial(scene, node), true)
                                : SetMaterial(scene, node, id);
    }
    if (auto* row = FindGiVolume(scene, node)) {
        auto x = row->volume;
        if (key == "gi.spacing")
            x.spacing = float(v.number);
        else if (key == "gi.priority") {
            if (v.number < 0 || v.number > UINT32_MAX || std::floor(v.number) != v.number)
                return false;
            x.priority = uint32_t(v.number);
        } else if (key == "gi.insideFade")
            x.insideFade = float(v.number);
        else if (key == "gi.outsideFade")
            x.outsideFade = float(v.number);
        else
            goto notGi;
        return SetGiVolume(scene, node, x);
    }
notGi:
    if (auto* row = FindCubemap(scene, node)) {
        auto x = row->cubemap;
        if (key == "cubemap.radius") {
            x.radius = float(v.number);
            return SetCubemap(scene, node, x);
        }
    }
    if (auto* row = FindAudioSource(scene, node)) {
        auto x = row->source;
        if (key == "audio.gain")
            x.volume = float(v.number);
        else if (key == "audio.spatial")
            x.spatial = v.boolean;
        else if (key == "audio.playOnLoad")
            x.playOnLoad = v.boolean;
        else if (key == "audio.minDistance")
            x.minDistance = float(v.number);
        else if (key == "audio.maxDistance")
            x.maxDistance = float(v.number);
        else
            goto notAudio;
        return SetAudioSource(scene, node, x);
    }
notAudio:
    if (auto* row = FindAudioRoom(scene, node)) {
        auto x = row->room;
        if (key == "audioRoom.enabled")
            x.enabled = v.boolean;
        else if (key == "audioRoom.priority") {
            if (v.number < 0 || v.number > UINT32_MAX || std::floor(v.number) != v.number)
                return false;
            x.priority = uint32_t(v.number);
        } else if (key == "audioRoom.wet")
            x.wet = float(v.number);
        else if (key == "audioRoom.rt60")
            x.rt60 = float(v.number);
        else if (key == "audioRoom.damping")
            x.damping = float(v.number);
        else
            goto notRoom;
        return SetAudioRoom(scene, node, x);
    }
notRoom:
    if (auto* row = FindPhysicsBody(scene, node)) {
        auto x = row->body;
        if (key == "physics.motion") {
            if (v.number < 0 || v.number > 2 || std::floor(v.number) != v.number)
                return false;
            x.motion = uint32_t(v.number);
        } else if (key == "physics.shape") {
            if (v.number < 0 || v.number > 2 || std::floor(v.number) != v.number)
                return false;
            x.shape = uint32_t(v.number);
        } else if (key == "physics.mass")
            x.mass = float(v.number);
        else if (key == "physics.friction")
            x.friction = float(v.number);
        else if (key == "physics.restitution")
            x.restitution = float(v.number);
        else if (key == "physics.radius")
            x.radius = float(v.number);
        else if (key == "physics.height")
            x.height = float(v.number);
        else if (key == "physics.sensor")
            x.sensor = v.boolean;
        else
            goto notPhysics;
        return SetPhysicsBody(scene, node, x);
    }
notPhysics:
    if (auto* row = FindTriggerVolume(scene, node)) {
        auto x = row->trigger;
        if (key == "trigger.enabled")
            x.enabled = v.boolean;
        else if (key == "trigger.once")
            x.once = v.boolean;
        else if (key == "trigger.tag")
            x.tag = v.text;
        else
            return false;
        return SetTriggerVolume(scene, node, x);
    }
    return false;
}

// Source/destination plug graph: authoring edges are explicitly typed,
// unique per destination, and cycle checked before any scene mutation.
// Evaluation applies a deterministic topological order to the scene.
struct Endpoint {
    uint32_t node = 0;
    std::string plug;
};
struct Connection {
    Endpoint source;
    Endpoint destination;
};
class Graph {
    std::vector<Connection> edges_;

  public:
    const std::vector<Connection>& Connections() const {
        return edges_;
    }
    bool Connect(const Connection& e) {
        const Descriptor *src = Find(e.source.plug), *dst = Find(e.destination.plug);
        if (!src || !dst || src->kind != dst->kind ||
            (e.source.node == e.destination.node && e.source.plug == e.destination.plug))
            return false;
        // DFS over destination-to-source dependencies, not just node IDs:
        // two different plugs on a node can legitimately be connected.
        std::vector<Endpoint> pending{e.destination};
        std::vector<Endpoint> visited;
        auto eq = [](const Endpoint& a, const Endpoint& b) {
            return a.node == b.node && a.plug == b.plug;
        };
        while (!pending.empty()) {
            Endpoint cur = std::move(pending.back());
            pending.pop_back();
            if (eq(cur, e.source))
                return false;
            if (std::any_of(
                    visited.begin(), visited.end(), [&](const Endpoint& x) { return eq(x, cur); }))
                continue;
            visited.push_back(cur);
            for (const auto& edge : edges_)
                if (eq(edge.source, cur))
                    pending.push_back(edge.destination);
        }
        // Maya-style single incoming connection: reconnect replaces it.
        for (auto it = edges_.begin(); it != edges_.end();) {
            if (eq(it->destination, e.destination))
                it = edges_.erase(it);
            else
                ++it;
        }
        edges_.push_back(e);
        return true;
    }
    void Disconnect(const Endpoint& destination) {
        edges_.erase(std::remove_if(edges_.begin(),
                                    edges_.end(),
                                    [&](const Connection& e) {
                                        return e.destination.node == destination.node &&
                                               e.destination.plug == destination.plug;
                                    }),
                     edges_.end());
    }
    bool Evaluate(Scene& scene) const {
        // Resolve edges by repeatedly consuming those with no pending
        // inbound dependency. Cycles cannot be added through Connect().
        std::vector<bool> done(edges_.size(), false);
        for (size_t pass = 0; pass < edges_.size(); ++pass) {
            bool progress = false;
            for (size_t i = 0; i < edges_.size(); ++i) {
                if (done[i])
                    continue;
                const auto& e = edges_[i];
                const bool blocked =
                    std::any_of(edges_.begin(), edges_.end(), [&](const Connection& other) {
                        const size_t k = size_t(&other - edges_.data());
                        return !done[k] && k != i && other.destination.node == e.source.node &&
                               other.destination.plug == e.source.plug;
                    });
                if (blocked)
                    continue;
                Value v;
                if (!Read(scene, e.source.node, e.source.plug, v) ||
                    !Write(scene, e.destination.node, e.destination.plug, v))
                    return false;
                done[i] = true;
                progress = true;
            }
            if (!progress)
                return false;
        }
        return true;
    }
};
} // namespace tour::scene::plugs
