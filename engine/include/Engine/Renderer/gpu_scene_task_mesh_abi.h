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
};
struct TaskMeshGpuPush {
    uint64_t params;
    uint32_t groupBase;
    uint32_t groupCount;
    uint32_t rasterClass;
    uint32_t taskGridX;
    uint32_t temporalPhase; // 0 normal, 1 seed, 2 remainder, 3 Main
    uint32_t reservedTemporal;
};
struct TaskMeshIndirectCommand {
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
static_assert(sizeof(TaskMeshGpuGroup) == 16);
static_assert(sizeof(TaskMeshGpuParams) == 208);
static_assert(offsetof(TaskMeshGpuParams, shading) == 80);
static_assert(offsetof(TaskMeshGpuParams, workCount) == 96);
static_assert(offsetof(TaskMeshGpuParams, hiz) == 144);
static_assert(offsetof(TaskMeshGpuParams, hizValid) == 164);
static_assert(offsetof(TaskMeshGpuParams, meshletVertices) == 168);
static_assert(offsetof(TaskMeshGpuParams, historyPrevious) == 184);
static_assert(offsetof(TaskMeshGpuParams, historyCapacity) == 200);
static_assert(sizeof(TaskMeshGpuPush) == 32);
static_assert(sizeof(TaskMeshIndirectCommand) == 12);
static_assert(std::is_trivially_copyable_v<TaskMeshGpuParams>);
} // namespace tour::gpu_scene
