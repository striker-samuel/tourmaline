#include "common.hlsli"

// Motion vector FS: per-pixel UV delta (prevUV - currUV) into an RG16F
// target. MASK (alpha-tested) cutouts reuse the shadow.frag alpha test so
// foliage gets leaf-shaped motion; opaque materials skip the texture fetch
// (shadingModel check mirrors default_lit.frag: ShadingModel::Masked == 2).

struct PSIn
{
    float4 pos : SV_Position;
    float4 prevClip : PREVCLIP;
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float2 motion : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];
    if (mat.params.w == 2.0f) // ShadingModel::Masked
    {
        float alphaCutoff = mat.params.x;
        float baseAlpha = mat.diffuse.a;
        int baseColorIdx = mat.tex.x;
        if (baseColorIdx >= 0)
        {
            uint slot = (uint)baseColorIdx;
            Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
            float4 s = baseTex.Sample(samplers[0], IN.uv);
            TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.pos.xy);
            baseAlpha *= s.a;
        }
        if (baseAlpha < alphaCutoff)
            discard;
    }
    // Dithered Blend: same screen-door pattern as the main view so motion
    // vectors exist exactly for the pixels the color pass kept.
    if (mat.params2.w == 2.0f && pc.pad_pc == 0)
    {
        float baseAlpha = mat.diffuse.a;
        int baseColorIdx = mat.tex.x;
        if (baseColorIdx >= 0)
        {
            uint slot = (uint)baseColorIdx;
            Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
            float4 s = baseTex.Sample(samplers[0], IN.uv);
            TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.pos.xy);
            baseAlpha *= s.a;
        }
        if (!DitherKeep(baseAlpha, IN.pos.xy))
            discard;
    }

    FrameUniforms frame = pc.frameUniforms.Get();

    // SV_Position is post-divide pixel coords; same NDC->UV convention as
    // the Y-flipped proj both frames share.
    float2 currUV = IN.pos.xy * frame.invViewportSize;
    float2 prevUV = (IN.prevClip.xy / IN.prevClip.w) * 0.5 + 0.5;
    float2 motion = prevUV - currUV;

    // Guard: geometry behind the previous camera, first frame or camera
    // reset produces huge reprojected deltas – treat them as "no history".
    if (dot(motion, motion) > 1.0)
        motion = float2(0.0, 0.0);

    OUT.motion = motion;
    return OUT;
}
