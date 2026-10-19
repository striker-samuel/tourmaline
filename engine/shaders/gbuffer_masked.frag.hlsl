// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"
#include "vertex_pack.hlsli"

// Thin G-buffer writer, MASK (cutout) variant — the gbuffer.frag.hlsl
// twin with the alpha test (see that file for the G-buffer layout).
// Pairs with default.vert.hlsl (uv at location 1 for the baseColor
// alpha sample, world normal at location 3). The FS discard runs
// BEFORE the output write, so cutout holes keep the pass's clear/
// loaded value exactly like they keep background depth (per-pixel
// cutout, same discipline as the prepass's shadow.frag legacy).

struct PSIn
{
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    // Dither-on-closeup inputs (Scene::ditherCloseup): pairs with
    // default.vert, which forwards the direct legs' per-draw pc word
    // (byte 240) flat here + the world position for the distance term
    // (the MAIN camera via FrameUniforms). Holes keep the loaded G-buffer
    // value exactly like they keep background depth.
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    // The lit family's double-sided flip (default_lit_body.hlsli): this
    // masked phase binds material-following cull (None for cutout cloth),
    // so back-facing fragments reach here carrying away-pointing
    // interpolated normals — flip like the lit FS or the stored oct normal
    // is inside-out for the cloth backs and the SSR/SSGI traces march
    // reflections off inverted normals.
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 gbuffer : SV_Target0; // RGBA8: (oct.xy, roughness, metal)
};

float2 OctEncodeNormal(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = (n.z >= 0.0f) ? n.xy
        : float2((1.0f - abs(n.y)) * OctSignH(n.x),
                 (1.0f - abs(n.x)) * OctSignH(n.y));
    return e * 0.5f + 0.5f;
}

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;

    // Dither-on-closeup (Scene::ditherCloseup; see common.hlsli Closeup-
    // DitherKeep): the G-buffer screen-doors in step with the depth and
    // the lit fade. First: discarded fragments skip the material fetch.
    if (IN.ditherCloseup != 0u)
    {
        FrameUniforms frameD = pc.frameUniforms.Get();
        if (!CloseupDitherKeep(IN.worldPos, frameD.cameraPos, pos.xy))
            discard;
    }

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // Alpha test (mirrors shadow_body.hlsli line-for-line — the leaf
    // SHAPES live in the baseColor texture's alpha, NOT the factor):
    // MASK cutout under the textured cutoff, dithered Blend under the
    // screen-door keep test — holes write neither depth nor G-buffer.
    const float alphaCutoff = mat.params.x;
    float baseAlpha = mat.diffuse.a;
    const int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        float4 s = baseTex.Sample(samplers[0], IN.uv);
        baseAlpha *= s.a;
    }
    const float alphaModeF = mat.params2.w;
    if (alphaModeF == 2.0f && pc.pad_pc == 0)
    {
        if (!DitherKeep(baseAlpha, pos.xy))
            discard;
    }
    else if (baseAlpha < alphaCutoff)
        discard;

    // glTF metallicRoughness factors x texture (the same tap as
    // gbuffer.frag / the lit path's non-layered branch — the bare
    // factor read the whole scene matte when the factors were the
    // multiplier-default 1.0).
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
    if (!IN.isFrontFace)
        n = -n; // the lit family's back-face flip (see PSIn)
    OUT.gbuffer = float4(OctEncodeNormal(n),
        clamp(roughness, 0.04f, 1.0f), metalFlag);
    return OUT;
}
