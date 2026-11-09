// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    // Dither-on-closeup inputs (Scene::ditherCloseup; the flat flag from
    // shadow.vert's pc word / the gfx seed twins' Draw-ring bit, + the
    // world position for the per-pixel camera-distance factor — the MAIN
    // camera is the fade's anchor even in light-view passes). Declared by
    // the FS; the VS twins (shadow.vert / gfxseedmasked / gfxshadowseed-
    // masked) deliver both.
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    // EXPLICIT location 8 (all ditherCloseup declarations pin it now -
    // see default_lit_body.hlsli's note; the sequential assignment made
    // this one land on 5/6 depending on the stripped members).
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
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

    // Dither-on-closeup (Scene::ditherCloseup; see common.hlsli Closeup-
    // DitherKeep): the depth screen-doors in step with the lit pass's
    // fade, so the holes keep the REAL background depth (solid depth
    // would leave black holes through the fading subtree) and the
    // subtree's shadow fades with it. The eye is the MAIN camera's (the
    // fade follows what the camera sees, not the light), reached through
    // the FrameUniforms this FS already reads below. First: discarded
    // fragments skip the material fetch entirely.
    if (IN.ditherCloseup != 0u)
    {
        FrameUniforms frameD = pc.frameUniforms.Get();
        if (!CloseupDitherKeep(IN.worldPos, frameD.cameraPos, clipPos.xy))
            discard;
    }
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
