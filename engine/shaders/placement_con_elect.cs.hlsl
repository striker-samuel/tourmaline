// placement_con_elect.cs.hlsl — constraint pass 2/3: cell pre-election.
// One thread per accepted instance: walk my cell's linked list; I win iff
// no list member beats me under (priority, tie -> smaller candidateIndex).
// Writes flag2[slot].x. Read-only over shared state — data-parallel,
// order-free, bit-exact vs CPU Phase 1.

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
    uint myCand = pc.instances.Get().items[slot].customB;
    int3 cell;
    uint cellIdx;
    if (!PlacementConCell(pc.prm.Get(), p, cell, cellIdx))
    {
        pc.flag2.Get().v[slot] = uint2(0, 0); // skip rule: never wins
        return;
    }

    uint2 myPri = PlacementConPriority(pc.prm.Get(), myCand);
    bool win = true;
    for (uint m = pc.head.Get().v[cellIdx]; m != PLACEMENT_CON_EMPTY; m = pc.next.Get().v[m])
    {
        if (m == slot)
            continue;
        uint candM = pc.instances.Get().items[m].customB;
        uint2 priM = PlacementConPriority(pc.prm.Get(), candM);
        if (PlacementConLosesTo(myPri, myCand, priM, candM))
        {
            win = false;
            break;
        }
    }
    pc.flag2.Get().v[slot] = uint2(win ? 1 : 0, 0);
}
