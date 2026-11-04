// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Editor node/plug schema (Maya-style shape/transform separation).
// Stable plug paths are distinct from display labels and ImGui widget IDs.
// This is an editor adapter over existing Scene tables + EditCmd commands,
// NOT a replacement for the scene graph or a new .tour serialization format.
#include <array>
#include <span>
#include <string_view>

namespace editor::plugs {

enum class Kind { Boolean, Integer, Float, Enum, VectorComponent, ColorComponent,
    Resource, String };
enum class NodeType { Transform, Mesh, Material, Light, GiVolume,
    Cubemap, Camera, AudioSource, AudioRoom, PhysicsBody, TriggerVolume,
    Terrain, Environment, ExportSettings, Image, Locator };

struct Descriptor {
    std::string_view path; // persistent, machine-readable ("transform.scale.x")
    std::string_view label; // editor presentation ("Scale X")
    Kind kind;
    bool overridable = true;
};
struct Type {
    NodeType id;
    std::string_view name;
    std::span<const Descriptor> properties;
};

// A transform is a DAG node; every transform channel has its own stable
// plug rather than one overloaded matrix-wide "Transform" identifier.
inline constexpr std::array transform = {
    Descriptor{"node.name", "Name", Kind::String},
    Descriptor{"transform.translate.x", "Translate X", Kind::VectorComponent},
    Descriptor{"transform.translate.y", "Translate Y", Kind::VectorComponent},
    Descriptor{"transform.translate.z", "Translate Z", Kind::VectorComponent},
    Descriptor{"transform.rotate.x", "Rotate X", Kind::VectorComponent},
    Descriptor{"transform.rotate.y", "Rotate Y", Kind::VectorComponent},
    Descriptor{"transform.rotate.z", "Rotate Z", Kind::VectorComponent},
    Descriptor{"transform.scale.x", "Scale X", Kind::VectorComponent},
    Descriptor{"transform.scale.y", "Scale Y", Kind::VectorComponent},
    Descriptor{"transform.scale.z", "Scale Z", Kind::VectorComponent},
    Descriptor{"node.visibility", "Visibility", Kind::Boolean},
    Descriptor{"node.static", "Static", Kind::Boolean},
    Descriptor{"node.lightmap", "Bake lightmaps", Kind::Boolean},
    Descriptor{"node.noLightmap", "Exclude lightmaps", Kind::Boolean},
    Descriptor{"node.ditherCloseup", "Dither closeup", Kind::Boolean},
    Descriptor{"node.volumetricShadow", "Volumetric shadow", Kind::Boolean},
    Descriptor{"node.environment", "Environment node", Kind::Boolean},
    Descriptor{"node.material", "Material", Kind::Resource},
};
inline constexpr std::array material = {
    Descriptor{"material.doubleSided", "Double sided", Kind::Boolean},
    Descriptor{"material.castShadow", "Cast shadows", Kind::Boolean},
    Descriptor{"material.receiveShadow", "Receive shadows", Kind::Boolean},
    Descriptor{"material.alphaMode", "Alpha mode", Kind::Enum},
    Descriptor{"material.baseColor.r", "Base color R", Kind::ColorComponent},
    Descriptor{"material.baseColor.g", "Base color G", Kind::ColorComponent},
    Descriptor{"material.baseColor.b", "Base color B", Kind::ColorComponent},
    Descriptor{"material.metallic", "Metallic", Kind::Float},
    Descriptor{"material.roughness", "Roughness", Kind::Float},
    Descriptor{"material.layered", "Layered material", Kind::Boolean},
};
inline constexpr std::array light = {
    Descriptor{"light.type", "Light type", Kind::Enum},
    Descriptor{"light.intensity", "Intensity", Kind::Float},
    Descriptor{"light.color.r", "Light color R", Kind::ColorComponent},
    Descriptor{"light.color.g", "Light color G", Kind::ColorComponent},
    Descriptor{"light.color.b", "Light color B", Kind::ColorComponent},
    Descriptor{"light.range", "Range", Kind::Float},
    Descriptor{"light.innerCone", "Inner cone", Kind::Float},
    Descriptor{"light.outerCone", "Outer cone", Kind::Float},
};
inline constexpr std::array giVolume = {
    Descriptor{"gi.enabled", "Enabled", Kind::Boolean},
    Descriptor{"gi.spacing", "Probe spacing", Kind::Float},
    Descriptor{"gi.priority", "Priority", Kind::Integer},
    Descriptor{"gi.fadeDistance", "Fade distance", Kind::Float},
};
inline constexpr std::array camera = {
    Descriptor{"camera.projection", "Projection", Kind::Enum},
    Descriptor{"camera.fovY", "Field of view", Kind::Float},
    Descriptor{"camera.near", "Near plane", Kind::Float},
    Descriptor{"camera.far", "Far plane", Kind::Float},
};
inline constexpr std::array cubemap = {
    Descriptor{"cubemap.radius", "Radius", Kind::Float},
    Descriptor{"cubemap.resolution", "Resolution", Kind::Integer},
};
inline constexpr std::array audio = {
    Descriptor{"audio.gain", "Volume", Kind::Float},
    Descriptor{"audio.spatial", "Spatial", Kind::Boolean},
    Descriptor{"audio.playOnLoad", "Play on load", Kind::Boolean},
};
inline constexpr std::array physics = {
    Descriptor{"physics.enabled", "Enabled", Kind::Boolean},
    Descriptor{"physics.mass", "Mass", Kind::Float},
};
inline constexpr std::array trigger = {
    Descriptor{"trigger.enabled", "Enabled", Kind::Boolean},
    Descriptor{"trigger.once", "Once", Kind::Boolean},
    Descriptor{"trigger.tag", "Tag", Kind::String},
};
inline constexpr std::array empty = std::array<Descriptor, 0>{};

inline constexpr std::array types = {
    Type{NodeType::Transform, "transform", transform},
    Type{NodeType::Mesh, "mesh", transform},
    Type{NodeType::Material, "material", material},
    Type{NodeType::Light, "light", light},
    Type{NodeType::GiVolume, "giVolume", giVolume},
    Type{NodeType::Cubemap, "cubemap", cubemap},
    Type{NodeType::Camera, "camera", camera},
    Type{NodeType::AudioSource, "audioSource", audio},
    Type{NodeType::AudioRoom, "audioRoom", empty},
    Type{NodeType::PhysicsBody, "physicsBody", physics},
    Type{NodeType::TriggerVolume, "triggerVolume", trigger},
    Type{NodeType::Terrain, "terrain", empty},
    Type{NodeType::Environment, "environment", transform},
    Type{NodeType::ExportSettings, "exportSettings", empty},
    Type{NodeType::Image, "image", empty},
    Type{NodeType::Locator, "locator", transform},
};

inline const Type* FindType(NodeType type) {
    for (const Type& t : types)
        if (t.id == type) return &t;
    return nullptr;
}
inline const Descriptor* Find(NodeType type, std::string_view path) {
    const Type* t = FindType(type);
    if (!t) return nullptr;
    for (const Descriptor& p : t->properties)
        if (p.path == path) return &p;
    return nullptr;
}
inline const Descriptor* FindByLabel(NodeType type, std::string_view label) {
    const Type* t = FindType(type);
    if (!t) return nullptr;
    for (const Descriptor& p : t->properties)
        if (p.label == label) return &p;
    return nullptr;
}
} // namespace editor::plugs
