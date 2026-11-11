// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Clear the CURRENT acquired flight's meshlet visibility hash BEFORE seed.
// Never clear the previous completed flight. A graphics/compute barrier is
// required after this pass and before task-shader history writes.
#include "gpu_platform.hlsli"
#define GS_TEMPORAL_HASH_LIMIT 1048576u
struct Entry { uint state; uint slot; uint generation; uint clusterId; };
struct History { uint overflow; uint reserved0; uint reserved1; uint reserved2;
                 Entry entries[GS_TEMPORAL_HASH_LIMIT]; };
struct Push { GPU_PTR(History) current; uint capacity; uint pad0; uint pad1; uint pad2; };
GPU_PUSH(Push,pc);
[numthreads(256,1,1)]
void mainCS(uint3 tid:SV_DispatchThreadID) {
    const uint i=tid.x;
    if(pc.capacity==0u || pc.capacity>GS_TEMPORAL_HASH_LIMIT ||
       (pc.capacity&(pc.capacity-1u))!=0u) return;
    if(i==0u) pc.current.Get().overflow=0u;
    if(i<pc.capacity) pc.current.Get().entries[i].state=0u;
}
