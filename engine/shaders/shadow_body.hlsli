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

ShadowPSOut mainPS(float4 clipPos : SV_Position, ShadowPSIn IN)
{
    ShadowPSOut OUT;
    // Window-depth guard: SV_Position.z can leave [0,1] (or go NaN) when
    // the VS z-squash clamp (shadow.vert: clip.z = max(z, 0) so casters
    // behind the light's near plane rasterize) meets w-crossing triangles —
    // z/w interpolates outside the range or hits 0/0. The conservative
    // SV_Depth write bypasses hardware depth clamping, so a raw garbage
    // value lands in the depth buffer and every depth consumer downstream
    // (SSR trace, volumetric march) samples it. Clamp into range,
    // NaN-safe: the fallback sits on the FAR side of the convention so
    // the conservative bound stays satisfied (never nearer in reverse-Z,
    // never farther in normal-Z).
    float d = clipPos.z;
#if defined(SHADOW_NORMAL_Z)
    OUT.outDepth = (d >= 0.0f && d <= 1.0f) ? d : 1.0f;
#else
    OUT.outDepth = (d >= 0.0f && d <= 1.0f) ? d : 0.0f;
#endif

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    float alphaCutoff = mat.params.x;
    float baseAlpha = mat.diffuse.a;

    int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        float4 s = baseTex.Sample(samplers[0], IN.uv);
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, clipPos.xy, pc.textureFeedbackEnabled != 0u);
        baseAlpha *= s.a;
    }

    // Dithered Blend (alphaMode == 2, dithered frame): screen-door discard
    // so the depth prepass stores dithered depth for transparent materials.
    // Masked and blended-Blend keep the classic alpha-cutout test.
    float alphaModeF = mat.params2.w;
    if (alphaModeF == 2.0f && pc.pad_pc == 0)
    {
        if (!DitherKeep(baseAlpha, clipPos.xy))
            discard;
    }
    else if (baseAlpha < alphaCutoff)
        discard;

    return OUT;
}
