// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Project-facing SPACE TRANSFORMS, intentionally independent of the
// renderer's push constants, draw ring, descriptors, and shader stage.
//
// Tourmaline uses mul(matrix, float4(position, 1)) throughout its main,
// GPU-driven indexed, mesh and shadow shaders. Do NOT assume a global
// UNITY_MATRIX_MVP-style matrix: main-view, shadow-cascade and spot tile
// passes can use different RenderView matrices. Pass the matrix explicitly.
// The functions below also work in project-authored vertex shaders.

float3 TM_ObjectToWorld(float3 positionOS, float4x4 objectToWorld)
{
    return mul(objectToWorld, float4(positionOS, 1.0f)).xyz;
}
float3 TM_WorldToView(float3 positionWS, float4x4 worldToView)
{
    return mul(worldToView, float4(positionWS, 1.0f)).xyz;
}
float4 TM_WorldToClip(float3 positionWS, float4x4 worldToClip)
{
    return mul(worldToClip, float4(positionWS, 1.0f));
}
float4 TM_ObjectToClip(float3 positionOS, float4x4 objectToWorld,
    float4x4 worldToClip)
{
    // Keep two matrix-vector products instead of building an MVP matrix
    // in the vertex stage; this matches the live indexed/mesh shaders.
    return mul(worldToClip, mul(objectToWorld, float4(positionOS, 1.0f)));
}
float3 TM_ClipToNdc(float4 clip)
{
    // Caller must supply clip.w != 0; clip space is not yet normalized.
    return clip.xyz / clip.w;
}

// Shadow lookup PROJECTION only; NOT shadow attenuation/sampling. The
// engine's cascaded directional/spot paths additionally use tile atlas
// scale/bias, normal/slope bias, EVSM/PCF, and contact shadows.
float4 TM_WorldToShadowClip(float3 positionWS, float4x4 shadowViewProj)
{
    return mul(shadowViewProj, float4(positionWS, 1.0f));
}
float3 TM_ShadowClipToCoord(float4 shadowClip)
{
    // Same xy mapping as shadows.hlsli::SampleCascadeTile:
    // uv = (clip.xy/clip.w)*0.5 + 0.5, depth = clip.z/clip.w.
    // Returns a sentinel outside [0,1] when behind the light/invalid.
    if (!(shadowClip.w > 0.0f))
        return float3(-1.0f, -1.0f, -1.0f);
    const float3 ndc = shadowClip.xyz / shadowClip.w;
    return float3(ndc.xy * 0.5f + 0.5f, ndc.z);
}
float3 TM_WorldToShadowCoord(float3 positionWS, float4x4 shadowViewProj)
{
    return TM_ShadowClipToCoord(
        TM_WorldToShadowClip(positionWS, shadowViewProj));
}
bool TM_ShadowCoordInBounds(float3 uvDepth)
{
    // A shadow atlas transform is still required before texture fetch.
    // Depth convention here is the normal-Z light atlas (not reverse-Z
    // main depth). Match SampleCascadeTile's strict interior depth test.
    return all(uvDepth.xy >= 0.0f) && all(uvDepth.xy <= 1.0f)
        && uvDepth.z > 0.0f && uvDepth.z < 1.0f;
}

// Function-like convenience macros evaluate each argument ONCE, via a
// typed function. No implicit pc.model, frame.viewProj, or bindless reads.
#define TM_OBJECT_TO_WORLD(positionOS, objectToWorld) TM_ObjectToWorld((positionOS), (objectToWorld))
#define TM_WORLD_TO_VIEW(positionWS, worldToView) TM_WorldToView((positionWS), (worldToView))
#define TM_WORLD_TO_CLIP(positionWS, worldToClip) TM_WorldToClip((positionWS), (worldToClip))
#define TM_OBJECT_TO_CLIP(positionOS, objectToWorld, worldToClip) TM_ObjectToClip((positionOS), (objectToWorld), (worldToClip))
#define TM_CLIP_TO_NDC(clipPosition) TM_ClipToNdc((clipPosition))

#define TM_WORLD_TO_SHADOW_CLIP(positionWS, shadowVP) TM_WorldToShadowClip((positionWS), (shadowVP))
#define TM_SHADOW_CLIP_TO_COORD(shadowClip) TM_ShadowClipToCoord((shadowClip))
#define TM_WORLD_TO_SHADOW_COORD(positionWS, shadowVP) TM_WorldToShadowCoord((positionWS), (shadowVP))
#define TM_SHADOW_COORD_IN_BOUNDS(uvDepth) TM_ShadowCoordInBounds((uvDepth))
