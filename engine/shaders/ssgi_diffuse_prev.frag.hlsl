// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSGI_DIFFUSE_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"

// Diffuse SSGI history mirror: 1:1 copies of the final renderDepth
// (R32F) and the thin G-buffer's oct world normal (RGBA8, .xy only),
// written once per TEMPORAL record at the END of the diffuse chain
// (this record's temporal pass read the PREVIOUS copy — the rendergraph
// orders the mirror's writes after the temporal pass's reads).
//
// The NEXT record's disocclusion tests sample these at the reprojected
// coordinate: the history validation needs the depth AND the surface
// orientation of the surface the HISTORY GI came from — last frame's
// values at a last-frame coordinate (the CURRENT buffers at that
// coordinate are a different camera's geometry under motion; see
// ssr_temporal.frag.hlsl's disocclusion note). The depth half is the
// SSR depth-history pattern verbatim (ssr_depth_history.frag.hlsl);
// the normal half is the diffuse chain's addition (ssr_diffuse's
// "history validation based on previous depth AND normals").
//
// SV_Position -> exact texel Loads: 1:1, no samplers, no derivatives;
// the depth rides .r unchanged (reversed-Z), the normal rides .xy
// (oct, the G-buffer's stored domain — decode happens at the test).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outDepth : SV_Target0;   // R32F mirror (r = reversed-Z depth)
    float4 outNormal : SV_Target1;  // RGBA8 mirror (xy = oct world normal)
};

// Must match Engine/Rendergraph/ssgi_diffuse.h SsgiDiffusePrevPush
// (16 bytes).
struct SsgiDiffusePrevPush
{
    uint depthTextureIndex;   // full-res final depth (bindless, Load'ed)
    uint gbufferTextureIndex; // full-res thin G-buffer (oct normal .xy)
    uint pad0;
    uint pad1;
};

GPU_PUSH(SsgiDiffusePrevPush, pp);

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    const int2 px = int2(pos.xy);
    Texture2D depthTex = textures[NonUniformResourceIndex(pp.depthTextureIndex)];
    OUT.outDepth = float4(depthTex.Load(int3(px, 0)).r, 0.0f, 0.0f, 1.0f);
    Texture2D gbTex = textures[NonUniformResourceIndex(pp.gbufferTextureIndex)];
    float4 gb = gbTex.Load(int3(px, 0));
    // The G-buffer texel verbatim (xy oct; z rough / w metal ride along
    // unused — the .zw channels stay free for future metadata).
    OUT.outNormal = float4(gb.xy, 0.0f, 1.0f);
    return OUT;
}
