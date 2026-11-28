// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Candidate E helper — zeroes taskOffsets[workCapacity], the running append
// counter that the atomic-append pass accumulates and the task_dispatch
// kernel reads as the DrawMeshTasksIndirect count. MUST be separated from the
// append by an explicit compute dependency, and from any prior frame's
// device-read of that word by the render-graph's write->read edge.
#include "gpu_scene_task_mesh.hlsli"
struct Push {
    GPU_PTR_WRITE(GsTaskOffsets) taskOffsets;
    uint workCapacity;
    uint reserved;
};
GPU_PUSH(Push, pc);
[numthreads(1, 1, 1)]
void mainCS(uint3 id : SV_DispatchThreadID) {
    if (id.x == 0u)
        pc.taskOffsets.Get().values[pc.workCapacity] = 0u;
}
