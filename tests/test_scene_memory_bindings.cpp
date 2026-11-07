// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Scene/gpu_scene_memory_bindings.h>
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
    ExternalBinding b{
        Semantic::LocalTransform, external, 2, 2, sizeof(glm::mat4), alignof(glm::mat4)};
    assert(Validate(b));
    b.count = 3;
    assert(!Validate(b));
    b.count = 2;
    b.stride = 0;
    assert(!Validate(b));

    // Semantic registry includes empty streams and reports field layouts.
    size_t visited = 0;
    ForEachView(scene, [&](View view, Layout layout) {
        assert(view.semantic == layout.semantic);
        ++visited;
    });
    assert(visited == AllSemantics.size());
    const Layout hierarchyLayout = Describe(scene, Semantic::Hierarchy);
    assert(hierarchyLayout.nodeIndexed && hierarchyLayout.externalBindingSupported);
    assert(hierarchyLayout.fieldCount == 5);
    assert(hierarchyLayout.fields[0].offset == offsetof(Hierarchy, parent));
    const Layout matrixLayout = Describe(scene, Semantic::LocalTransform);
    assert(matrixLayout.fieldCount == 4);
    assert(matrixLayout.fields[3].offset == 48);
    assert(matrixLayout.format == ScalarFormat::Mat4x4Float32);

    // Bind real caller-owned memory to all three authoritative streams.
    Scene bound;
    const int root = addNode(bound, -1, 0);
    const int child = addNode(bound, root, 1);
    alignas(16) glm::mat4 externalLocal[8]{};
    alignas(16) glm::mat4 externalGlobal[8]{};
    alignas(16) Hierarchy externalHierarchy[8]{};
    ExternalBinding localBind{Semantic::LocalTransform, externalLocal, 2, 8, sizeof(glm::mat4), 16};
    ExternalBinding globalBind{
        Semantic::GlobalTransform, externalGlobal, 2, 8, sizeof(glm::mat4), 16};
    ExternalBinding hierarchyBind{
        Semantic::Hierarchy, externalHierarchy, 2, 8, sizeof(Hierarchy), 16};
    assert(BindExternal(bound, localBind));
    assert(BindExternal(bound, globalBind));
    assert(BindExternal(bound, hierarchyBind));
    assert(bound.localTransform.data() == externalLocal);
    assert(bound.globalTransform.data() == externalGlobal);
    assert(bound.hierarchy.data() == externalHierarchy);
    assert(externalHierarchy[child].parent == root);
    assert(GetView(bound, Semantic::Hierarchy).ownership == Ownership::External);
    assert(GetView(bound, Semantic::LocalTransform).capacity == 8);

    const NodeHandle childHandle = MakeNodeHandle(bound, child);
    glm::mat4 translated(1.0f);
    translated[3].x = 7.0f;
    assert(SetLocalTransform(bound, childHandle, translated));
    assert(externalLocal[child][3].x == 7.0f);
    assert(recalculateGlobalTransformsDynamic(bound));
    assert(externalGlobal[child][3].x == 7.0f);

    // Rejected bindings must not modify the currently active buffers.
    ExternalBinding invalidBinding = localBind;
    invalidBinding.stride += 16;
    assert(!BindExternal(bound, invalidBinding));
    invalidBinding = localBind;
    invalidBinding.count = 1;
    assert(!BindExternal(bound, invalidBinding));
    invalidBinding = localBind;
    invalidBinding.access = Access::ReadOnly;
    assert(!BindExternal(bound, invalidBinding));
    assert(bound.localTransform.data() == externalLocal);

    assert(DetachExternal(bound, Semantic::LocalTransform));
    assert(DetachExternal(bound, Semantic::GlobalTransform));
    assert(DetachExternal(bound, Semantic::Hierarchy));
    assert(bound.localTransform.data() != externalLocal);
    assert(bound.globalTransform.data() != externalGlobal);
    assert(bound.hierarchy.data() != externalHierarchy);
    assert(bound.localTransform[child][3].x == 7.0f);
    assert(bound.hierarchy[child].parent == root);

    // Copying a bound scene must NOT carry another owner's borrowed memory.
    assert(BindExternal(bound, localBind));
    Scene copied(bound);
    assert(copied.localTransform.data() != externalLocal);
    assert(copied.localTransform[child][3].x == 7.0f);
    assert(DetachExternal(bound, Semantic::LocalTransform));

    // GPU tables describe their independently published per-flight addresses.
    tour::gpu_scene::RenderDatabase gpuDb;
    const auto meshId = gpuDb.meshes.Insert(tour::gpu_scene::Mesh{});
    assert(meshId);
    tour::gpu_scene::Addresses gpuAddresses{};
    gpuAddresses.meshes = 0x1000u; // fabricated device address for metadata test
    const auto gpuViews = GetGpuViews(gpuDb, gpuAddresses, 1);
    assert(gpuViews.size() == 6);
    assert(gpuViews[0].semantic == GpuSemantic::Mesh);
    assert(gpuViews[0].count == 1);
    assert(gpuViews[0].stride == sizeof(tour::gpu_scene::Row<tour::gpu_scene::Mesh>));
    assert(gpuViews[0].generationOffset ==
           offsetof(tour::gpu_scene::Row<tour::gpu_scene::Mesh>, identity));
    assert(gpuViews[0].aliveOffset == gpuViews[0].generationOffset + 4);
    assert(gpuViews[0].Published());
    assert(gpuViews[0].flight == 1);
    assert(!gpuViews[1].Published());
    return 0;
}
