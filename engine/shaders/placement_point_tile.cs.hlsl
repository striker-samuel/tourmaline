// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_point_tile.cs.hlsl — GPU evaluation, authored point cloud domain.
// Mirrors CPU EvaluateTile() for point-cloud layers: indexed point pick,
// density field, quantized accept, weighted prototype,
// yaw + uniform scale, atomic append. customB carries the candidate index
// for the downstream constraint passes.
//
// The optional slope-to-rock rule follows the CPU oracle. Min-distance
// is handled by the separate constraint kernels.

#include "placement_common.hlsli"
#include "placement_field.hlsli"
#include "placement_domains.hlsli"
#include "gpu_platform.hlsli"

// Must match C++ PlacementScatterPush (64 bytes).
struct PlacementScatterPush
{
    GPU_PTR_WRITE(PlacementInstanceBuffer) outInstances; // 0
    GPU_PTR_WRITE(PlacementCountBuffer) outCount;        // 8
    GPU_PTR_READ(PlacementDomainParams) prm;            // 16
    uint64_t pad0;                                           // 24
    uint seedLo;      // 32
    uint seedHi;      // 36
    uint nodeId;      // 40
    uint candidateCount; // 44
    int3 tile;        // 48
    uint flags;       // 60
};

GPU_PUSH(PlacementScatterPush, pc);

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    uint i = groupId.x * 64 + threadId.x;
    if (i >= pc.candidateCount)
        return;

    PlacementDomainParams prm = pc.prm.Get();
    // One painted point maps to one candidate. Never wrap when the
    // graph's candidate limit is higher than the source point count.
    if (i >= prm.vertCount || prm.domainKind != 3u)
        return;
    uint4 id = PlacementMakeStableID(pc.seedLo, pc.seedHi, pc.nodeId,
        pc.tile, 0, i);
    float3 p = prm.pos.Get().p[i].xyz;
    float3 nrm = float3(0.0f, 1.0f, 0.0f);
    if ((prm.flags & 1u) != 0u)
    {
        float3 inputN = prm.nrm.Get().n[i].xyz;
        if (dot(inputN, inputN) > 1e-12f)
            nrm = normalize(inputN);
    }

    // ---- THE PROGRAM + accept (PR02: the actual graph SSA). ----
    PlacementEcoMask eco;
    eco.weights = pc.prm.Get().eco;
    eco.channels = pc.prm.Get().ecoChannels;
    eco.w = pc.prm.Get().ecoW;
    eco.h = pc.prm.Get().ecoH;
    eco.ox = pc.prm.Get().ecoOX;
    eco.oz = pc.prm.Get().ecoOZ;
    eco.sx = pc.prm.Get().ecoSX;
    eco.sz = pc.prm.Get().ecoSZ;
    float d = PlacementEvaluateField(pc.prm.Get().instrs,
        pc.prm.Get().instrCount, pc.prm.Get().acceptIndex,
        p, nrm, pc.seedLo, pc.seedHi, eco);
    float q = PlacementQuantizeDensity(d * pc.prm.Get().densityScale);
    bool keep = (q >= 1.0f) || (PlacementRandom01(id, PLACEMENT_CH_DENSITY) < q);
    if (!keep)
        return;

    // ---- Prototype + transform (own channels). ----
    float rv = PlacementRandom01(id, PLACEMENT_CH_VARIANT);
    uint proto = PlacementScatterProto(rv, pc.prm.Get());
    proto = PlacementSlopeToRock(proto, nrm, pc.prm.Get().protoCount,
        pc.prm.Get().slopeToRock, pc.prm.Get().slopeThresholdDeg);
    float yaw = (PlacementRandom01(id, PLACEMENT_CH_ROTATION) * 2.0f - 1.0f) * 3.14159265f;
    float sc = pc.prm.Get().minScale
        + PlacementRandom01(id, PLACEMENT_CH_SCALE)
        * (pc.prm.Get().maxScale - pc.prm.Get().minScale);

    PlacementRenderInstance inst;
    inst.position = p;
    inst.yaw = yaw;
    inst.scale = float3(sc, sc, sc);
    inst.prototypeId = proto;
    inst.stableLow = id.x;
    inst.customB = i;

    // ---- Bounded append (the capacity guard; see placement_common). ----
    uint slot;
    InterlockedAdd(pc.outCount.Get().count, 1u, slot);
    if (slot >= pc.outCount.Get().capacity)
    {
        InterlockedAdd(pc.outCount.Get().overflow, 1u);
        return;
    }
    pc.outInstances.Get().items[slot] = inst;
}
