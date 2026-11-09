// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Raster-command build over GPU-scene work expansion. The per-flight work
// buffer contains instance slot/generation + revision-local registered
// geometry ID. The geometry table resolves that ID to real index-buffer
// ranges. ALL commands are written in compute (no transfer seeding).
//
// Work count stays GPU-resident in workOffsets[candidateCount]. Zero commands
// for unused slots make fixed-capacity indirect consumption safe (though a
// production indirect-count backend can later avoid zero-command overhead).
//
// Indirect firstInstance = work slot. The raster VS must fetch work[i],
// then validate the persistent Instance row's slot AND generation.
#include "gpu_platform.hlsli"

#define GS_RASTER_MAX_WORK     1048576u
#define GS_RASTER_MAX_COMMANDS (4u * GS_RASTER_MAX_WORK)
#define GS_RASTER_MAX_GEO      65535u
#define GS_RASTER_MAX_CANDIDATES 1048576u
#define GS_INVALID            0xffffffffu

struct SceneSubmeshWork { uint4 identity; uint4 detail; };
struct RasterIndexedGeometry {
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint flags;
};
struct RasterIndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct WorkBuffer { SceneSubmeshWork rows[GS_RASTER_MAX_WORK]; };
struct OffsetBuffer { uint values[GS_RASTER_MAX_CANDIDATES + 1u]; };
struct GeometryBuffer { RasterIndexedGeometry rows[GS_RASTER_MAX_GEO]; };
struct IndirectBuffer { RasterIndirectCmd rows[GS_RASTER_MAX_COMMANDS]; };
struct AliasBuffer { uint slots[GS_RASTER_MAX_GEO]; };

struct RasterWorkDrawBuildPush {
    GPU_PTR_READ(WorkBuffer) work;
    GPU_PTR_READ(OffsetBuffer) workOffsets;
    GPU_PTR_READ(GeometryBuffer) indexedGeometry;
    GPU_PTR_WRITE(IndirectBuffer) indirect;
    uint candidateCount;
    uint geometryCount;
    uint commandCapacity;
    uint binStride; // number of work slots per raster class
    GPU_PTR_READ(AliasBuffer) aliases;
    uint aliasCount;
    uint padding;
};
GPU_PUSH(RasterWorkDrawBuildPush, pc);

[numthreads(128, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.commandCapacity || i >= GS_RASTER_MAX_COMMANDS)
        return;
    RasterIndirectCmd command;
    command.indexCount = 0;
    command.instanceCount = 0;
    command.firstIndex = 0;
    command.vertexOffset = 0;
    command.firstInstance = 0;

    // This offset is the GPU-computed total work count. Never trust a value
    // outside the reserved command range, including an overflowed prefix.
    const uint stride = pc.binStride;
    if (stride != 0u && stride <= GS_RASTER_MAX_WORK &&
        pc.commandCapacity == 4u * stride &&
        pc.candidateCount <= GS_RASTER_MAX_CANDIDATES) {
        const uint bin = i / stride;
        const uint workIndex = i % stride;
        const uint count = pc.workOffsets.Get().values[pc.candidateCount];
        if (count <= stride && workIndex < count) {
            const SceneSubmeshWork work = pc.work.Get().rows[workIndex];
            const uint transientId = work.identity.z;
            if (work.identity.x != GS_INVALID && work.identity.y != 0u &&
                work.detail.z == bin &&
                transientId < pc.aliasCount && transientId < GS_RASTER_MAX_GEO) {
                const uint geometry = pc.aliases.Get().slots[transientId];
                if (geometry < pc.geometryCount && geometry < GS_RASTER_MAX_GEO) {
                    const RasterIndexedGeometry indexed =
                        pc.indexedGeometry.Get().rows[geometry];
                    if (indexed.indexCount != 0u) {
                    command.indexCount = indexed.indexCount;
                    command.instanceCount = 1u;
                    command.firstIndex = indexed.firstIndex;
                    command.vertexOffset = indexed.vertexOffset;
                    command.firstInstance = workIndex;
                    }
                }
            }
        }
    }
    pc.indirect.Get().rows[i] = command;
}
