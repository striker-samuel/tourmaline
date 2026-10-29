// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_drawbuild.cs.hlsl — PR03 chain 4/4: emit the indirect
// commands. One thread per proto (<= 8): writes the COMPLETE command.
// The immutable halves (indexCount/firstIndex/vertexOffset) ride the
// PUSH as the prototype table (registered via the runtime's
// SeedIndirect metadata cache, captured by-value into the recorded
// pass); the dynamic halves (instanceCount/firstInstance) come from
// the bins. The kernel therefore SINGLE-SCOPE-writes the buffer
// (COMPUTE) every frame — the old split ownership (a TRANSFER-scoped
// upload seeding the immutable halves + this kernel rewriting the
// dynamic ones) was the stranded-upload-write class: the draw's
// barrier src carried only the last writer's scope, so the upload's
// bytes were never delivered to the DRAW_INDIRECT read (memory
// dependencies are not transitive; see todo.txt).
//
// The VS consumes: visible[SV_InstanceID] -> source slot -> instance.
// SV_InstanceID includes the command's firstInstance under SPIR-V, so
// bin p's draw walks EXACTLY its own visible range
// [offsets[p], offsets[p] + counts[p]).

#include "placement_common.hlsli"
#include "placement_cull.hlsli"

// Must match C++ PlacementDrawBuildPush (128 bytes): the 32-byte header
// (BDAs + protoCount + pads) + the prototype table's three fields, each
// packed as two uint4 lanes over protos 0..7.
struct PlacementDrawBuildPush
{
    vk::BufferPointer<PlacementBinsBuffer> bins;               // 0
    vk::BufferPointer<PlacementIndirectBuffer> indirect;     // 8
    uint protoCount; // 16
    uint pad0;        // 20
    uint pad1;        // 24
    uint pad2;        // 28
    uint4 indexCount0123;   // 32  protos 0..3
    uint4 indexCount4567;  // 48  protos 4..7
    uint4 firstIndex0123;   // 64
    uint4 firstIndex4567;   // 80
    int4 vertexOffset0123;  // 96
    int4 vertexOffset4567;  // 112
};

[[vk::push_constant]]
ConstantBuffer<PlacementDrawBuildPush> pc;

[numthreads(8, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint proto = groupId.x * 8 + threadId.x;
    if (proto >= pc.protoCount)
        return;
    // The prototype table: two uint4 lanes per field, selected by the
    // proto's half then indexed within the lane.
    const uint4 idxC = (proto < 4u) ? pc.indexCount0123 : pc.indexCount4567;
    const uint4 fstI = (proto < 4u) ? pc.firstIndex0123 : pc.firstIndex4567;
    const int4 vtxO = (proto < 4u) ? pc.vertexOffset0123 : pc.vertexOffset4567;
    const uint lane = proto & 3u;
    // The COMPLETE command: immutable halves from the push-carried
    // table, dynamic halves from the bins (the cull's counts + the
    // prefix's exclusive-sum offsets).
    PlacementIndirectCmd cmd;
    cmd.indexCount = idxC[lane];
    cmd.firstIndex = fstI[lane];
    cmd.vertexOffset = vtxO[lane];
    cmd.instanceCount =
        pc.bins.Get().v[PLACEMENT_BINS_COUNT_BASE + proto];
    cmd.firstInstance =
        pc.bins.Get().v[PLACEMENT_BINS_OFFSET_BASE + proto];
    pc.indirect.Get().c[proto] = cmd;
}
