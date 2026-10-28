// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_drawbuild.cs.hlsl — PR03 chain 4/4: emit the indirect
// commands. One thread per proto (<= 8): writes the DYNAMIC halves of
// the command (instanceCount = counts[p], firstInstance = offsets[p]).
// The CPU seeds the immutable halves (indexCount/firstIndex/
// vertexOffset) once per prototype table — never per frame.
//
// The VS consumes: visible[SV_InstanceID] -> source slot -> instance.
// SV_InstanceID includes the command's firstInstance under SPIR-V, so
// bin p's draw walks EXACTLY its own visible range
// [offsets[p], offsets[p] + counts[p]).

#include "placement_common.hlsli"
#include "placement_cull.hlsli"

// Must match C++ PlacementDrawBuildPush (32 bytes).
struct PlacementDrawBuildPush
{
    vk::BufferPointer<PlacementBinsBuffer> bins;               // 0
    vk::BufferPointer<PlacementIndirectBuffer> indirect;     // 8
    uint protoCount; // 16
    uint pad0;        // 20
    uint pad1;        // 24
    uint pad2;        // 28
};

[[vk::push_constant]]
ConstantBuffer<PlacementDrawBuildPush> pc;

[numthreads(8, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint proto = groupId.x * 8 + threadId.x;
    if (proto >= pc.protoCount)
        return;
    pc.indirect.Get().c[proto].instanceCount =
        pc.bins.Get().v[PLACEMENT_BINS_COUNT_BASE + proto];
    pc.indirect.Get().c[proto].firstInstance =
        pc.bins.Get().v[PLACEMENT_BINS_OFFSET_BASE + proto];
}
