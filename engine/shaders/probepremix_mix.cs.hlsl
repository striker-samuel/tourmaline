// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Probe pre-mix blend pass (docs/probe_premix_plan.md, pass B): each
// texel of the blended cubemap's (face, mip) is the selection-weighted
// sum of the source cubemaps at the SAME mip level - the Remember Me
// mip-to-mip rule (the GGX prefiltered chains are already baked per
// capture; the mix NEVER re-prefilters).
//
// One dispatch per mip (z = the face layer); the destination is THIS
// mip's 2D-array storage view in the bindless storage array (binding 2,
// the evsmStorage/spdImages slot space). The sources sample through
// the bindless cubemaps array (binding 5) with the blend weights the
// weights pass wrote (BDA; sel.indices[] are the cubemap bindless
// indices). GPU-owned end to end: no CPU readback anywhere.
//
// The face->direction mapping mirrors the CPU FaceUvToDirection
// (cubemap.h: Px, Nx, Py, Ny, Pz, Nz; texel-center uv -> the cubemap
// direction) so the blended content aligns with every source chain
// exactly.

#define PROBEPREMIX_MIX_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"

// Mirrors smvk::probepremix::BlendSelectionGpu (probepremix.h).
struct BlendSelectionGpu
{
    uint count;
    uint indices[4];
    float weights[4];
    float pos[3];
    float boxMinus[3];
    float boxPlus[3];
    uint pad0;
    uint pad1;
};

// Mirrors smvk::probepremix::MixPush (probepremix.h).
struct MixPush
{
    GPU_PTR_READ(BlendSelectionGpu) selection; // 0
    uint dstIndex;                                 // 8  this mip's storage view
    uint mip;                                      // 12
    uint faceSize;                                 // 16
    uint samplerIndex;                             // 20 bindless sampler (clamp, linear mips)
    uint mipCount;                                 // 24 (diagnostics)
    uint pad0;                                     // 28
};

GPU_PUSH(MixPush, pc);

// Bindless storage image ARRAY (set 0, binding 2): the same
// declaration space evsm_resolve/spd_downsample use (one unified
// bindless index space). Indexed by pc.dstIndex - THIS dispatch's mip
// 2D-array view (6 face layers). dstIndex is a dispatch-uniform push
// constant, so no NonUniformResourceIndex (the evsm_resolve rule:
// push constants are uniform across the dispatch). float4 matches
// the RGBA16F blended cubemap (format pairing must agree - see
// evsm_blur.hlsli). A SINGULAR declaration here would consume
// descriptor element 0 of the array (whatever view registered there -
// bloom/evsm, NOT this mip's view) and every mip's blend would land
// in it: the per-mip dispatches must INDEX the array, never address a
// single binding.
[[vk::binding(2, 0)]]
RWTexture2DArray<float4> premixStorage[];

// FaceUvToDirection (cubemap.h) in HLSL: texel-center uv in [0,1] ->
// the cubemap direction. Face order = CubemapFace: Px, Nx, Py, Ny,
// Pz, Nz (the captures' encoding convention).
float3 FaceDir(uint face, float2 uv)
{
    const float su = uv.x * 2.0f - 1.0f;
    const float sv = uv.y * 2.0f - 1.0f;
    if (face == 0u) return float3( 1.0f, -sv, -su);
    if (face == 1u) return float3(-1.0f, -sv,  su);
    if (face == 2u) return float3( su,  1.0f,  sv);
    if (face == 3u) return float3( su, -1.0f, -sv);
    if (face == 4u) return float3( su, -sv,  1.0f);
    return float3(-su, -sv, -1.0f);
}

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    const BlendSelectionGpu sel = pc.selection.Get();
    if (sel.count == 0u || tid.x >= pc.faceSize || tid.y >= pc.faceSize)
        return;

    // Texel-center uv of this (face, x, y) in the blended cubemap.
    const float2 uv = (float2(tid.xy) + 0.5f) / float(pc.faceSize);
    const float3 dir = normalize(FaceDir(tid.z, uv));

    // Zero-texel guard for the LOD: this dispatch serves ONE mip
    // (pc.mip) - the sources fetch that same level, mip-to-mip.
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    SamplerState smp = samplers[NonUniformResourceIndex(pc.samplerIndex)];
    [loop]
    for (uint s = 0; s < sel.count; ++s)
    {
        const uint cubemapIdx = sel.indices[s];
        if (cubemapIdx == 0xffffffffu || sel.weights[s] <= 0.0f)
            continue;
        TextureCube cube = cubemaps[NonUniformResourceIndex(cubemapIdx)];
        acc += sel.weights[s]
            * cube.SampleLevel(smp, dir, float(pc.mip)).rgb;
    }
    premixStorage[pc.dstIndex][
        int3(int(tid.x), int(tid.y), int(tid.z))] = float4(acc, 1.0f);
}
