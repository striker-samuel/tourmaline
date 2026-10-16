// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"
#include "vertex_pack.hlsli"

// Thin G-buffer writer (SSSR-style receiver layer — see
// Rendergraph/ssr.h + ssr.frag.hlsl): pairs with default.vert.hlsl
// (same VS as the lit path, so the world normal arrives interpolated
// at location 3; the scene push carries materialId + bMats).
//
// Output (RGBA8_UNorm, the "thin G-buffer" AMD SSSR consumes):
//   R,G = octahedral-encoded world surface normal (geometric — the
//         macro surface shape the depth-derivation used to approximate,
//         piecewise-planar per triangle = the wireframe source; the
//         real interpolated normal is smooth by construction)
//   B   = material roughness (continuous — drives the trace's
//         (1-r)^2 gloss fade + the r^2 cone jitter)
//   A   = metallic flag (Fresnel F0 rise to ~1)
//
// Bound in the Forward+ depth prepass Phase 1 (solid) so the G-buffer
// costs only a trivial FS + 4 bytes/px of ROP on a pass that already
// rasterizes every opaque surface — Main is untouched. Phase 2 (MASK
// cutout) uses gbuffer_masked.frag (this + the alpha test). Clear
// value (0.5, 0.5, 1, 0) decodes as normal +Z, roughness 1 (matte),
// metal 0 — "no data" reads as a safe non-reflector (split frames keep
// last frame's values via LoadOp::Load: static normals never change).

struct PSIn
{
    // Location 1 = default.vert's uv (material texture coordinates — the
    // metallicRoughness tap below needs them); location 3 = the world
    // normal output (explicit so the FS interface can be a subset of
    // the VS outputs).
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
};

struct PSOut
{
    float4 gbuffer : SV_Target0; // RGBA8: (oct.xy, roughness, metal)
};

// Octahedral encode (the inverse of vertex_pack.hlsli OctDecodeNormal;
// stored as xy*0.5+0.5 for the RGBA8 unorm target).
float2 OctEncodeNormal(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = (n.z >= 0.0f) ? n.xy
        : float2((1.0f - abs(n.y)) * OctSignH(n.x),
                 (1.0f - abs(n.x)) * OctSignH(n.y));
    return e * 0.5f + 0.5f;
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];
    // glTF metallicRoughness: the params are FACTORS, the final values
    // are factor x texture (G=roughness, B=metallic — the same tap the
    // lit path's non-layered branch does). The old writer stored the
    // bare factor, and a pak whose factors are all 1.0 (the common
    // multiplier default) read as a fully matte scene — the classify
    // then correctly emitted zero rays and SSR silently died.
    float roughness = mat.params.z;
    float metallic = mat.params.y;
    const int mrIdx = mat.tex.y;
    if (mrIdx >= 0)
    {
        Texture2D mrTex = textures[NonUniformResourceIndex((uint)mrIdx)];
        float4 mrSample = mrTex.Sample(samplers[0], IN.uv);
        roughness *= mrSample.g;
        metallic *= mrSample.b;
    }
    const float metalFlag = (saturate(metallic) >= 0.50f) ? 1.0f : 0.0f;
    float3 n = normalize(IN.normal);
    OUT.gbuffer = float4(OctEncodeNormal(n),
        clamp(roughness, 0.04f, 1.0f), metalFlag);
    return OUT;
}
