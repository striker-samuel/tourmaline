// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless GPU Scene validation: view-space parity and struct ABI.
#include <Engine/Renderer/gpu_scene_validation.h>

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace tour::gpu_scene;

namespace {
void CheckDoubleView() {
    const glm::dvec3 eye(1.0e12, -1.0e12, 0.0);
    // This double world->clip puts the eye at clip origin, including
    // sub-metre offsets that cannot survive a float32 world VP.
    glm::dmat4 vp(1.0);
    vp[3] = glm::dvec4(-eye, 1.0);
    const RelativeFrustum frustum = DoubleRelativeFrustum(vp, eye);
    assert(frustum.Valid());
    InstanceCullGpuView packed{};
    assert(PackInstanceCullView(frustum, packed));
    assert(sizeof(packed) == 128);
    const glm::dvec3 rejoined = tour::math::JoinDoubleFloat(packed.origin);
    assert(glm::length(rejoined-eye) < 0.01);
    for (int i=0;i<6;++i)
        assert(std::isfinite(double(packed.inwardPlanes[i].w)));

    RenderDatabase db{};
    const auto mesh = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    assert(mesh);
    Instance near{};
    near.worldPosition = tour::math::SplitDoubleFloat(
        eye + glm::dvec3(0.25, 0.0, 0.5));
    near.localSphere = glm::vec4(0,0,0,0.01f);
    near.refs = glm::uvec4(mesh.slot, mesh.generation, 0u, 0u);
    const auto id = db.instances.Insert(near);
    assert(id);
    assert(ReferenceInstanceVisible(db, id, frustum));
    near.worldPosition = tour::math::SplitDoubleFloat(
        eye + glm::dvec3(10.0, 0.0, 0.5));
    assert(db.instances.Update(id, near));
    assert(!ReferenceInstanceVisible(db, id, frustum));

    // The disabled (infinite-far) plane is intentionally valid and can
    // never reject an instance. A zero normal/zero offset is malformed.
    auto disabled = frustum;
    disabled.planes[5] = glm::dvec4(0,0,0,1e30);
    assert(disabled.Valid());
    disabled.planes[0] = glm::dvec4(0);
    assert(!disabled.Valid());
    assert(!PackInstanceCullView(disabled, packed));
}

void CheckLegacyRebasing() {
    smvk::RenderView view{};
    // Absolute plane: world x <= 103, but origin is world x=100.
    view.m_viewPlanes[0] = glm::vec4(-1,0,0,103);
    view.m_viewPlanes[1] = glm::vec4( 1,0,0,-97);
    view.m_viewPlanes[2] = glm::vec4(0, 1,0,4);
    view.m_viewPlanes[3] = glm::vec4(0,-1,0,4);
    view.m_viewPlanes[4] = glm::vec4(0,0,1,4);
    view.m_viewPlanes[5] = glm::vec4(0,0,-1,4);
    const auto frustum = LegacyRelativeFrustum(view, glm::dvec3(100,0,0));
    assert(frustum.Valid());
    assert(std::abs(frustum.planes[0].w - 3.0) < 1e-8);
    assert(std::abs(frustum.planes[1].w - 3.0) < 1e-8);
    InstanceCullGpuView gpu{};
    assert(PackInstanceCullView(frustum, gpu));
    assert(gpu.inwardPlanes[0].w == 3.0f);
    assert(gpu.inwardPlanes[1].w == 3.0f);

    RenderDatabase db{};
    auto m = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    Instance a{};
    a.worldPosition = tour::math::SplitDoubleFloat(glm::dvec3(101,0,0));
    a.localSphere = glm::vec4(0,0,0,0.25f);
    a.refs = glm::uvec4(m.slot,m.generation,0u,0u);
    auto id = db.instances.Insert(a);
    auto member = db.leafMembers.Insert(LeafMember{
        glm::uvec4(id.slot,id.generation,0u,0u)});
    assert(ReferenceInstanceVisible(db,id,frustum));
    auto verdict = ReferenceCandidate(db,member.slot,frustum);
    assert(verdict.value.x==id.slot && verdict.value.z==1 && verdict.value.w==0);
    a.worldPosition = tour::math::SplitDoubleFloat(glm::dvec3(1000,0,0));
    assert(db.instances.Update(id,a));
    verdict = ReferenceCandidate(db,member.slot,frustum);
    assert(verdict.value.z==0 && verdict.value.w==0);

    // Invalid mesh bounds fail open and are marked bypass.
    assert(db.meshes.Update(m, Mesh{}));
    verdict = ReferenceCandidate(db,member.slot,frustum);
    assert(verdict.value.z==1 && verdict.value.w==1);
    assert(db.instances.Erase(id));
    verdict = ReferenceCandidate(db,member.slot,frustum);
    assert(verdict.value.x == InvalidIndex && verdict.value.z==0);
}
}
int main() {
    CheckDoubleView();
    CheckLegacyRebasing();
    std::puts("gpu scene validation views: PASS");
}
