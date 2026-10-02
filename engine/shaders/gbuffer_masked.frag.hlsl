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
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, pos.xy, pc.textureFeedbackEnabled != 0u);
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

    const float roughness =
        clamp(mat.params.z, 0.04f, 1.0f);
    const float metal =
        (mat.params.y >= 0.50f) ? 1.0f : 0.0f;
    float3 n = normalize(IN.normal);
    OUT.gbuffer = float4(OctEncodeNormal(n), roughness, metal);
    return OUT;
}
