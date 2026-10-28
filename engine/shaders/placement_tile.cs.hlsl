// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_tile.cs.hlsl — GPU tile evaluation, grid-domain MVP.
// One thread per candidate (any order — no atomics except the final
// append). Mirrors CPU EvaluateTile() exactly: candidate -> StableID ->
// grid sample -> THE PROGRAM (PlacementEvaluateField: the actual graph
// SSA, every FieldOp) -> densityScale -> quantized stochastic accept ->
// weighted prototype -> yaw+uniform scale -> bounded append.
//
// The mesh-surface alias sampling + heightfield sampling live in their
// own kernels (same evaluator, same push family or their params);
// min-distance is the constraint trio; the CPU EvaluateTile() in
// placement_tile.h is the oracle for all paths.

#include "placement_common.hlsli"
#include "placement_field.hlsli"

// Must match C++ PlacementTilePush (168 bytes).
struct PlacementTilePush
{
    vk::BufferPointer<PlacementInstanceBuffer> outInstances; // 0
    vk::BufferPointer<PlacementCountBuffer> outCount;        // 8
    uint seedLo;      // 16
    uint seedHi;      // 20
    uint nodeId;      // 24
    uint candidateCount; // 28
    int3 tile;        // 32
    uint pad0;        // 44
    float3 gridOrigin;// 48
    uint cellsX;      // 60
    float2 gridExtent;// 64
    uint cellsZ;      // 72
    uint pad1;        // 76
    float densityScale; // 80 (was: the const-extracted density shortcut)
    float noiseAmpRetired;  // 84 RETIRED: 0 (the program subsumes noise)
    float noiseFreqRetired; // 88 RETIRED: 0
    uint pad2;        // 92
    float minScale;   // 96
    float maxScale;   // 100
    uint protoCount;  // 104
    uint pad3;        // 108
    float4 weightsA;  // 112
    float4 weightsB;  // 128
    // ---- the program tail (PR02: the graph SSA) ----
    vk::BufferPointer<PlacementInstrBuffer> instrs; // 144
    uint instrCount;   // 152
    uint acceptIndex;  // 156
    uint pad4;         // 160
    uint pad5;         // 164
};

[[vk::push_constant]]
ConstantBuffer<PlacementTilePush> pc;

uint PlacementSelectProto(float r, uint count, float4 wA, float4 wB)
{
    float total = wA.x + wA.y + wA.z + wA.w + wB.x + wB.y + wB.z + wB.w;
    if (count == 0 || total <= 0.0f)
        return 0;
    float target = clamp(r, 0.0f, 0.99999994f) * total;
    float acc = 0.0f;
    acc += wA.x; if (target < acc) return 0;
    acc += wA.y; if (target < acc) return min(1, (int)count - 1);
    acc += wA.z; if (target < acc) return min(2, (int)count - 1);
    acc += wA.w; if (target < acc) return min(3, (int)count - 1);
    acc += wB.x; if (target < acc) return min(4, (int)count - 1);
    acc += wB.y; if (target < acc) return min(5, (int)count - 1);
    acc += wB.z; if (target < acc) return min(6, (int)count - 1);
    return min(7, (int)count - 1);
}

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    uint i = groupId.x * 64 + threadId.x;
    if (i >= pc.candidateCount)
        return;

    // ---- Candidate ID (order-independent). ----
    uint4 id = PlacementMakeStableID(pc.seedLo, pc.seedHi, pc.nodeId,
        pc.tile, 0, i);

    // ---- Grid domain sample (mirrors GridDomain::sample). ----
    uint cellCount = max(pc.cellsX * pc.cellsZ, 1);
    uint cell = i % cellCount;
    uint cx = cell % max(pc.cellsX, 1);
    uint cz = cell / max(pc.cellsX, 1);
    float2 j = PlacementRandom02(id, PLACEMENT_CH_POSITION);
    float fx = ((float)cx + j.x) / (float)max(pc.cellsX, 1);
    float fz = ((float)cz + j.y) / (float)max(pc.cellsZ, 1);
    float3 p = float3(pc.gridOrigin.x + fx * pc.gridExtent.x,
                      pc.gridOrigin.y,
                      pc.gridOrigin.z + fz * pc.gridExtent.y);

    // ---- THE PROGRAM: the actual graph SSA (PR02). ----
    // Density order mirrors the CPU contract exactly:
    //   d = EvaluateField(program, sample)[acceptIndex]
    //   q = QuantizeDensity(d * layer.densityScale)
    //   keep = Random01(id, Density) < q
    // (last-ULP evaluator divergence is absorbed by the quantization).
    float3 nrm = float3(0.0f, 1.0f, 0.0f); // grid domain: flat base plane
    float d = PlacementEvaluateField(pc.instrs, pc.instrCount,
        pc.acceptIndex, p, nrm, pc.seedLo, pc.seedHi);
    float q = PlacementQuantizeDensity(d * pc.densityScale);
    bool keep = (q >= 1.0f) || (PlacementRandom01(id, PLACEMENT_CH_DENSITY) < q);
    if (!keep)
        return;

    // ---- Prototype + transform (own channels). ----
    float rv = PlacementRandom01(id, PLACEMENT_CH_VARIANT);
    uint proto = PlacementSelectProto(rv, pc.protoCount, pc.weightsA, pc.weightsB);
    float yaw = (PlacementRandom01(id, PLACEMENT_CH_ROTATION) * 2.0f - 1.0f) * 3.14159265f;
    float sc = pc.minScale + PlacementRandom01(id, PLACEMENT_CH_SCALE) * (pc.maxScale - pc.minScale);

    PlacementRenderInstance inst;
    inst.position = p;
    inst.yaw = yaw;
    inst.scale = float3(sc, sc, sc);
    inst.prototypeId = proto;
    inst.stableLow = id.x;
    inst.customB = i; // candidate index: constraint passes recompute IDs

    // ---- Bounded append (the capacity guard). ----
    // count is the RAW result: on overflow it stays > capacity (each
    // over-capacity thread still bumped it) and every consumer clamps
    // with min(count, capacity). The dropped-accepted count lands in
    // overflow — a diagnostic error, never a silent truncation.
    uint slot;
    InterlockedAdd(pc.outCount.Get().count, 1u, slot);
    if (slot >= pc.outCount.Get().capacity)
    {
        InterlockedAdd(pc.outCount.Get().overflow, 1u);
        return;
    }
    pc.outInstances.Get().items[slot] = inst;
}
