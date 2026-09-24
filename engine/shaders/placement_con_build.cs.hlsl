// placement_con_build.cs.hlsl — constraint pass 1/3: spatial grid build.
// One thread per accepted instance (slot < count): compute the volume cell
// and push the slot onto that cell's linked list (classic lock-free
// head-exchange; 32-bit atomics only, no Int64 anywhere).
// Out-of-volume instances link nothing (skip rule, same as CPU).

#include "placement_common.hlsli"
#include "placement_constraint.hlsli"

[[vk::push_constant]]
ConstantBuffer<PlacementConPush> pc;

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    uint slot = groupId.x * 64 + threadId.x;
    uint count = pc.countBuf.Get().count;
    if (slot >= count)
        return;

    float3 p = pc.instances.Get().items[slot].position;
    int3 cell;
    uint cellIdx;
    if (!PlacementConCell(pc.prm.Get(), p, cell, cellIdx))
        return; // skip rule: outside the volume

    uint old;
    InterlockedExchange(pc.head.Get().v[cellIdx], slot, old);
    pc.next.Get().v[slot] = old;
}
