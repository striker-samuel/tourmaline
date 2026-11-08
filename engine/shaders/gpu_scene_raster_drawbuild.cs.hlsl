// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Device-side indexed raster command builder. ONE compute writer owns all
// 20 bytes of each command (no mixed transfer/compute writes). Missing geometry
// yields a zero-instance command, never an out-of-bounds memory fetch.
// The caller must validate group and geometry buffer capacities before dispatch,
// and barrier compute storage writes before DrawIndexedIndirect command reads.
#include "gpu_platform.hlsli"

#define GPU_SCENE_RASTER_MAX_GROUPS 65535
struct RasterIndexedGeometry {
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint flags;
};
struct RasterDrawGroup {
    uint geometryIndex;
    uint firstInstance;
    uint instanceCount;
    uint flags;
};
struct RasterIndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct RasterGeometryBuffer { RasterIndexedGeometry items[GPU_SCENE_RASTER_MAX_GROUPS]; };
struct RasterGroupBuffer { RasterDrawGroup items[GPU_SCENE_RASTER_MAX_GROUPS]; };
struct RasterCommandBuffer { RasterIndirectCmd items[GPU_SCENE_RASTER_MAX_GROUPS]; };

struct RasterDrawBuildPush {
    GPU_PTR_READ(RasterGeometryBuffer) indexedGeometry;
    GPU_PTR_READ(RasterGroupBuffer) groups;
    GPU_PTR_WRITE(RasterCommandBuffer) indirect;
    uint groupCount;
    uint geometryCount;
};
GPU_PUSH(RasterDrawBuildPush, pc);

[numthreads(64, 1, 1)]
void mainCS(uint3 threadId : SV_DispatchThreadID) {
    uint i = threadId.x;
    if (i >= pc.groupCount)
        return;
    RasterDrawGroup group = pc.groups.Get().items[i];
    RasterIndirectCmd cmd;
    cmd.indexCount = 0;
    cmd.instanceCount = 0;
    cmd.firstIndex = 0;
    cmd.vertexOffset = 0;
    cmd.firstInstance = 0;
    if (group.geometryIndex < pc.geometryCount) {
        RasterIndexedGeometry geom = pc.indexedGeometry.Get().items[group.geometryIndex];
        cmd.indexCount = geom.indexCount;
        cmd.instanceCount = group.instanceCount;
        cmd.firstIndex = geom.firstIndex;
        cmd.vertexOffset = geom.vertexOffset;
        cmd.firstInstance = group.firstInstance;
    }
    pc.indirect.Get().items[i] = cmd;
}
