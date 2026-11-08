// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Temporal GPU-scene seed: classify CURRENT frustum-visible work against
// LAST frame's surviving persistent (slot,generation) identities.
// Work-slot indices / draw order must never be used as history keys.
#include "gpu_platform.hlsli"

#define GS_MAX_WORK 1048576u
#define GS_MAX_INSTANCES 4194304u
struct RasterIndirectCmd { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
struct RasterCmds { RasterIndirectCmd rows[GS_MAX_WORK]; };
struct WorkRow { uint4 identity; uint4 detail; };
struct WorkRows { WorkRow rows[GS_MAX_WORK]; };
struct History { uint generations[GS_MAX_INSTANCES]; };

struct RasterSeedPush {
    GPU_PTR_READ(RasterCmds) commands;
    GPU_PTR_READ(WorkRows) work;
    GPU_PTR_READ(History) previous;
    GPU_PTR_WRITE(RasterCmds) seed;
    GPU_PTR_WRITE(RasterCmds) remainder;
    uint commandCapacity;
    uint instanceSlots;
    uint historySlots;
    uint historyValid;
};
GPU_PUSH(RasterSeedPush, pc);
[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.commandCapacity || i >= GS_MAX_WORK) return;
    const RasterIndirectCmd cmd = pc.commands.Get().rows[i];
    RasterIndirectCmd s = cmd;
    RasterIndirectCmd r = cmd;
    bool wasVisible = false;
    if (pc.historyValid != 0u && cmd.instanceCount != 0u &&
        cmd.firstInstance < GS_MAX_WORK) {
        const WorkRow w = pc.work.Get().rows[cmd.firstInstance];
        const uint slot = w.identity.x;
        const uint gen = w.identity.y;
        if (gen != 0u && slot < pc.instanceSlots &&
            slot < min(pc.historySlots, GS_MAX_INSTANCES)) {
            wasVisible = pc.previous.Get().generations[slot] == gen;
        }
    }
    if (!wasVisible) s.instanceCount = 0u;
    if (wasVisible) r.instanceCount = 0u;
    pc.seed.Get().rows[i] = s;
    pc.remainder.Get().rows[i] = r;
}
