// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// C++ mirrors of the staged dense meshlet candidate HLSL kernels.
// Push layouts are scalar 64-bit BDA pointers followed by uint32 words;
// all are <= Vulkan's guaranteed 128-byte minimum push-constant range.
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <Engine/Core/double_float.h>
#include <glm/glm.hpp>

namespace tour::gpu_scene {
struct alignas(16) CompactMeshletCandidate {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t vertexOffsetBits = 0;
    uint32_t originalWorkSlot = 0;
    uint32_t rasterClass = 0xffffffffu;
    uint32_t clusterIndex = 0xffffffffu;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};
struct MeshletCandidateCountPush {
    uint64_t work = 0, workOffsets = 0, meshes = 0;
    uint64_t geometry = 0, aliases = 0, counts = 0;
    uint32_t selectedCount = 0, workCapacity = 0, meshCount = 0;
    uint32_t geometryCount = 0, aliasCount = 0, unused = 0;
};
struct MeshletCandidateScatterPush {
    uint64_t work = 0, sceneWorkOffsets = 0, meshletOffsets = 0;
    uint64_t meshes = 0, geometry = 0, aliases = 0, clusters = 0;
    uint64_t instances = 0, view = 0, candidates = 0;
    uint32_t sceneCandidateCount = 0, workCapacity = 0;
    uint32_t meshCount = 0, geometryCount = 0, aliasCount = 0;
    uint32_t clusterCount = 0, instanceCount = 0, candidateCapacity = 0;
    uint32_t indexBase = 0, viewValid = 0, coneEnabled = 0, reserved = 0;
};
struct MeshletPsoCountPush {
    uint64_t candidates = 0, meshletOffsets = 0, blockCounts = 0;
    uint32_t workCapacity = 0, candidateCapacity = 0;
    uint32_t blockCount = 0, reserved = 0;
};
struct MeshletPsoScatterPush {
    uint64_t candidates = 0, meshletOffsets = 0, blockBases = 0;
    uint64_t classCounts = 0, commands = 0;
    uint32_t workCapacity = 0, candidateCapacity = 0;
    uint32_t blockCount = 0, commandCapacity = 0;
    uint32_t pad0 = 0, pad1 = 0;
    alignas(16) uint32_t binOffsets[4]{};
    alignas(16) uint32_t binCapacities[4]{};
};
static_assert(sizeof(CompactMeshletCandidate) == 32);
static_assert(offsetof(CompactMeshletCandidate, originalWorkSlot) == 12);
static_assert(offsetof(CompactMeshletCandidate, rasterClass) == 16);
static_assert(std::is_trivially_copyable_v<CompactMeshletCandidate>);
static_assert(sizeof(MeshletCandidateCountPush) == 72);
static_assert(sizeof(MeshletCandidateScatterPush) == 128);
static_assert(sizeof(MeshletPsoCountPush) == 40);
static_assert(sizeof(MeshletPsoScatterPush) == 96);
static_assert(offsetof(MeshletPsoScatterPush, binOffsets) == 64);
static_assert(offsetof(MeshletPsoScatterPush, binCapacities) == 80);
// C++ mirror of the candidate scatter's View buffer input (the HLSL
// DoubleFloat3 origin + six origin-relative frustum planes, 128 bytes).
// The planes come from LegacyRelativeFrustum: w already folds the origin,
// so distances evaluate against DoubleFloatRelative positions directly.
struct alignas(16) MeshletCullViewData {
    tour::math::DoubleFloat3 origin{};
    glm::vec4 planes[6]{};
};
static_assert(sizeof(MeshletCullViewData) == 128);
} // namespace tour::gpu_scene
