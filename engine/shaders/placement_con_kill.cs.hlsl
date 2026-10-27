// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_con_kill.cs.hlsl — constraint pass 3/3: neighbourhood kill.
// One thread per accepted instance: when I am an elected winner, walk the
// 27 neighbouring cells; I die iff some OTHER winner conflicts
// (dist < spacing) and beats me under (priority, tie -> smaller
// candidateIndex). Reads the barrier-separated winner snapshot
// (flag2[].x, never written by this pass) and writes flag2[slot].y.
// Bit-exact vs CPU Phase 2 by construction.

#include "placement_common.hlsli"
#include "placement_constraint.hlsli"

[[vk::push_constant]]
ConstantBuffer<PlacementConPush> pc;

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    uint slot = groupId.x * 64 + threadId.x;
    // Raw count can exceed capacity on scatter overflow — clamp before
    // any items[]/flag2[] access (the capacity-guard contract).
    uint count = min(pc.countBuf.Get().count, pc.countBuf.Get().capacity);
    if (slot >= count)
        return;

    uint alive = 0;
    if (pc.flag2.Get().v[slot].x != 0)
    {
        float3 p = pc.instances.Get().items[slot].position;
        uint myCand = pc.instances.Get().items[slot].customB;
        int3 cell;
        uint cellIdx;
        alive = 1;
        if (PlacementConCell(pc.prm.Get(), p, cell, cellIdx))
        {
            uint2 myPri = PlacementConPriority(pc.prm.Get(), myCand);
            float minD2 = pc.prm.Get().spacing * pc.prm.Get().spacing;
            for (int dz = -1; dz <= 1 && alive != 0; ++dz)
            for (int dy = -1; dy <= 1 && alive != 0; ++dy)
            for (int dx = -1; dx <= 1 && alive != 0; ++dx)
            {
                int3 nc = cell + int3(dx, dy, dz);
                if (nc.x < 0 || nc.y < 0 || nc.z < 0
                    || (uint)nc.x >= pc.prm.Get().dimX
                    || (uint)nc.y >= pc.prm.Get().dimY
                    || (uint)nc.z >= pc.prm.Get().dimZ)
                    continue;
                uint nlin = ((uint)nc.z * pc.prm.Get().dimY + (uint)nc.y)
                    * pc.prm.Get().dimX + (uint)nc.x;
                for (uint m = pc.head.Get().v[nlin]; m != PLACEMENT_CON_EMPTY; m = pc.next.Get().v[m])
                {
                    if (m == slot)
                        continue;
                    if (pc.flag2.Get().v[m].x == 0)
                        continue; // not a winner: cannot kill
                    float3 q = pc.instances.Get().items[m].position;
                    float3 d = p - q;
                    if (dot(d, d) >= minD2)
                        continue;
                    uint candM = pc.instances.Get().items[m].customB;
                    uint2 priM = PlacementConPriority(pc.prm.Get(), candM);
                    if (PlacementConLosesTo(myPri, myCand, priM, candM))
                    {
                        alive = 0;
                        break;
                    }
                }
            }
        }
    }
    pc.flag2.Get().v[slot].y = alive;
}
