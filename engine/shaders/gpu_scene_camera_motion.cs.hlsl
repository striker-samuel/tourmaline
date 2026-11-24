// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define CAMERA_MOTION_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "gpu_platform.hlsli"

// Camera-only motion reconstruction (the Phase 3 static-geometry leg):
// ONE thread per motion texel reconstructs the receiver's camera-relative
// position from the depth prepass, re-expresses it in the previous frame's
// camera-relative space, reprojects through the previous view-projection and
// writes the prevUV - currUV delta into the RG16F motion target. Static
// geometry no longer needs a redundant raster pass — the motion for a static
// surface is exactly the camera motion, recovered here.
//
// Conventions (the engine standards, see ssgi_diffuse_trace.cs.hlsl's
// coordinate audit): framebuffer y-down NDC (top = -1), uv = ndc*0.5+0.5;
// reversed-Z (1 = near, 0 = far/sky); background rejection via
// DofIsBackgroundOrGarbage.
//
// Bindless storage write (binding 2): the element type MUST match the
// motion target's format — RG16_Float -> float2.

[[vk::binding(2, 0)]]
RWTexture2D<float2> motionStorage[];

// The camera-relative frames are REBASED view-projection matrices (the same
// relativeViewProjection the raster legs use): relative = viewProj *
// translate(cameraPos), so they map a camera-relative position straight to
// clip. The inverse (translate(-cameraPos) * invViewProj) reconstructs the
// camera-relative position from a y-down NDC + depth WITHOUT round-tripping
// an absolute world position through float32 (large-world safe).
struct CameraMotionPush
{
    float4x4 invRelativeViewProj;   // current inverse rebased view-proj
    float4x4 prevRelativeViewProj;  // previous rebased view-proj
    float4 originDelta;             // xyz = curCameraPos - prevCameraPos (double split)
    uint depthTextureIndex;         // full-res opaque depth (the prepass target)
    uint outImageIndex;             // bindless storage index of the motion target
    uint outWidth;                  // motion extent (the dispatch domain)
    uint outHeight;
    uint samplerIndex;              // point/clamp (hard depth taps)
    uint pad0;
};

GPU_PUSH(CameraMotionPush, sp);

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= sp.outWidth || tid.y >= sp.outHeight)
        return;

    const float2 invOut = float2(1.0f / float(sp.outWidth), 1.0f / float(sp.outHeight));
    const float2 uv = (float2(tid.xy) + 0.5f) * invOut;

    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    SamplerState hardSmp = samplers[NonUniformResourceIndex(sp.samplerIndex)];
    const float depth = depthTex.SampleLevel(hardSmp, uv, 0).r;

    // Sky/garbage: no receiver, no motion (0 = stationary, the temporal
    // consumer's no-history convention for background).
    float2 motion = float2(0.0f, 0.0f);
    if (!DofIsBackgroundOrGarbage(depth))
    {
        const float2 ndc = uv * 2.0f - 1.0f;
        // Camera-relative position in the CURRENT frame's rebased space.
        const float4 h = mul(sp.invRelativeViewProj, float4(ndc, depth, 1.0f));
        const float3 curRelative = h.xyz / max(h.w, 1e-9f);
        // Re-express in the previous frame's rebased space, then reproject.
        const float3 prevRelative = curRelative + sp.originDelta.xyz;
        const float4 prevClip = mul(sp.prevRelativeViewProj, float4(prevRelative, 1.0f));
        if (isfinite(prevClip.w) && prevClip.w > 0.0f)
        {
            const float2 prevUV = (prevClip.xy / prevClip.w) * 0.5f + 0.5f;
            const float2 m = prevUV - uv;
            if (isfinite(m.x) && isfinite(m.y))
                motion = m;
        }
    }
    motionStorage[sp.outImageIndex][tid.xy] = motion;
}
