// Alpha-tested shadow/depth FS body for cutout (MASK) materials, shared by
// shadow.frag.hlsl and shadow_rev.frag.hlsl.
//
// The shader only discards fragments whose baseColor alpha is under the
// material cutoff, so foliage casts leaf-shaped shadows / leaves
// leaf-shaped holes in depth. Lighting, normal mapping etc. are
// intentionally skipped.
//
// Conservative depth: the entry file picks the mode matching its pipeline
// depth convention, which lets the driver run the depth test BEFORE this
// shader even though it may discard. Occluded masked fragments (e.g. deep
// foliage layers after the temporal seed primed depth) then die without the
// baseColor fetch. The written value is the passthrough interpolated depth,
// so the bound is trivially satisfied; depth writes still happen only for
// surviving (non-discarded) fragments — this is NOT early_fragment_tests,
// which would write depth for discarded hole texels and punch black holes.
//
//   SHADOW_NORMAL_Z defined : normal-Z passes (shadow cascades / spot
//                             atlases, clear 1.0 + LessEqual) ->
//                             SV_DepthGreaterEqual (never farther).
//   otherwise               : reversed-Z passes (Forward+ prepass, temporal
//                             seed, remainder; clear 0 + GreaterEqual) ->
//                             SV_DepthLessEqual (never nearer, raw float).

struct ShadowPSIn
{
    float4 clipPos : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

#if defined(SHADOW_NORMAL_Z)
struct ShadowPSOut
{
    float outDepth : SV_DepthGreaterEqual;
};
#else
struct ShadowPSOut
{
    float outDepth : SV_DepthLessEqual;
};
#endif

ShadowPSOut mainPS(ShadowPSIn IN)
{
    ShadowPSOut OUT;
    OUT.outDepth = IN.clipPos.z;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    float alphaCutoff = mat.params.x;
    float baseAlpha = mat.diffuse.a;

    int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        float4 s = baseTex.Sample(samplers[0], IN.uv);
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.clipPos.xy);
        baseAlpha *= s.a;
    }

    // Dithered Blend (alphaMode == 2, dithered frame): screen-door discard
    // so the depth prepass stores dithered depth for transparent materials.
    // Masked and blended-Blend keep the classic alpha-cutout test.
    float alphaModeF = mat.params2.w;
    if (alphaModeF == 2.0f && pc.pad_pc == 0)
    {
        if (!DitherKeep(baseAlpha, IN.clipPos.xy))
            discard;
    }
    else if (baseAlpha < alphaCutoff)
        discard;

    return OUT;
}
