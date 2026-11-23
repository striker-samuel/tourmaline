// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_drawbuild.cs.hlsl — PR03 chain 4/4: emit the indirect
// commands. One thread per proto (up to kPlacementMaxProtos): writes the
// COMPLETE command. The immutable halves (indexCount/firstIndex/
// vertexOffset) ride the proto table (a read-only BDA buffer registered
// via the runtime's SeedIndirect and carried in the push as its address);
// the dynamic halves (instanceCount/firstInstance) come from the bins.
// The kernel therefore SINGLE-SCOPE-writes the buffer (COMPUTE) every
// frame — the old split ownership (a TRANSFER-scoped upload seeding the
// immutable halves + this kernel rewriting the dynamic ones) was the
// stranded-upload-write class: the draw's barrier src carried only the
// last writer's scope, so the upload's bytes were never delivered to the
// DRAW_INDIRECT read (memory dependencies are not transitive; see
// todo.txt).
//
// The VS consumes: visible[SV_InstanceID] -> source slot -> instance.
// SV_InstanceID includes the command's firstInstance under SPIR-V, so
// bin p's draw walks EXACTLY its own visible range
// [offsets[p], offsets[p] + counts[p]).

#include "placement_common.hlsli"
#include "placement_cull.hlsli"
#include "gpu_platform.hlsli"

// Must match C++ PlacementDrawBuildPush (32 bytes): bins + indirect +
// protoTable BDAs + the runtime prototype count + pad. The immutable
// command halves live in the proto table (rows[proto].{indexCount,
// firstIndex, vertexOffset}), not the push.
struct PlacementDrawBuildPush
{
    GPU_PTR_READ(PlacementBinsBuffer) bins;               // 0
    GPU_PTR_WRITE(PlacementIndirectBuffer) indirect;     // 8
    GPU_PTR_READ(PlacementProtoTableBuffer) protoTable;  // 16
    uint protoCount; // 24
    uint pad0;        // 28
};

GPU_PUSH(PlacementDrawBuildPush, pc);

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint proto = groupId.x * 64 + threadId.x;
    if (proto >= pc.protoCount)
        return;
    const PlacementProtoRow row = pc.protoTable.Get().rows[proto];
    // The COMPLETE command: immutable halves from the proto table, dynamic
    // halves from the bins (the cull's counts + the prefix's offsets).
    PlacementIndirectCmd cmd;
    cmd.indexCount = row.indexCount;
    cmd.firstIndex = row.firstIndex;
    cmd.vertexOffset = row.vertexOffset;
    cmd.instanceCount =
        pc.bins.Get().v[PLACEMENT_BINS_COUNT_BASE + proto];
    cmd.firstInstance =
        pc.bins.Get().v[PLACEMENT_BINS_OFFSET_BASE + proto];
    pc.indirect.Get().c[proto] = cmd;
}
