// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Scalar-address mirrors of gpu_scene_task_mesh.hlsli. Keep pointer
// and word order identical; check DXC SPIR-V offsets before enabling.
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tour::gpu_scene {
struct TaskMeshGpuGroup {
    uint32_t workSlot;
    uint32_t firstCluster;
    uint32_t clusterCount;
    uint32_t rasterClass;
};
struct TaskMeshGpuParams {
    uint64_t groups, work, clusters, instances, cullView, rasterView;
    uint64_t geometry, aliases, indices, positions, shading, taskOffsets;
    uint32_t workCount, instanceCount, clusterCount, geometryCount;
    uint32_t aliasCount, indexBase, indexWordCapacity, vertexCapacity;
    uint32_t viewValid, coneEnabled, workCapacity, debugMode; // 0 lit, 1 meshlets, 2 LODs
    uint64_t hiz; // reversed-Z MIN Hi-Z (meshlet occlusion), 0 = disabled
    uint32_t hizWidth, hizHeight, hizMips, hizValid;
    uint64_t meshletVertices;  // indexed-meshlet unique vertex windows (0 = none)
    uint64_t meshletTriangles; // indexed-meshlet local triangle ids (3 u8/uint)
    uint64_t historyPrevious;  // completed previous-frame keyed meshlet survivors
    uint64_t historyCurrent;   // cleared current-frame keyed survivor table
    uint32_t historyCapacity;  // power of two, <= 1048576 slots
    uint32_t historyValid;     // previous history matches scene/geometry revision
    // Motion tail (batched motion mesh shader): the previous camera-relative
    // view + previous instance transforms. 0 = no history (first frame /
    // camera cut); prevSlots bounds the prev read against a shrunken snapshot.
    uint64_t prevView = 0;
    uint64_t prevTransforms = 0;
    uint32_t prevValid = 0;
    uint32_t prevSlots = 0;
};
struct TaskMeshGpuPush {
    uint64_t params;
    uint32_t groupBase;
    uint32_t groupCount;
    uint32_t rasterClass;
    uint32_t taskGridX;
    uint32_t temporalPhase;  // 0 normal, 1 seed, 2 remainder, 3 Main
    uint32_t vertexOptFlags; // bit 0 = rigid-transform normalize elision
    // Skinned tail (mirrors GsTaskMeshPush): the frame's deformed heap
    // (5 words/vertex) + the skinned span's global base. Pushed AT DISPATCH
    // (after the skinning pass — the parity-flipped BDA is only known then),
    // never baked into the params buffer. A skinned instance's mesh shader
    // fetches (vertexId - skinDeformedBase) * 5. skinValid = 1 only when the
    // frame ran the skinning pass.
    uint64_t skinDeformed = 0;
    uint32_t skinDeformedBase = 0;
    uint32_t skinValid = 0;
    // Previous parity half + validity (skeletal motion vectors). skinDeformedPrev
    // is last frame's cur half of the SAME heap; skinPrevValid gates it (1 only
    // while consecutive frames skinned the same heap + span + base — a rebuild
    // or skinning gap leaves 0 and the motion shader falls back to zero mesh
    // motion). The base is SHARED with the current half (parity flip only
    // changes the half offset, never the vertex addressing).
    uint64_t skinDeformedPrev = 0;
    uint32_t skinPrevValid = 0;
};
struct TaskMeshIndirectCommand {
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
static_assert(sizeof(TaskMeshGpuGroup) == 16);
static_assert(sizeof(TaskMeshGpuParams) == 232);
static_assert(offsetof(TaskMeshGpuParams, shading) == 80);
static_assert(offsetof(TaskMeshGpuParams, workCount) == 96);
static_assert(offsetof(TaskMeshGpuParams, hiz) == 144);
static_assert(offsetof(TaskMeshGpuParams, hizValid) == 164);
static_assert(offsetof(TaskMeshGpuParams, meshletVertices) == 168);
static_assert(offsetof(TaskMeshGpuParams, historyPrevious) == 184);
static_assert(offsetof(TaskMeshGpuParams, historyCapacity) == 200);
static_assert(sizeof(TaskMeshGpuPush) == 64);
static_assert(offsetof(TaskMeshGpuPush, skinDeformed) == 32);
static_assert(offsetof(TaskMeshGpuPush, skinDeformedPrev) == 48);
static_assert(offsetof(TaskMeshGpuPush, skinPrevValid) == 56);
static_assert(sizeof(TaskMeshIndirectCommand) == 12);
static_assert(std::is_trivially_copyable_v<TaskMeshGpuParams>);
} // namespace tour::gpu_scene
