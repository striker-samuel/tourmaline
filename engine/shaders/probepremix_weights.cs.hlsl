// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Probe pre-mix weights pass (docs/probe_premix_plan.md, pass A): reads
// the RESIDENT probe tail through the fpLights buffer-address (the same
// type-2 entries the lit walk reads - the GPU-side selection we already
// have; NO CPU classification), evaluates the CURRENT validated weighting
// - fadeWeight^3 * influence^2 at the point of interest (the camera
// position) - selects the top-K (K = 4, deterministic tie-break:
// weight, then influence, then cubemap index), normalizes, and writes
// the selection for the mix pass + the material's dominant-box
// parallax:
//
//   indices[4] = the CUBEMAP bindless indices (the mix pass samples
//                cubemaps[indices[s]] directly - the light entry's
//                direction.y payload, not the light entry index);
//   weights[4] = the normalized blend weights (sum = 1 over the set);
//   boxMinus/boxPlus = the DOMINANT (slot 0) probe's packed parallax
//                box (the minus/plus plane distances - the same packing
//                the lit walk's MakeProbeLightEntry uses), mirrored
//                into FrameUniforms so the material's single-fetch
//                branch keeps the depth-fitted box parallax without
//                reading the light tail.
//
// Zero candidates -> count 0: the mix pass writes nothing and the
// material falls back to the per-pixel walk (then the global cubemap /
// analytic sky) unchanged - the pre-mix is a strict addition, never a
// regression path.
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
    float pos[3];          // 36 dominant probe's capture position
    float boxMinus[3];    // 48 dominant minus-plane distances
    float boxPlus[3];     // 60 dominant plus-plane distances
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
    float poiZ;                                       // 24
    uint lightCount;                                  // 28
    uint probeCount;                                  // 32
};

[[vk::push_constant]]
ConstantBuffer<WeightsPush> pc;

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

    // Top-K selection (K = 4): fixed insertion slots, deterministic
    // tie-break (blend weight, then influence, then cubemap index - the
    // wide-first policy the A/B validated).
    uint bestCube[4];
    float bestW[4];
    float bestInf[4];
    float3 bestPos[4];
    float bestBoxMinus[4][3];
    float bestBoxPlus[4][3];
    [unroll]
    for (uint s = 0; s < 4; ++s) {
        bestCube[s] = 0xffffffffu;
        bestW[s] = 0.0f;
        bestInf[s] = 0.0f;
        bestPos[s] = float3(0.0f, 0.0f, 0.0f);
        [unroll]
        for (uint a = 0; a < 3; ++a) {
            bestBoxMinus[s][a] = 0.0f;
            bestBoxPlus[s][a] = 0.0f;
        }
    }

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
        float weight = 0.0f;
        if (dist <= influenceRadius) {
            weight = 1.0f;
        } else if (fadeRadius > 0.0f && dist <= influenceRadius + fadeRadius) {
            weight = 1.0f - (dist - influenceRadius) / fadeRadius;
        }
        if (weight <= 0.0f)
            continue;

        // The CURRENT validated blend weight (environment.hlsli's
        // wide-break cubic): fade^3 * influence^2. The mix normalizes
        // over the selected set.
        const float w3 = weight * weight * weight;
        const float wEff = w3 * influenceRadius * influenceRadius;

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

        // Insertion: the slot this candidate belongs in. The comparison
        // key is (wEff, influence, cubemap index) - deterministic.
        [unroll]
        for (uint s = 0; s < 4; ++s) {
            const bool better = (wEff > bestW[s]) ||
                (wEff == bestW[s] && influenceRadius > bestInf[s]) ||
                (wEff == bestW[s] && influenceRadius == bestInf[s] &&
                    cubemapIdx < bestCube[s]);
            if (!better)
                continue;
            // Shift the tail down to make room.
            [unroll]
            for (uint t = 3; t > s; --t) {
                bestCube[t] = bestCube[t - 1];
                bestW[t] = bestW[t - 1];
                bestInf[t] = bestInf[t - 1];
                [unroll]
                for (uint a = 0; a < 3; ++a) {
                    bestBoxMinus[t][a] = bestBoxMinus[t - 1][a];
                    bestBoxPlus[t][a] = bestBoxPlus[t - 1][a];
                bestPos[t] = bestPos[t - 1];
                }
            }
            bestCube[s] = cubemapIdx;
            bestW[s] = wEff;
            bestInf[s] = influenceRadius;
            bestPos[s] = L.position;
            [unroll]
            for (uint a = 0; a < 3; ++a) {
                bestBoxMinus[s][a] = dNeg[a];
                bestBoxPlus[s][a] = dPos[a];
            }
            break;
        }
    }

    // Count = the number of selected slots with a valid candidate.
    uint k = 0;
    [unroll]
    for (uint s = 0; s < 4; ++s)
        if (bestCube[s] != 0xffffffffu)
            k = s + 1;

    BlendSelectionGpu sel;
    sel.pad0 = 0u;
    sel.pad1 = 0u;
    if (k == 0u) {
        // Zero candidates: the material falls back to the per-pixel
        // walk (count 0 disables the pre-mix branch; nothing changes).
        sel.count = 0u;
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
    [unroll]
    for (uint s = 0; s < 4; ++s) {
        sel.indices[s] = bestCube[s];
        sel.weights[s] = bestW[s] / sumW;
        // Dominant probe: slot 0 (the highest key) mirrors its capture
        // position + packed parallax box for the material's single-fetch
        // branch (environment.hlsli reads them via the selection BDA -
        // one unpack, no light-tail fetch).
        if (s == 0u) {
            sel.pos[0] = bestPos[s].x;
            sel.pos[1] = bestPos[s].y;
            sel.pos[2] = bestPos[s].z;
            [unroll]
            for (uint a = 0; a < 3; ++a) {
                sel.boxMinus[a] = bestBoxMinus[s][a];
                sel.boxPlus[a] = bestBoxPlus[s][a];
            }
        }
    }
    pc.selection.Get() = sel;
}
