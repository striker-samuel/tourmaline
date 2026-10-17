// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Probe pre-mix weights pass (docs/probe_premix_plan.md, pass A; Phase 2
// = the Remember Me NDF transition-region weights). Reads the RESIDENT
// probe tail through the fpLights buffer-address (the same type-2 entries
// the lit walk reads - the GPU-side selection we already have; NO CPU
// classification), and writes the selection for the mix pass + the
// material's dominant-box parallax:
//
//   indices[4] = the CUBEMAP bindless indices (the mix pass samples
//                cubemaps[indices[s]] directly);
//   weights[4] = the normalized blend weights (sum = 1 over the set);
//   pos/boxMinus/boxPlus = the WEIGHT-LERPED parallax box across the
//                selected set (the same packing the lit walk's
//                MakeProbeLightEntry uses; lerping positions + plane
//                distances = lerping the AABB corners), mirrored through
//                the FrameUniforms selection BDA so the material's
//                single-fetch branch keeps the depth-fitted box parallax
//                WITHOUT popping when the dominant slot swaps.
//
// The NDF weighting (Lagarde, GPU Pro 4 - the CPU mirror ComputeNdfWeights
// in probepremix.h, pinned by test_cubemap.cpp):
//   d_i = (dist - influence) / fade   clamped [0,1]  (<=0 inner, (0,1)
//        transition, >=1 out - dropped)
//   inner hit (d <= 0): the DESIGNATED DOMINANT gets EXCLUSIVE weight
//        (ties: widest influence wins, then lowest cubemap index - the
//        validated wide-first policy; the documented Remember Me
//        limitation at inner-region exits applies)
//   one candidate: weight 1 (the a-term divides by N-1 = 0)
//   zero candidates: count 0 (the material falls back to the per-pixel
//        walk unchanged - the zero-regression contract)
//   transition region: the published a*b weighting:
//        a_i = (1 - d_i / S_d) / (N - 1),  b_i = (1 - d_i) / S_r,
//        W_i = a_i * b_i (normalized over the selected set)
//   K selection (the mix budget = 4): top-K by W (ties: influence, then
//        cubemap index), re-normalized over the selected.
// The transition formula makes the blend spatially varying even where two
// influence volumes overlap fully: the probe nearer its inner region
// outweighs the outer one - the original's point.
//
// Single group, one thread (the tail is <= 64 entries). GPU-owned end
// to end: no CPU readback, no classification, no per-object selection.

#define PROBEPREMIX_WEIGHTS_PASS
#include "common.hlsli"

// Mirrors smvk::probepremix::BlendSelectionGpu (probepremix.h).
struct BlendSelectionGpu
{
    uint count;          // 0
    uint indices[4];     // 4  cubemap bindless indices (the mix samples these)
    float weights[4];     // 20 normalized (sum = 1 over the set)
    float pos[3];          // 36 weight-lerped capture position
    float boxMinus[3];    // 48 weight-lerped minus-plane distances
    float boxPlus[3];     // 60 weight-lerped plus-plane distances
    uint pad0;            // 72
    uint pad1;            // 76
};

// Mirrors smvk::probepremix::WeightsPush (probepremix.h).
struct WeightsPush
{
    vk::BufferPointer<ForwardPlusLightBuffer> lights; // 0
    vk::BufferPointer<BlendSelectionGpu> selection;   // 8
    float poiX;                                       // 16
    float poiY;                                       // 20
    float poiZ;                                        // 24
    uint lightCount;                                  // 28
    uint probeCount;                                  // 32
    uint pad0;                                         // 36
    uint pad1;                                         // 40
};

[[vk::push_constant]]
ConstantBuffer<WeightsPush> pc;

// Gathered candidate (in-influence only; NDF in [0, 1)). The tail cap:
// kForwardPlusMaxProbes (64, the resident budget) - the walk never sees
// more, and the graph buffer is sized for it.
struct NdfEntry
{
    uint cube;              // cubemap bindless index
    float ndf;              // clamped [0, 1)
    float influence;         // tie-break (wide wins)
    float3 pos;              // capture position
    float3 boxMinus;         // packed minus-plane distances
    float3 boxPlus;         // packed plus-plane distances
};

[numthreads(1, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    (void)tid;
    const uint total = pc.lightCount + pc.probeCount;
    // Guard: the tail must exist (the host only appends the passes when
    // probeCount > 0, but clamp defensively so a stale window can never
    // walk past the buffer's written range).
    if (total > (uint)MAX_FP_LIGHTS)
        return;
    const uint tailMax = 64; // kForwardPlusMaxProbes (the resident cap)

    // ---- Gather (pass 1 over the tail): in-influence candidates ------
    NdfEntry tail[64];
    uint n = 0;
    const float3 poi = float3(pc.poiX, pc.poiY, pc.poiZ);
    [loop]
    for (uint li = pc.lightCount; li < total; ++li)
    {
        ForwardPlusLight L = pc.lights.Get().lights[li];
        if (L.type != kForwardPlusProbe)
            continue; // direct light: the light loop owns it
        uint cubemapIdx = (uint)L.direction.y;
        if (cubemapIdx == 0xffffffffu)
            continue;
        float influenceRadius = max(L.range - L.direction.x, 0.0f);
        float fadeRadius = max(L.direction.x, 0.0f);
        float dist = length(poi - L.position);
        // NDF: 0 inside the inner region (influence), 1 at the outer
        // boundary (influence + fade). fade == 0 (a hard sphere): inside
        // = inner (0), outside = out (drop).
        float ndf = 1.0f;
        if (dist <= influenceRadius) {
            ndf = 0.0f;
        } else if (fadeRadius > 0.0f && dist <= influenceRadius + fadeRadius) {
            ndf = (dist - influenceRadius) / fadeRadius;
        }
        if (ndf >= 1.0f)
            continue; // outside the outer boundary - not a candidate
        if (n >= tailMax)
            break;

        // The packed depth-fitted box (forwardplus.h): color.rgb =
        // minus-plane distances, (intensity, inner, outer) = plus-plane
        // distances; all-zero = the symmetric influence cube.
        float3 dNeg = L.color.rgb;
        float3 dPos = float3(L.intensity, L.innerAngle, L.outerAngle);
        if (dot(dNeg, dNeg) + dot(dPos, dPos) <= 1e-10f) {
            const float h = max(influenceRadius, 0.05f);
            dNeg = float3(h, h, h);
            dPos = float3(h, h, h);
        }

        tail[n].cube = cubemapIdx;
        tail[n].ndf = ndf;
        tail[n].influence = influenceRadius;
        tail[n].pos = L.position;
        tail[n].boxMinus = dNeg;
        tail[n].boxPlus = dPos;
        ++n;
    }

    BlendSelectionGpu sel;
    sel.pad0 = 0u;
    sel.pad1 = 0u;
    [unroll]
    for (uint s = 0; s < 4; ++s) {
        sel.indices[s] = 0xffffffffu;
        sel.weights[s] = 0.0f;
    }
    sel.pos[0] = 0.0f; sel.pos[1] = 0.0f; sel.pos[2] = 0.0f;
    [unroll]
    for (uint a = 0; a < 3; ++a) {
        sel.boxMinus[a] = 0.0f;
        sel.boxPlus[a] = 0.0f;
    }

    // ---- Special cases (the ComputeNdfWeights evaluation order) ------
    if (n == 0u) {
        // Zero candidates: count 0 (the material falls back to the
        // per-pixel walk; nothing changes).
        sel.count = 0u;
        pc.selection.Get() = sel;
        return;
    }

    // An INNER hit (d <= 0): the DESIGNATED DOMINANT gets EXCLUSIVE
    // weight. Ties: widest influence wins, then the lowest cubemap index
    // (the validated wide-first policy).
    int dominant = -1;
    [loop]
    for (uint i = 0; i < n; ++i) {
        if (tail[i].ndf > 0.0f)
            continue;
        if (dominant < 0
            || tail[i].influence > tail[dominant].influence
            || (tail[i].influence == tail[dominant].influence
                && tail[i].cube < tail[dominant].cube))
            dominant = int(i);
    }
    if (dominant >= 0) {
        sel.count = 1u;
        sel.indices[0] = tail[dominant].cube;
        sel.weights[0] = 1.0f;
        sel.pos[0] = tail[dominant].pos.x;
        sel.pos[1] = tail[dominant].pos.y;
        sel.pos[2] = tail[dominant].pos.z;
        [unroll]
        for (uint a = 0; a < 3; ++a) {
            sel.boxMinus[a] = tail[dominant].boxMinus[a];
            sel.boxPlus[a] = tail[dominant].boxPlus[a];
        }
        pc.selection.Get() = sel;
        return;
    }

    // One candidate: the a-term degenerates (N-1 = 0) - the exclusive
    // weight 1 (the published fix).
    if (n == 1u) {
        sel.count = 1u;
        sel.indices[0] = tail[0].cube;
        sel.weights[0] = 1.0f;
        sel.pos[0] = tail[0].pos.x;
        sel.pos[1] = tail[0].pos.y;
        sel.pos[2] = tail[0].pos.z;
        [unroll]
        for (uint a = 0; a < 3; ++a) {
            sel.boxMinus[a] = tail[0].boxMinus[a];
            sel.boxPlus[a] = tail[0].boxPlus[a];
        }
        pc.selection.Get() = sel;
        return;
    }

    // ---- Transition region: the published a*b weighting ---------------
    float sumD = 0.0f, sumR = 0.0f;
    {
        [loop]
        for (uint i = 0; i < n; ++i) {
            sumD += tail[i].ndf;
            sumR += (1.0f - tail[i].ndf);
        }
        sumD = max(sumD, 1e-6f);
        sumR = max(sumR, 1e-6f);
    }

    // ---- Top-K selection (K = 4, the mix budget) ----------------------
    // Key: (W, influence, cubemap index) - deterministic.
    uint bestCube[4];
    float bestW[4];
    float bestInf[4];
    float3 bestPos[4];
    float3 bestBoxMinus[4];
    float3 bestBoxPlus[4];
    [unroll]
    for (uint s = 0; s < 4; ++s) {
        bestCube[s] = 0xffffffffu;
        bestW[s] = 0.0f;
        bestInf[s] = 0.0f;
        bestPos[s] = float3(0.0f, 0.0f, 0.0f);
        bestBoxMinus[s] = float3(0.0f, 0.0f, 0.0f);
        bestBoxPlus[s] = float3(0.0f, 0.0f, 0.0f);
    }
    [loop]
    for (uint i = 0; i < n; ++i)
    {
        const float d = tail[i].ndf;
        const float a = (1.0f - d / sumD) / float(n - 1);
        const float b = (1.0f - d) / sumR;
        const float w = max(a, 0.0f) * max(b, 0.0f);
        if (w <= 0.0f)
            continue;
        [unroll]
        for (uint s = 0; s < 4; ++s) {
            const bool better = (w > bestW[s]) ||
                (w == bestW[s] && tail[i].influence > bestInf[s]) ||
                (w == bestW[s] && tail[i].influence == bestInf[s]
                    && tail[i].cube < bestCube[s]);
            if (!better)
                continue;
            // Shift the tail down to make room.
            [unroll]
            for (uint t = 3; t > s; --t) {
                bestCube[t] = bestCube[t - 1];
                bestW[t] = bestW[t - 1];
                bestInf[t] = bestInf[t - 1];
                bestPos[t] = bestPos[t - 1];
                bestBoxMinus[t] = bestBoxMinus[t - 1];
                bestBoxPlus[t] = bestBoxPlus[t - 1];
            }
            bestCube[s] = tail[i].cube;
            bestW[s] = w;
            bestInf[s] = tail[i].influence;
            bestPos[s] = tail[i].pos;
            bestBoxMinus[s] = tail[i].boxMinus;
            bestBoxPlus[s] = tail[i].boxPlus;
            break;
        }
    }

    // Count = the number of selected slots with a valid candidate.
    uint k = 0;
    [unroll]
    for (uint s = 0; s < 4; ++s)
        if (bestCube[s] != 0xffffffffu)
            k = s + 1;
    if (k == 0u) {
        sel.count = 0u;
        pc.selection.Get() = sel;
        return;
    }

    // Normalize over the SELECTED set (the mix weights sum to 1).
    float sumW = 0.0f;
    [unroll]
    for (uint s = 0; s < 4; ++s)
        sumW += bestW[s];
    sumW = max(sumW, 1e-6f);

    sel.count = k;
    // The parallax box: WEIGHT-LERPED across the selected set (the plan's
    // Phase 3) - lerping the capture positions AND the plane distances by
    // the same renormalized weights the mix blends radiance with is
    // EXACTLY lerping the AABB corners (pos -/+ distance is linear in
    // both), so the single box the material's one-fetch branch uses
    // tracks the blend smoothly - no popping when the dominant slot
    // swaps while moving (the temporal property; static captures can't
    // show it - the slot-0 box alone pops at slot swaps).
    float3 lerpedPos = float3(0.0f, 0.0f, 0.0f);
    float3 lerpedMinus = float3(0.0f, 0.0f, 0.0f);
    float3 lerpedPlus = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint s = 0; s < 4; ++s) {
        sel.indices[s] = bestCube[s];
        // Slots beyond count carry weight 0 (their indices stay
        // 0xffffffff - the mix pass iterates [0, count) only).
        const float w = (s < k) ? bestW[s] / sumW : 0.0f;
        sel.weights[s] = w;
        lerpedPos += w * bestPos[s];
        lerpedMinus += w * bestBoxMinus[s];
        lerpedPlus += w * bestBoxPlus[s];
    }
    sel.pos[0] = lerpedPos.x;
    sel.pos[1] = lerpedPos.y;
    sel.pos[2] = lerpedPos.z;
    [unroll]
    for (uint a = 0; a < 3; ++a) {
        sel.boxMinus[a] = lerpedMinus[a];
        sel.boxPlus[a] = lerpedPlus[a];
    }
    pc.selection.Get() = sel;
}
