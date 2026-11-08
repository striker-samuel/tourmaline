// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPUIR candidate selection: CPU frustum-tests COARSE leaves and exports
// compact ranges. It does NOT touch individual instances on the hot path.
// The next compute stage consumes the member ranges and performs per-instance
// tests. Neither stage replaces GfxDraw/seed/HZB yet.

#include <Engine/Renderer/gpu_scene_leaf.h>
#include <Engine/Memory/arena.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace tour::gpu_scene {

inline constexpr uint32_t MaxPassedLeafRanges = 65536;
inline constexpr uint32_t MaxPassedInstanceCandidates = 1048576;

// GPU binary-searches ranges[].members.z (candidateBase), then resolves
// leaves -> members -> instances using *generational* row references.
// GPU-compatible HLSL mirror: gpu_scene_instance_cull.cs.hlsl.
struct alignas(16) PassedInstanceRange {
    glm::uvec4 members{0u}; // x first LeafMember row, y count, z prefix, w reserved
    glm::uvec4 leaf{0u};    // x leaf slot, y leaf generation, z/w reserved
};
struct alignas(16) InstanceCullVerdict {
    glm::uvec4 value{InvalidIndex, 0u, 0u, 0u};
    // x instance slot, y generation, z visible, w bypass/error flags
};
static_assert(sizeof(PassedInstanceRange) == 32);
static_assert(sizeof(InstanceCullVerdict) == 16);

// Binary CPU/HLSL ABI. View data lives behind a BDA pointer (128 bytes),
// not embedded into push constants. Must match the shader's declared
// GpuSceneCullView and GpuSceneInstanceCullPush exactly.
//
// PACKING RULE (the all-zero-verdict bug): DXC packs a
// [[vk::push_constant]] ConstantBuffer<T> member at the next 8-byte
// boundary after vk::BufferPointer scalars - a following uint4 is NOT
// rounded up to 16. The C++ mirror must therefore pack TIGHT after an odd
// pointer run: 7 pointers end at 56, so counts sits at 56 (never
// alignas(16) here - that inserts 8 padding bytes the shader does not see
// and shifts every count the kernel reads).
struct InstanceCullGpuView {
    math::DoubleFloat3 origin;
    glm::vec4 inwardPlanes[6]{};
};
struct InstanceCullGpuPush {
    uint64_t ranges = 0;
    uint64_t leaves = 0;
    uint64_t members = 0;
    uint64_t instances = 0;
    uint64_t meshes = 0;
    uint64_t view = 0;
    uint64_t verdicts = 0;
    glm::uvec4 counts{0u};
    glm::uvec4 counts2{0u};
    // Zero for validation/pass-all; nonzero BDA of per-instance slot ->
    // admitted generation. Only exact generation matches may rasterize.
    uint64_t ownerGenerations = 0;
};
static_assert(sizeof(InstanceCullGpuView) == 128);
static_assert(offsetof(InstanceCullGpuView, inwardPlanes) == 32);
static_assert(sizeof(InstanceCullGpuPush) == 96);
static_assert(offsetof(InstanceCullGpuPush, counts) == 56);
static_assert(offsetof(InstanceCullGpuPush, counts2) == 72);
static_assert(offsetof(InstanceCullGpuPush, ownerGenerations) == 88);

struct RelativeFrustum {
    // The origin and six inward half-spaces must be in the SAME frame as
    // the shaders' per-view origin. Planes: dot(normal, relative) + w >= 0.
    glm::dvec3 originWorld{0.0};
    std::array<glm::dvec4, 6> planes{};

    [[nodiscard]] bool Valid() const {
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(originWorld[k]))
                return false;
        for (const auto& p : planes) {
            for (int k = 0; k < 4; ++k)
                if (!std::isfinite(p[k]))
                    return false;
            // A disabled infinite-far plane is (0,0,0,+1e30).
            // Zero normal + positive w rejects nothing; zero plane
            // with w<=0 is malformed, not a valid cull frustum.
            if (!(glm::dot(glm::dvec3(p), glm::dvec3(p)) > 1e-20) && !(p.w > 0.0))
                return false;
        }
        return true;
    }
};

enum class CandidateStatus : uint32_t { Ok, InvalidView, InvalidSnapshot, CapacityExceeded };
enum class CandidateMode : uint32_t {
    Frustum,
    PassAll // debug/parity: identical candidates, no coarse rejection
};
struct CandidateSelection {
    CandidateStatus status = CandidateStatus::Ok;
    std::vector<PassedInstanceRange> ranges;
    uint32_t candidateCount = 0;
    uint32_t rejectedLeaves = 0;
    uint32_t unculledLeaves = 0; // incomplete bounds => must pass
    [[nodiscard]] bool Valid() const {
        return status == CandidateStatus::Ok;
    }
};

[[nodiscard]] inline bool PossibleLeafInFrustum(const Leaf& leaf, const RelativeFrustum& view) {
    // Fail open: invalid/incomplete bounds must NEVER drop geometry.
    if ((leaf.range.z & LeafFlagBoundsComplete) == 0)
        return true;
    const glm::dvec3 center = math::JoinDoubleFloat(leaf.worldCenter) - view.originWorld;
    const glm::dvec3 extents(leaf.halfExtents);
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(center[k]) || !std::isfinite(extents[k]) || extents[k] < 0.0)
            return true;
    for (const auto& p : view.planes) {
        const glm::dvec3 n(p);
        const double reach = glm::dot(glm::abs(n), extents);
        const double signedDistance = glm::dot(n, center) + p.w;
        if (!std::isfinite(signedDistance) || !std::isfinite(reach))
            return true;
        // Outward slack for finite float32 leaf bounds/view reconstruction.
        // The actual future render-view guard is an additional dilation.
        const double slack = 0.01 + 2e-6 * (std::abs(signedDistance) + reach + 1.0);
        if (signedDistance + reach < -slack)
            return false;
    }
    return true;
}

// The selector processes one ticket per CPU leaf, not each instance.
// Missing/incomplete bounds bypass group culling, retaining members for
// GPU validation. A bad snapshot or capacity overflow aborts the ENTIRE
// selection: callers must fall back to the legacy draw path.
[[nodiscard]] inline CandidateSelection SelectPassedInstanceRanges(
    const RenderDatabase& db,
    std::span<const LeafTicket> tickets,
    const RelativeFrustum& view,
    CandidateMode mode = CandidateMode::Frustum,
    uint32_t maxRanges = MaxPassedLeafRanges,
    uint32_t maxCandidates = MaxPassedInstanceCandidates) {
    CandidateSelection out{};
    const auto fail = [&](CandidateStatus error) {
        out = {};
        out.status = error;
        return out;
    };
    if (mode == CandidateMode::Frustum && !view.Valid())
        return fail(CandidateStatus::InvalidView);
    maxRanges = std::min(maxRanges, MaxPassedLeafRanges);
    maxCandidates = std::min(maxCandidates, MaxPassedInstanceCandidates);

    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const LeafTicket& ticket : tickets) {
        if (!ticket.leaf || !seen.emplace(ticket.leaf.slot, ticket.leaf.generation).second)
            return fail(CandidateStatus::InvalidSnapshot);
        const Leaf* leaf = db.leaves.Get(ticket.leaf);
        if (!leaf || leaf->range.x != ticket.firstMember || leaf->range.y != ticket.memberCount ||
            ticket.memberCount == 0 ||
            uint64_t(ticket.firstMember) + ticket.memberCount > db.leafMembers.Size())
            return fail(CandidateStatus::InvalidSnapshot);
        const bool safe = (leaf->range.z & LeafFlagBoundsComplete) != 0;
        if (!safe)
            ++out.unculledLeaves;
        if (mode == CandidateMode::Frustum && safe && !PossibleLeafInFrustum(*leaf, view)) {
            ++out.rejectedLeaves;
            continue;
        }
        if (out.ranges.size() >= maxRanges ||
            uint64_t(out.candidateCount) + ticket.memberCount > maxCandidates)
            return fail(CandidateStatus::CapacityExceeded);
        PassedInstanceRange range{};
        range.members = glm::uvec4(ticket.firstMember, ticket.memberCount, out.candidateCount, 0u);
        range.leaf = glm::uvec4(ticket.leaf.slot, ticket.leaf.generation, 0u, 0u);
        out.ranges.push_back(range);
        out.candidateCount += ticket.memberCount;
    }
    return out;
}

// No-heap production candidate selector. All scratch and output ranges
// belong to FrameArena; the caller must consume them before frame reset.
// Duplicate leaf identities and all snapshot errors fail closed at the
// selection level, enabling the renderer's complete legacy fallback.
struct FrameCandidateSelection {
    CandidateStatus status = CandidateStatus::Ok;
    std::span<const PassedInstanceRange> ranges{};
    uint32_t candidateCount = 0;
    uint32_t rejectedLeaves = 0;
    uint32_t unculledLeaves = 0;
    [[nodiscard]] bool Valid() const { return status == CandidateStatus::Ok; }
};

[[nodiscard]] inline FrameCandidateSelection SelectPassedInstanceRangesFrame(
    const RenderDatabase& db,
    std::span<const LeafTicket> tickets,
    const RelativeFrustum& view,
    CandidateMode mode = CandidateMode::Frustum,
    uint32_t maxRanges = MaxPassedLeafRanges,
    uint32_t maxCandidates = MaxPassedInstanceCandidates) {
    FrameCandidateSelection out{};
    const auto fail = [&](CandidateStatus error) {
        FrameCandidateSelection failed{};
        failed.status = error;
        return failed;
    };
    if (mode == CandidateMode::Frustum && !view.Valid())
        return fail(CandidateStatus::InvalidView);
    maxRanges = std::min(maxRanges, MaxPassedLeafRanges);
    maxCandidates = std::min(maxCandidates, MaxPassedInstanceCandidates);

    // Validate unique leaf identities using FrameArena sorting instead of
    // std::set nodes. The output retains original spatial ticket ordering.
    if (!tickets.empty()) {
        auto* keys = static_cast<uint64_t*>(
            smvk::FrameArena().Allocate(tickets.size() * sizeof(uint64_t), 16));
        for (size_t i = 0; i < tickets.size(); ++i) {
            const auto id = tickets[i].leaf;
            if (!id)
                return fail(CandidateStatus::InvalidSnapshot);
            keys[i] = (uint64_t(id.slot) << 32) | id.generation;
        }
        if (tickets.size() > 1)
            std::sort(keys, keys + tickets.size());
        for (size_t i = 1; i < tickets.size(); ++i)
            if (keys[i] == keys[i - 1])
                return fail(CandidateStatus::InvalidSnapshot);
    }

    const size_t capacity = std::min<size_t>(tickets.size(), maxRanges);
    auto* ranges = capacity
        ? static_cast<PassedInstanceRange*>(smvk::FrameArena().Allocate(
              capacity * sizeof(PassedInstanceRange), alignof(PassedInstanceRange)))
        : nullptr;
    size_t used = 0;
    for (const LeafTicket& ticket : tickets) {
        const Leaf* leaf = db.leaves.Get(ticket.leaf);
        if (!leaf || leaf->range.x != ticket.firstMember ||
            leaf->range.y != ticket.memberCount || ticket.memberCount == 0 ||
            uint64_t(ticket.firstMember) + ticket.memberCount > db.leafMembers.Size())
            return fail(CandidateStatus::InvalidSnapshot);
        const bool safe = (leaf->range.z & LeafFlagBoundsComplete) != 0;
        if (!safe)
            ++out.unculledLeaves;
        if (mode == CandidateMode::Frustum && safe && !PossibleLeafInFrustum(*leaf, view)) {
            ++out.rejectedLeaves;
            continue;
        }
        if (used >= capacity ||
            uint64_t(out.candidateCount) + ticket.memberCount > maxCandidates)
            return fail(CandidateStatus::CapacityExceeded);
        PassedInstanceRange range{};
        range.members = glm::uvec4(ticket.firstMember, ticket.memberCount,
                                    out.candidateCount, 0u);
        range.leaf = glm::uvec4(ticket.leaf.slot, ticket.leaf.generation, 0u, 0u);
        std::construct_at(ranges + used++, range);
        out.candidateCount += ticket.memberCount;
    }
    out.ranges = std::span<const PassedInstanceRange>(ranges, used);
    return out;
}

// CPU-only per-instance reference oracle for validation of the GPU kernel.
// It inspects a single candidate, not a hot-path prerequisite of leaf cull.
// Invalid generational handles are rejected; incomplete geometry bounds
// conservatively pass (the later renderer must resolve residency/LOD).
[[nodiscard]] inline bool ReferenceInstanceVisible(const RenderDatabase& db,
                                                   Handle<Instance> id,
                                                   const RelativeFrustum& view) {
    const Instance* inst = db.instances.Get(id);
    if (!inst)
        return false;
    if (!view.Valid())
        return true;
    if (inst->refs.x >= db.meshes.Rows().size())
        return true; // unknown geometry cannot justify occlusion
    const auto& mesh = db.meshes.Rows()[inst->refs.x];
    if (!mesh.identity.y || mesh.identity.x != inst->refs.y ||
        (mesh.value.flags & MeshFlagCompleteBounds) == 0)
        return true;

    const glm::dvec3 translation = math::JoinDoubleFloat(inst->worldPosition) - view.originWorld;
    const glm::dvec3 bx(inst->basisX), by(inst->basisY), bz(inst->basisZ);
    const glm::dvec3 local(inst->localSphere);
    const glm::dvec3 center = translation + bx * local.x + by * local.y + bz * local.z;
    const double radius = double(inst->localSphere.w) *
                          std::sqrt(glm::dot(bx, bx) + glm::dot(by, by) + glm::dot(bz, bz));
    if (!std::isfinite(radius) || radius < 0.0)
        return true;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(center[k]))
            return true;

    for (const auto& p : view.planes) {
        const double signedDistance = glm::dot(glm::dvec3(p), center) + p.w;
        if (!std::isfinite(signedDistance))
            return true;
        const double slack = 0.02 + 2e-6 * (std::abs(signedDistance) + radius + 1.0);
        if (signedDistance < -(radius + slack))
            return false;
    }
    return true;
}

} // namespace tour::gpu_scene
