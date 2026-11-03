// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0
// (04_project_game sample content — see LICENSE.md)

// Compile-only project shader demonstrating the opt-in helper library.
// No renderer program references this module; GameProjectLit_shaders
// builds it as a regression probe for the vertex/shadow helper ABI.
#define TM_SHADOW_CASTER_PASS 1
#include "tm_project_shader_macros.hlsli"
#if !TM_PASS_SHADOW_CASTER
#error "Shadow-caster probe did not publish the compile-time pass flag"
#endif

struct ProjectSpaceProbeOut
{
    float4 position : SV_Position;
    float3 shadowUvDepth : TEXCOORD0;
};

ProjectSpaceProbeOut mainVS(uint vertexId : SV_VertexID)
{
    // Identity matrices keep the example fully portable; a game passes
    // its per-draw model and selected RenderView VP here instead.
    const float4x4 model = float4x4(
        1, 0, 0, 0,  0, 1, 0, 0,
        0, 0, 1, 0,  0, 0, 0, 1);
    const float4x4 vp = model;
    const float3 positionOS = float3(
        float(vertexId & 1u), float((vertexId >> 1u) & 1u), 0.5f);

    ProjectSpaceProbeOut result;
    const float3 world = TM_OBJECT_TO_WORLD(positionOS, model);
    const float3 viewPos = TM_WORLD_TO_VIEW(world, vp);
    const float4 clip = TM_OBJECT_TO_CLIP(positionOS, model, vp);
    const float3 ndc = TM_CLIP_TO_NDC(clip);
    const float4 worldClip = TM_WORLD_TO_CLIP(world, vp);
    const float4 lightClip = TM_WORLD_TO_SHADOW_CLIP(world, vp);
    const float3 shadowUVZ = TM_SHADOW_CLIP_TO_COORD(lightClip);
    const float3 shadowUVZ2 = TM_WORLD_TO_SHADOW_COORD(world, vp);
    // Keep all helpers live during compilation and test function types.
    result.position = worldClip + (clip - worldClip) +
        float4((viewPos - world) + (ndc - clip.xyz / clip.w), 0);
    result.shadowUvDepth = TM_SHADOW_COORD_IN_BOUNDS(shadowUVZ)
        ? shadowUVZ2 : float3(-1.0f, -1.0f, -1.0f);
    return result;
}
