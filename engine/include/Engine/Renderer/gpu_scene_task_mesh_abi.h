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
    uint32_t viewValid, coneEnabled, workCapacity, reserved1;
    uint64_t hiz; // reversed-Z MIN Hi-Z (meshlet occlusion), 0 = disabled
    uint32_t hizWidth, hizHeight, hizMips, hizValid;
};
struct TaskMeshGpuPush {
    uint64_t params;
    uint32_t groupBase;
    uint32_t groupCount;
    uint32_t rasterClass;
    uint32_t taskGridX;
};
struct TaskMeshIndirectCommand {
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
static_assert(sizeof(TaskMeshGpuGroup) == 16);
static_assert(sizeof(TaskMeshGpuParams) == 168);
static_assert(offsetof(TaskMeshGpuParams, shading) == 80);
static_assert(offsetof(TaskMeshGpuParams, workCount) == 96);
static_assert(offsetof(TaskMeshGpuParams, hiz) == 144);
static_assert(offsetof(TaskMeshGpuParams, hizValid) == 164);
static_assert(sizeof(TaskMeshGpuPush) == 24);
static_assert(sizeof(TaskMeshIndirectCommand) == 12);
static_assert(std::is_trivially_copyable_v<TaskMeshGpuParams>);
} // namespace tour::gpu_scene
