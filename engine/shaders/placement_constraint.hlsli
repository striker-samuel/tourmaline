// placement_constraint.hlsli — shared code for the min-distance passes.
// Mirrors ApplyMinDistance's snapshot rule: elect cell argmax on
// (priority, tie -> smaller candidateIndex), then kill vs the winner
// snapshot. Order-free by construction: elect reads positions/IDs only,
// kill reads the barrier-separated winner snapshot, every thread writes
// exactly one output element. Any thread order agrees bit-exactly
// (modulo the documented 1-ulp cell-boundary caveat).
//
// StableIDs are recomputed from candidateIndex (RenderInstance.customB),
// so no ID buffer is needed. candidateIndex also serves as the
// tie-break, matching the CPU (accept order preserves candidate order).

#ifndef PLACEMENT_CONSTRAINT_HLSLI
#define PLACEMENT_CONSTRAINT_HLSLI

#define PLACEMENT_CON_EMPTY 0xFFFFFFFF

// Must match C++ PlacementConParams (64 bytes, dx-layout).
struct PlacementConParams
{
    float3 origin;   // 0  constraint volume origin (world)
    float spacing;   // 12 cell size == min spacing
    uint dimX;       // 16
    uint dimY;       // 20
    uint dimZ;       // 24
    uint pad0;       // 28
    uint seedLo;     // 32
    uint seedHi;     // 36
    uint nodeId;     // 40
    uint pad1;       // 44
    int3 tile;       // 48
    uint pad2;       // 60
};

#define PLACEMENT_CON_MAX_INSTANCES 65536
#define PLACEMENT_CON_MAX_CELLS 262144 // 64^3

struct PlacementFlagBuffer { uint v[PLACEMENT_CON_MAX_INSTANCES]; };
struct PlacementHeadBuffer { uint v[PLACEMENT_CON_MAX_CELLS]; };
// Elect writes .x (winner), kill reads .x and writes .y (alive): the
// snapshot (.x) is never written while kill runs, so kill reads are
// race-free by construction.
struct PlacementFlag2Buffer { uint2 v[PLACEMENT_CON_MAX_INSTANCES]; };

// Shared push for all three passes (64 bytes, dx-layout).
// build: head + next. elect: head + next + flag2.x. kill reads
// head + next + flag2.x, writes flag2.y.
struct PlacementConPush
{
    vk::BufferPointer<PlacementInstanceBuffer> instances; // 0
    vk::BufferPointer<PlacementHeadBuffer> head;          // 8
    vk::BufferPointer<PlacementFlagBuffer> next;          // 16
    vk::BufferPointer<PlacementFlag2Buffer> flag2;        // 24
    vk::BufferPointer<PlacementConParams> prm;            // 32
    vk::BufferPointer<PlacementCountBuffer> countBuf;     // 40
    uint pad0;  // 48
    uint pad1;  // 52
    uint pad2;  // 56
    uint pad3;  // 60
};

// Volume-cell coords; valid=false invokes the skip rule (mesh overshoot).
bool PlacementConCell(PlacementConParams prm, float3 p,
    out int3 cell, out uint cellIdx)
{
    int3 c = int3(
        (int)floor((p.x - prm.origin.x) / prm.spacing),
        (int)floor((p.y - prm.origin.y) / prm.spacing),
        (int)floor((p.z - prm.origin.z) / prm.spacing));
    cell = c;
    cellIdx = 0;
    if (c.x < 0 || c.y < 0 || c.z < 0
        || (uint)c.x >= prm.dimX || (uint)c.y >= prm.dimY || (uint)c.z >= prm.dimZ)
        return false;
    cellIdx = ((uint)c.z * prm.dimY + (uint)c.y) * prm.dimX + (uint)c.x;
    return true;
}

// Full StableID from the candidate index (mirrors MakeStableID).
uint4 PlacementConId(PlacementConParams prm, uint candIndex)
{
    return PlacementMakeStableID(prm.seedLo, prm.seedHi, prm.nodeId,
        prm.tile, 0, candIndex);
}

uint2 PlacementConPriority(PlacementConParams prm, uint candIndex)
{
    uint4 id = PlacementConId(prm, candIndex);
    return PlacementHashChannel(id, PLACEMENT_CH_VARIANT);
}

// Strict "a loses to b": higher priority wins, ties go to the smaller
// candidate index. Exactly one side loses for any distinct pair.
bool PlacementConLosesTo(uint2 priA, uint candA, uint2 priB, uint candB)
{
    if (PlacementPriorityBeats(priB, priA))
        return true;
    if (PlacementPriorityBeats(priA, priB))
        return false;
    return candB < candA;
}

#endif
