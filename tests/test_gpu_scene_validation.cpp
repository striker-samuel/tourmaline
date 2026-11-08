// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless GPU Scene validation: view-space parity and struct ABI.
#include <Engine/Renderer/gpu_scene_validation.h>

#include <cmath>
#include <cstdio>
#include <limits>
// House test convention (AGENTS.md): the CHECK macro ALWAYS evaluates its
// condition, in every build flavor. This file originally used CHECK(),
// whose NDEBUG no-op makes Release runs vacuous - and any side effect
// inside an assert (see test_gpu_scene_candidates.cpp:
// assert(leaves.Flush(db))) was silently skipped, crashing the run.
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)
namespace {
int g_checks = 0;
int g_failures = 0;
} // namespace

using namespace tour::gpu_scene;

namespace {
void CheckDoubleView() {
    const glm::dvec3 eye(1.0e12, -1.0e12, 0.0);
    // This double world->clip puts the eye at clip origin, including
    // sub-metre offsets that cannot survive a float32 world VP.
    glm::dmat4 vp(1.0);
    vp[3] = glm::dvec4(-eye, 1.0);
    const RelativeFrustum frustum = DoubleRelativeFrustum(vp, eye);
    CHECK(frustum.Valid());
    InstanceCullGpuView packed{};
    CHECK(PackInstanceCullView(frustum, packed));
    CHECK(sizeof(packed) == 128);
    const glm::dvec3 rejoined = tour::math::JoinDoubleFloat(packed.origin);
    CHECK(glm::length(rejoined - eye) < 0.01);
    for (int i = 0; i < 6; ++i)
        CHECK(std::isfinite(double(packed.inwardPlanes[i].w)));

    RenderDatabase db{};
    const auto mesh = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    CHECK(mesh);
    Instance near{};
    near.worldPosition = tour::math::SplitDoubleFloat(eye + glm::dvec3(0.25, 0.0, 0.5));
    near.localSphere = glm::vec4(0, 0, 0, 0.01f);
    near.refs = glm::uvec4(mesh.slot, mesh.generation, 0u, 0u);
    const auto id = db.instances.Insert(near);
    CHECK(id);
    CHECK(ReferenceInstanceVisible(db, id, frustum));
    near.worldPosition = tour::math::SplitDoubleFloat(eye + glm::dvec3(10.0, 0.0, 0.5));
    CHECK(db.instances.Update(id, near));
    CHECK(!ReferenceInstanceVisible(db, id, frustum));

    // The disabled (infinite-far) plane is intentionally valid and can
    // never reject an instance. A zero normal/zero offset is malformed.
    auto disabled = frustum;
    disabled.planes[5] = glm::dvec4(0, 0, 0, 1e30);
    CHECK(disabled.Valid());
    disabled.planes[0] = glm::dvec4(0);
    CHECK(!disabled.Valid());
    CHECK(!PackInstanceCullView(disabled, packed));
}

void CheckMalformedDoubleFrusta() {
    const glm::dvec3 origin(1.0e12, -1.0e12, 0.0);

    // A zero/degenerate VP must never become six disabled planes.
    // Before this regression fix it produced an apparently valid
    // all-pass frustum and allowed unsafe GPU-cull dispatch.
    const auto zero = DoubleRelativeFrustum(glm::dmat4(0.0), origin);
    CHECK(!zero.Valid());
    InstanceCullGpuView packed{};
    CHECK(!PackInstanceCullView(zero, packed));

    // Side-plane degeneracy cannot represent an infinite far plane.
    glm::dmat4 noHorizontal(1.0);
    noHorizontal[0] = glm::dvec4(0.0);
    CHECK(!DoubleRelativeFrustum(noHorizontal, origin).Valid());

    // Non-finite VP components are invalid even in depth planes.
    glm::dmat4 nanVP(1.0);
    nanVP[0][0] = std::numeric_limits<double>::quiet_NaN();
    CHECK(!DoubleRelativeFrustum(nanVP, origin).Valid());

    // A standard infinite-far reversed-Z projection has a disabled
    // positive depth half-space. Retain that legitimate case.
    glm::dmat4 infiniteReverseZ(1.0);
    infiniteReverseZ[2][2] = 0.0;
    infiniteReverseZ[3][2] = 1.0;
    infiniteReverseZ[2][3] = -1.0;
    infiniteReverseZ[3][3] = 0.0;
    const auto infinite = DoubleRelativeFrustum(infiniteReverseZ, glm::dvec3(0.0));
    CHECK(infinite.Valid());
    CHECK(PackInstanceCullView(infinite, packed));
}

void CheckIndirectAndAuthoritativeView() {
    CHECK(sizeof(SceneIndirectDispatch) == 12);
    CHECK(sizeof(SceneIndirectArgsPush) == 24);
    CHECK(sizeof(SceneIndirectVerifyPush) == 32);

    const auto zero = ReferenceIndirectDispatch(0u, 524288u);
    CHECK(zero.x == 0u && zero.y == 1u && zero.z == 1u);
    CHECK(ReferenceIndirectDispatch(1u, 524288u).x == 1u);
    CHECK(ReferenceIndirectDispatch(64u, 524288u).x == 1u);
    CHECK(ReferenceIndirectDispatch(65u, 524288u).x == 2u);
    CHECK(ReferenceIndirectDispatch(524288u, 524288u).x == 8192u);
    CHECK(ReferenceIndirectDispatch(524289u, 524288u).x == 0u);

    // Projection and rotation are relative, not a huge absolute-world VP.
    // The origin remains full double throughout plane construction.
    const glm::dvec3 origin(1.0e12, -1.0e12, 1.0e12);
    glm::dmat4 projection(1.0);
    const auto frustum = AuthoritativeRelativeFrustum(projection, glm::dmat4(1.0), origin);
    CHECK(frustum.Valid());
    InstanceCullGpuView packed{};
    CHECK(PackInstanceCullView(frustum, packed));
    CHECK(glm::length(tour::math::JoinDoubleFloat(packed.origin) - origin) < 0.01);
    CHECK(std::abs(frustum.planes[0].w - 1.0) < 1e-12);

    RenderDatabase db{};
    const auto mesh = db.meshes.Insert(Mesh{0u, 1u, 1u, MeshFlagCompleteBounds});
    CHECK(mesh);
    Instance instance{};
    instance.refs = glm::uvec4(mesh.slot, mesh.generation, 0u, 0u);
    instance.worldPosition = tour::math::SplitDoubleFloat(origin + glm::dvec3(0.25, 0.0, 0.5));
    instance.localSphere = glm::vec4(0, 0, 0, 0.01f);
    auto handle = db.instances.Insert(instance);
    CHECK(ReferenceInstanceVisible(db, handle, frustum));
    instance.worldPosition = tour::math::SplitDoubleFloat(origin + glm::dvec3(10.0, 0.0, 0.5));
    CHECK(db.instances.Update(handle, instance));
    CHECK(!ReferenceInstanceVisible(db, handle, frustum));
}

void CheckStableCompaction() {
    CHECK(sizeof(CompactCountPush) == 24);
    CHECK(sizeof(CompactPrefixPush) == 24);
    CHECK(sizeof(CompactScatterPush) == 32);

    // The same predicate is used in the count and scatter shaders.
    // Invalid tombstones and invisible rows must not reach draw expansion.
    std::vector<InstanceCullVerdict> input(130);
    for (uint32_t i = 0; i < input.size(); ++i) {
        input[i].value = glm::uvec4(i, 1u, i % 3u == 0u ? 1u : 0u, 0u);
    }
    input[0].value.x = InvalidIndex;
    input[3].value.y = 0u;
    input[64].value.z = 1u;  // first index of the next workgroup
    input[129].value.z = 1u; // partial last workgroup
    const auto output = CompactVisibleVerdicts(input);
    CHECK(output.size() == 43u);
    CHECK(output.front().value.x == 6u);
    CHECK(output.back().value.x == 129u);
    for (size_t i = 1; i < output.size(); ++i)
        CHECK(output[i - 1].value.x < output[i].value.x);

    std::vector<InstanceCullVerdict> empty(64);
    CHECK(CompactVisibleVerdicts(empty).empty());
    empty[17].value = glm::uvec4(10u, 7u, 1u, 1u);
    auto single = CompactVisibleVerdicts(empty);
    CHECK(single.size() == 1u);
    CHECK(glm::all(glm::equal(single[0].value, empty[17].value)));

    // Counts add up across whole and partial 64-wide workgroups.
    size_t totals = 0;
    for (size_t first = 0; first < input.size(); first += 64u) {
        const size_t last = std::min(first + 64u, input.size());
        totals += CompactVisibleVerdicts(
                      std::span<const InstanceCullVerdict>(input.data() + first, last - first))
                      .size();
    }
    CHECK(totals == output.size());
}

void CheckSubmeshWorkExpansion() {
    CHECK(sizeof(SceneSubmeshWork) == 32);
    CHECK(sizeof(WorkSelectPush) == 128);
    CHECK(offsetof(WorkSelectPush, lodEyeBudget) == 96);
    CHECK(sizeof(WorkPrefixPush) == 24);
    CHECK(sizeof(WorkScatterPush) == 48);

    RenderDatabase db{};
    // LOD0 has two registered submeshes; LOD1 is a fallback.
    std::array<Submesh, 3> submeshes{Submesh{11u, 101u, 201u, 301u},
                                     Submesh{12u, 102u, 202u, 302u},
                                     Submesh{21u, 121u, 221u, 321u}};
    const auto subs = db.submeshes.InsertContiguous(submeshes);
    CHECK(subs.size() == 3);
    std::array<Lod, 2> levels{Lod{subs[0].slot, 2u, 0.0f, LodFlagResident | LodFlagFilterable},
                              Lod{subs[2].slot, 1u, 1.0f, LodFlagResident | LodFlagFilterable}};
    const auto lods = db.lods.InsertContiguous(levels);
    CHECK(lods.size() == 2);
    const auto mesh = db.meshes.Insert(Mesh{lods[0].slot, 2u, 3u, MeshFlagCompleteBounds});
    CHECK(mesh);
    Instance instance{};
    instance.refs = glm::uvec4(mesh.slot, mesh.generation, 0u, 0u);
    const auto id = db.instances.Insert(instance);
    CHECK(id);
    const auto v = [&](uint32_t visible) {
        InstanceCullVerdict verdict{};
        verdict.value = glm::uvec4(id.slot, id.generation, visible, 0u);
        return verdict;
    };
    std::vector<InstanceCullVerdict> visible{v(1u)};
    auto work = ReferenceExpandWork(db, visible);
    CHECK(work.size() == 2u);
    CHECK(work[0].identity.x == id.slot && work[0].identity.z == 11u);
    CHECK(work[1].identity.z == 12u);
    CHECK(work[0].detail.x == 0u);
    CHECK(work[1].detail.y == subs[1].slot);
    CHECK(work[1].detail.w == 302u);

    // Unregistered submeshes are excluded without a hole in the output.
    CHECK(db.submeshes.Update(subs[0], Submesh{}));
    work = ReferenceExpandWork(db, visible);
    CHECK(work.size() == 1u);
    CHECK(work[0].identity.z == 12u);

    // When LOD0 is evicted, LOD1 is the first eligible resident LOD.
    CHECK(db.meshes.Update(mesh, Mesh{lods[0].slot, 2u, 2u, MeshFlagCompleteBounds}));
    work = ReferenceExpandWork(db, visible);
    CHECK(work.size() == 1u);
    CHECK(work[0].identity.z == 21u);
    CHECK(work[0].detail.x == 1u);

    // A stale generation and a visibility rejection produce no work.
    auto stale = visible;
    stale[0].value.y = id.generation + 1u;
    CHECK(ReferenceExpandWork(db, stale).empty());
    visible[0].value.z = 0u;
    CHECK(ReferenceExpandWork(db, visible).empty());

    visible[0].value.z = 1u;
    CHECK(db.meshes.Erase(mesh));
    CHECK(ReferenceExpandWork(db, visible).empty());
}

void CheckLegacyRebasing() {
    smvk::RenderView view{};
    // Absolute plane: world x <= 103, but origin is world x=100.
    view.m_viewPlanes[0] = glm::vec4(-1, 0, 0, 103);
    view.m_viewPlanes[1] = glm::vec4(1, 0, 0, -97);
    view.m_viewPlanes[2] = glm::vec4(0, 1, 0, 4);
    view.m_viewPlanes[3] = glm::vec4(0, -1, 0, 4);
    view.m_viewPlanes[4] = glm::vec4(0, 0, 1, 4);
    view.m_viewPlanes[5] = glm::vec4(0, 0, -1, 4);
    const auto frustum = LegacyRelativeFrustum(view, glm::dvec3(100, 0, 0));
    CHECK(frustum.Valid());
    CHECK(std::abs(frustum.planes[0].w - 3.0) < 1e-8);
    CHECK(std::abs(frustum.planes[1].w - 3.0) < 1e-8);
    InstanceCullGpuView gpu{};
    CHECK(PackInstanceCullView(frustum, gpu));
    CHECK(gpu.inwardPlanes[0].w == 3.0f);
    CHECK(gpu.inwardPlanes[1].w == 3.0f);

    RenderDatabase db{};
    auto m = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    Instance a{};
    a.worldPosition = tour::math::SplitDoubleFloat(glm::dvec3(101, 0, 0));
    a.localSphere = glm::vec4(0, 0, 0, 0.25f);
    a.refs = glm::uvec4(m.slot, m.generation, 0u, 0u);
    auto id = db.instances.Insert(a);
    auto member = db.leafMembers.Insert(LeafMember{glm::uvec4(id.slot, id.generation, 0u, 0u)});
    CHECK(ReferenceInstanceVisible(db, id, frustum));
    auto verdict = ReferenceCandidate(db, member.slot, frustum);
    CHECK(verdict.value.x == id.slot && verdict.value.z == 1 && verdict.value.w == 0);
    a.worldPosition = tour::math::SplitDoubleFloat(glm::dvec3(1000, 0, 0));
    CHECK(db.instances.Update(id, a));
    verdict = ReferenceCandidate(db, member.slot, frustum);
    CHECK(verdict.value.z == 0 && verdict.value.w == 0);

    // Invalid mesh bounds fail open and are marked bypass.
    CHECK(db.meshes.Update(m, Mesh{}));
    verdict = ReferenceCandidate(db, member.slot, frustum);
    CHECK(verdict.value.z == 1 && verdict.value.w == 1);
    CHECK(db.instances.Erase(id));
    verdict = ReferenceCandidate(db, member.slot, frustum);
    CHECK(verdict.value.x == InvalidIndex && verdict.value.z == 0);
}
} // namespace
int main() {
    CheckDoubleView();
    CheckMalformedDoubleFrusta();
    CheckStableCompaction();
    CheckIndirectAndAuthoritativeView();
    CheckSubmeshWorkExpansion();
    CheckLegacyRebasing();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
