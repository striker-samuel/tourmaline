// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_hf_tile.cs.hlsl — GPU tile evaluation, heightfield domain.
// Mirrors CPU EvaluateTile() for heightfield layers: uniform XZ pick +
// bilinear height + central-difference normal, density field, quantized
// accept, weighted prototype, yaw + uniform scale, atomic append.

#include "placement_common.hlsli"
#include "placement_field.hlsli"
#include "placement_domains.hlsli"

// Must match C++ PlacementScatterPush (64 bytes).
struct PlacementScatterPush
{
    vk::BufferPointer<PlacementInstanceBuffer> outInstances; // 0
    vk::BufferPointer<PlacementCountBuffer> outCount;        // 8
    vk::BufferPointer<PlacementDomainParams> prm;            // 16
    uint64_t pad0;                                           // 24
    uint seedLo;      // 32
    uint seedHi;      // 36
    uint nodeId;      // 40
    uint candidateCount; // 44
    int3 tile;        // 48
    uint flags;       // 60
};

[[vk::push_constant]]
ConstantBuffer<PlacementScatterPush> pc;

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    uint i = groupId.x * 64 + threadId.x;
    if (i >= pc.candidateCount)
        return;

    uint4 id = PlacementMakeStableID(pc.seedLo, pc.seedHi, pc.nodeId,
        pc.tile, 0, i);

    // ---- Heightfield sample (mirrors HeightfieldDomain::sample). ----
    float2 j = PlacementRandom02(id, PLACEMENT_CH_POSITION);
    float x = pc.prm.Get().extMinX + j.x * (pc.prm.Get().extMaxX - pc.prm.Get().extMinX);
    float z = pc.prm.Get().extMinZ + j.y * (pc.prm.Get().extMaxZ - pc.prm.Get().extMinZ);
    float y = PlacementHfHeight(pc.prm.Get(), x, z);
    float3 nrm = PlacementHfNormal(pc.prm.Get(), x, z);
    float3 p = float3(x, y, z);

    // ---- Density + accept. ----
    float q = PlacementScatterDensity(pc.prm.Get(), p, pc.seedLo, pc.seedHi);
    bool keep = (q >= 1.0f) || (PlacementRandom01(id, PLACEMENT_CH_DENSITY) < q);
    if (!keep)
        return;

    // ---- Prototype + transform (own channels). ----
    float rv = PlacementRandom01(id, PLACEMENT_CH_VARIANT);
    uint proto = PlacementScatterProto(rv, pc.prm.Get());
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
