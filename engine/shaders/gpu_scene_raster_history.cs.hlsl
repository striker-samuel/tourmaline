// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Per-flight visibility snapshot; GPU-writes generations of surviving work.
// Distinct Clear/Commit dispatches with an explicit shader-memory barrier.
#include "gpu_platform.hlsli"
#define GS_MAX_WORK 1048576u
#define GS_MAX_INSTANCES 4194304u
struct RasterIndirectCmd { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
struct RasterCmds { RasterIndirectCmd rows[GS_MAX_WORK]; };
struct WorkRow { uint4 identity; uint4 detail; };
struct WorkRows { WorkRow rows[GS_MAX_WORK]; };
struct History { uint generations[GS_MAX_INSTANCES]; };
struct Push {
    GPU_PTR_READ(RasterCmds) commands;
    GPU_PTR_READ(WorkRows) work;
    GPU_PTR_WRITE(History) history;
    uint commandCapacity;
    uint instanceSlots;
    uint historySlots;
    uint reserved;
};
GPU_PUSH(Push, pc);
[numthreads(64, 1, 1)]
void mainClear(uint3 tid : SV_DispatchThreadID) {
    if (tid.x < pc.historySlots && tid.x < GS_MAX_INSTANCES)
        pc.history.Get().generations[tid.x] = 0u;
}
[numthreads(64, 1, 1)]
void mainCommit(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.commandCapacity || i >= GS_MAX_WORK) return;
    const RasterIndirectCmd cmd = pc.commands.Get().rows[i];
    if (cmd.instanceCount == 0u || cmd.indexCount == 0u ||
        cmd.firstInstance >= GS_MAX_WORK) return;
    const WorkRow w = pc.work.Get().rows[cmd.firstInstance];
    if (w.identity.y == 0u || w.identity.x >= pc.instanceSlots ||
        w.identity.x >= pc.historySlots || w.identity.x >= GS_MAX_INSTANCES) return;
    uint ignored;
    InterlockedExchange(pc.history.Get().generations[w.identity.x],
                        w.identity.y, ignored);
}
