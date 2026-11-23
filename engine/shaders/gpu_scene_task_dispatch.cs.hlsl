// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU-generated VkDrawMeshTasksIndirectCommandEXT arrays (12 bytes/command).
// First-stage opt-in implementation uses the same task-group list in all
// four PSO dispatches; task shader gates by class. Later per-class GPU
// compaction can eliminate the fourfold task invocation overhead.
// maxX/maxY must be queried from VkPhysicalDeviceMeshShaderPropertiesEXT.
// Host must prove taskGroupCapacity >= complete count and enforce
// no-partial-render ownership before dispatching.
#include "gpu_platform.hlsli"
#define GS_MAX_WORK 1048576u
#define GS_MAX_TASK_GROUPS 16777216u
struct Offsets { uint values[GS_MAX_WORK+1u]; };
struct Args { uint words[12]; }; // 4 * (x,y,z) tightly packed
struct Push {
    GPU_PTR_READ(Offsets) taskOffsets;
    GPU_PTR_WRITE(Args) arguments;
    uint workCapacity;
    uint taskGroupCapacity;
    uint maxX;
    uint maxY;
};
GPU_PUSH(Push,pc);
[numthreads(1,1,1)]
void mainCS(uint3 id:SV_DispatchThreadID) {
    if (id.x!=0u) return;
    uint x=0u,y=0u;
    if (pc.workCapacity<=GS_MAX_WORK &&
        pc.maxX!=0u && pc.maxY!=0u) {
        const uint total=pc.taskOffsets.Get().values[pc.workCapacity];
        if (total<=pc.taskGroupCapacity && total<=GS_MAX_TASK_GROUPS) {
            // Dispatch only the groups that carry work: x = min(total, maxX),
            // not the full device-limit width. Launching pc.maxX groups (the
            // pre-2d9744bb "fixed x" design) made every small scene pay a
            // 4194304-group dispatch for ~184 real groups - a ~15 ms fixed
            // task/mesh cost on single Sponza. For total <= maxX this is a
            // single row (y=1, groupId.y==0) so the task shader's
            // taskGridX flatten is unused; for total > maxX x stays maxX and
            // the flatten matches. Padded workgroups are still rejected in
            // mainAS for the multi-row case.
            x=total==0u?0u:(total<pc.maxX?total:pc.maxX);
            y=total==0u?0u:(total/x+(total%x!=0u?1u:0u));
            if (y>pc.maxY) { x=0u;y=0u; }
        }
    }
    [unroll] for(uint cls=0u;cls<4u;++cls) {
        pc.arguments.Get().words[cls*3u+0u]=x;
        pc.arguments.Get().words[cls*3u+1u]=y;
        pc.arguments.Get().words[cls*3u+2u]=1u;
    }
}
