// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Scene/scene_memory_bindings.h>
#include <cassert>
#include <cstdint>

int main() {
    using namespace tour::scene;
    using namespace tour::scene::memory;

    Scene scene;
    auto empty = GetView(scene, Semantic::Hierarchy);
    assert(empty.Empty());
    assert(empty.data == nullptr);
    assert(empty.stride == sizeof(Hierarchy));

    // This probe exercises direct scene data only; no GPU is needed.
    scene.hierarchy.resize(2);
    scene.localTransform.resize(2);
    scene.globalTransform.resize(2);
    scene.nodeGeneration.resize(2);
    scene.hidden.resize(2);
    scene.tables.meshes.resize(1);
    scene.hierarchy[1].parent = 0;

    const auto hierarchy = GetView(scene, Semantic::Hierarchy);
    assert(hierarchy.data == scene.hierarchy.data());
    assert(hierarchy.count == 2 && hierarchy.stride == sizeof(Hierarchy));
    assert(hierarchy.ownership == Ownership::Borrowed);
    assert(hierarchy.access == Access::ReadOnly);
    assert(HierarchyFields[0].offset == offsetof(Hierarchy, parent));
    assert(HierarchyFields[1].offset == offsetof(Hierarchy, firstChild));
    const auto* entries = static_cast<const Hierarchy*>(hierarchy.data);
    assert(entries[1].parent == 0);

    auto local = GetView(scene, Semantic::LocalTransform);
    assert(local.data == scene.localTransform.data());
    assert(local.stride == sizeof(glm::mat4));
    assert(GetView(scene, Semantic::Hidden).format == ScalarFormat::UInt8);
    assert(GetView(scene, Semantic::MeshRows).count == 1);
    assert(GetView(scene, Semantic::NodeGeneration).format == ScalarFormat::UInt32);

    alignas(16) float external[32]{};
    ExternalBinding b { Semantic::LocalTransform, external, 2, 2,
                        sizeof(glm::mat4), alignof(glm::mat4) };
    assert(Validate(b));
    b.count = 3;
    assert(!Validate(b));
    b.count = 2;
    b.stride = 0;
    assert(!Validate(b));
    return 0;
}
