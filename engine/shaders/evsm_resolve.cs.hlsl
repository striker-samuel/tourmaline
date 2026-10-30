// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// EVSM per-sample MSAA resolve (compute): 2x MSAA D32 -> single-sample
// RGBA32F moments.
//
// The caster pass renders DEPTH ONLY at 2x MSAA (the PCF shadow
// programs — shadow.vert + the depth-only opaque leg / alpha-tested
// shadow.frag leg — see RecordShadowCastersDirect): no MSAA color
// attachment, no per-fragment exp() in the raster path. The 2x MSAA
// DEPTH is the whole MSAA payload: at silhouette edges each sample
// carries its own coverage/depth, which is exactly the distribution
// information EVSM wants — per-SAMPLE depth, better than the old
// per-fragment color path (one pixel-center warp replicated to both
// samples).
//
// This pass reconstructs the moments from it: for each atlas texel,
// load BOTH depth samples, warp EACH sample separately, THEN average.
// Order matters — EVSM is nonlinear:
//   (e^{c z0} + e^{c z1}) / 2  !=  e^{c (z0+z1)/2}
// averaging depths first would fake a mid-depth occluder that does not
// exist; averaging the WARPED moments is the true per-sample coverage
// mix (the old 2x MSAA color + hardware resolve averaged the same
// population — this replaces it at a fraction of the bandwidth:
// 2x4B depth reads + 16B write per texel instead of a 2x RGBA32F color
// target + its resolve).
//
// Clear convention rides the DEPTH clear: uncovered/cleared samples read
// z = 1.0 (the far plane), and warp(1) IS the old EvsmClearColor far-plane
// moment set — no color clear, no clear gate changes.
//
// Moment range by construction: z in [0,1] -> warp in [e^-c, e^c], the
// squares in [e^-2c, e^2c] = 2.9e36 at c=42, comfortable fp32 headroom —
// and an average of in-range values is in-range, so the blur's input is
// clean by construction (its H-leg sanitize stays as defense-in-depth).
//
// Reads through the bindless MSAA array texturesMS[] (kBinding_MsaaTextures
// — UpdateDescriptors mirrors MSAA views at the same slot indices);
// writes through the shared bindless storage array (binding 2, same slot
// space as textures[] — see evsm_blur.hlsli). srcIndex/dstIndex are
// dispatch-uniform, so no NonUniformResourceIndex.
//
// C++ mirror: smvk::EvsmResolvePush in Engine/Rendergraph/evsm.h (16 bytes).

#define EVSM_RESOLVE_PASS
#include "common.hlsli"
#include "evsm.hlsli"
#include "gpu_platform.hlsli"

// Bindless storage image array (set 0, binding 2) — same declaration as
// evsm_blur.hlsli (the other storage-writing TU). float4 MUST match the
// RGBA32F moment atlas: an OpTypeImage Format operand mismatched against
// the view's VkFormat is undefined for the whole image.
[[vk::binding(2, 0)]]
RWTexture2D<float4> evsmStorage[];

struct EvsmResolvePush
{
    uint srcIndex;    // bindless texturesMS[] index of the 2x MSAA depth view
    uint dstIndex;    // bindless storage index of moments[flight]
    uint atlasWidth;  // atlas extent in texels (dispatch covers the atlas)
    uint atlasHeight;
};

GPU_PUSH(EvsmResolvePush, pcResolve);

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pcResolve.atlasWidth || tid.y >= pcResolve.atlasHeight)
        return;

    const int2 p = int2(tid.xy);
    Texture2DMS<float4> src = texturesMS[pcResolve.srcIndex];
    // 2x MSAA (desc.msaaSamples == 2, enforced on the C++ side).
    const float z0 = src.Load(p, 0).r;
    const float z1 = src.Load(p, 1).r;

    // Warp per sample, average AFTER (see the header: the nonlinear order).
    const float4 m =
        (EvsmWarp(saturate(z0), EVSM_EXPONENT)
            + EvsmWarp(saturate(z1), EVSM_EXPONENT)) * 0.5f;

    evsmStorage[pcResolve.dstIndex][uint2(p)] = m;
}
