#include "common.hlsli"

// Vertex-color debug view (F6): visualizes the COLOR0 stream the
// layered-material path blends with (see default_lit.frag.hlsl).
//   layered (layerMaterials.w > 1): normalized per-layer weights as a
//     false-color mix — R/G/B/Y per layer 0..3, using the same palette
//     as VULKANTOY_LAYERS_FLAT so flat-factor and blended views compare
//     directly. Corners isolate single layers, edges blend pairs.
//   legacy (w == 0): raw vertex-color RGB (plain albedo tint path).
// Pairs with default.vert.hlsl (same PSIn as default_lit). Alpha cutout
// + dithered Blend discard match the lit path so silhouettes agree.
//
// Like the other debug views the output is pre-scaled by 1/exposure: it
// travels through HDR + tonemap (hdr * exposure -> ACES) and a raw 0..1
// color would crush to black under physical-units exposure. No fog on
// purpose — this view shows paint data, not depth cues.

struct PSIn
{
    float4 clipPos : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // Same layer convention as default_lit: vertex-color RGBA channels
    // are weights, normalized by their sum. Showing the normalized
    // weights (not raw) means this view matches what the lit path
    // actually blends with.
    int layerMode = mat.layerMaterials.w;
    const bool layered = (layerMode > 1);
    float4 layerW = layered
        ? IN.color / max(IN.color.x + IN.color.y + IN.color.z + IN.color.w, 1e-6f)
        : float4(1.0f, 0.0f, 0.0f, 0.0f);

    // Alpha cutout so holes match the lit view (layered-aware: per-layer
    // alpha blended by weight, same convention as default_lit — layer 0
    // is this material, invalid ids reuse it).
    MaterialValues ml0 = mat;
    MaterialValues ml1 = mat;
    MaterialValues ml2 = mat;
    MaterialValues ml3 = mat;
    if (layered)
    {
        int id1 = mat.layerMaterials.x;
        int id2 = mat.layerMaterials.y;
        int id3 = mat.layerMaterials.z;
        if (layerMode > 1 && id1 >= 0 && id1 < MAX_MATERIALS)
            ml1 = pc.bMats.Get().mats[id1];
        if (layerMode > 2 && id2 >= 0 && id2 < MAX_MATERIALS)
            ml2 = pc.bMats.Get().mats[id2];
        if (layerMode > 3 && id3 >= 0 && id3 < MAX_MATERIALS)
            ml3 = pc.bMats.Get().mats[id3];
    }
    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float alphaModeF = mat.params2.w;
    float baseAlpha = 1.0f;
    if (layered)
    {
        baseAlpha = 0.0f;
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            MaterialValues ml = ml0;
            float w = layerW.x;
            if (li == 1) { ml = ml1; w = layerW.y; }
            else if (li == 2) { ml = ml2; w = layerW.z; }
            else if (li == 3) { ml = ml3; w = layerW.w; }
            if (w < 0.004f)
                continue;
            float a = ml.diffuse.a;
            if (ml.tex.x >= 0)
            {
                uint slot = (uint)ml.tex.x;
                Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
                a *= baseTex.Sample(samplers[0], IN.uv).a;
                TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.clipPos.xy);
            }
            baseAlpha += a * w;
        }
    }
    else
    {
        baseAlpha = mat.diffuse.a;
        if (mat.tex.x >= 0)
        {
            uint slot = (uint)mat.tex.x;
            Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
            baseAlpha *= baseTex.Sample(samplers[0], IN.uv).a;
            TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.clipPos.xy);
        }
    }
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, IN.clipPos.xy))
        discard;

    float3 color;
    if (layered)
    {
        // Layer palette mirrors the VULKANTOY_LAYERS_FLAT validation
        // colors (see BuildLayersCubeScene): layer 0 red, 1 green,
        // 2 blue, 3 yellow.
        color = layerW.x * float3(1.00f, 0.15f, 0.15f)
              + layerW.y * float3(0.15f, 1.00f, 0.15f)
              + layerW.z * float3(0.15f, 0.15f, 1.00f)
              + layerW.w * float3(1.00f, 1.00f, 0.15f);
    }
    else
    {
        color = IN.color.rgb;
    }

    FrameUniforms frame = pc.frameUniforms.Get();
    float exp = max(frame.exposure, 1e-6f);
    color /= exp;

    OUT.outColor = float4(SanitizeHdr(color), (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
