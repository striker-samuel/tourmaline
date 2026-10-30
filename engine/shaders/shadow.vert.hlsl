// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SHADOW_DIRECT_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"
#include "gpu_platform.hlsli"

// Direct shadow VS: mirrors PushConstants bytes 0..192 field-for-field,
// then overlays the unused prevModel scratch (192..240) with the direct
// skin tail (208..240) - the same layout default.vert uses. The fragment
// stage keeps the scene pc (shadow_body reads frameUniforms/bMats/
// materialId/pad_pc, never prevModel), so the overlay is safe.
struct ShadowVsPush
{
    float4x4 model;
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    GPU_PTR_READ(LightBuffer) bLights;
    GPU_PTR_READ(MaterialsBuffer) bMats;
    GPU_PTR_READ(DirectionalCascadeBuffer) cascadeBuffer;
    uint materialId;
    uint comparisonSamplerId;
    uint numCascades;
    uint cascadeIndex;
    GPU_PTR_READ(ForwardPlusLightBuffer) fpLights;
    GPU_PTR_READ(ForwardPlusTileCountBuffer) fpTileCounts;
    GPU_PTR_READ(ForwardPlusTileIndexBuffer) fpTileIndices;
    uint fpTileCountX;
    uint fpLightCount;
    uint fpTileSize;
    uint fpMaxPerTile;
    GPU_PTR_READ(SpotShadowBuffer) spotShadowBuffer;
    uint spotShadowMapIndex;
    uint spotShadowCount;
    uint spotIndex;
    uint pad_pc;
    GPU_PTR_READ(RenderViewBuffer) renderViews;
    uint mainRenderView;
    uint renderViewIndex;
    // 192..208: unused prevModel scratch (shadow.vert never reads it).
    float4 _padChart;
    // 208..240: the direct-path skin tail. A skinned draw fetches its
    // position from the frame's deformed heap instead of the bind-pose
    // vertex input (the Northlight one-fetch swap - the shadow leg needs
    // position only, never normals/tangents).
    uint skinned;      // 208: 1 = fetch from the deformed heap
    uint deformedBase; // 212: the skinned span's heap-id base
    GPU_PTR_READ(GfxSkinDeformedHeap) vsDeformed; // 216
    uint4 _padSkin;    // 224..240: layout filler
    // 240..256: the per-draw dither-on-closeup flag lives in .x; y..w
    // stay scratch.
    uint4 padEnd;
};

GPU_PUSH(ShadowVsPush, pcS);

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    uint vertexID : SV_VertexID;
};

struct VSOut
{
    float4 pos : SV_Position;

    // Consumed only by shadow.frag (alpha-tested cutout depth). The
    // depth-only program has no FS; extra varyings are legal there
    // (same pattern as default.vert with the Forward+ depth program).
    // COLOR0 stays a full float4: the shadow_body FS declares a vec4 at
    // Location 0, and a vec3 here would leave component 3 undeclared
    // (VUID-RuntimeSpirv-OpEntryPoint-08743). The FS never reads it, but
    // the declared interface must agree.
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    // Dither-on-closeup inputs for shadow_body's screen-door (Scene::
    // ditherCloseup; see common.hlsli CloseupDitherKeep): the world
    // position for the per-pixel camera-distance factor (the MAIN camera
    // is the fade's anchor even in light-view passes) + the per-draw
    // flag from the pc word (PushConstants::padEnd_.x — the direct
    // shadow/prepass legs push it per draw; the filtered seed twins
    // deliver the Draw ring's flag at the same locations).
    [[vk::location(2)]] float3 worldPos : TEXCOORD2;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;

    // Optional skin fetch: a skinned draw swaps the bind-pose position for
    // the frame's deformed word (5 words/vertex at
    // (vertexID - deformedBase) * 5; the tail is zeroed on static draws).
    float3 position = IN.position;
    if (pcS.skinned != 0u)
    {
        const uint d = (IN.vertexID - pcS.deformedBase) * 5;
        position = float3(
            asfloat(pcS.vsDeformed.Get().word[d + 0]),
            asfloat(pcS.vsDeformed.Get().word[d + 1]),
            asfloat(pcS.vsDeformed.Get().word[d + 2]));
    }

    float4 world = mul(pcS.model, float4(position, 1.0f));

    OUT.color = IN.color;
    OUT.uv = IN.uv;
    OUT.worldPos = world.xyz;
    // Byte 240 of the scene pc (padEnd_.x — the per-draw overlay scratch's
    // last word): the direct legs' per-draw dither-on-closeup flag.
    OUT.ditherCloseup = pcS.padEnd.x;

    // RenderView path: every shadow pass owns an allocated view (one per
    // directional cascade / spot tile); its VP is the single source of
    // truth. The legacy descriptor paths below stay as fallbacks for
    // pipelines driven without the RenderView system.
    float4x4 lightVP;
    if (pcS.renderViewIndex < MAX_RENDER_VIEWS)
    {
        lightVP = pcS.renderViews.Get().views[pcS.renderViewIndex].m_viewProjectionMatrix;
    }
    else if (pcS.spotIndex != 0xFFFFFFFFu && pcS.spotShadowCount > 0u && pcS.spotIndex < pcS.spotShadowCount)
    {
        lightVP = pcS.spotShadowBuffer.Get().tiles[pcS.spotIndex].lightVP;
    }
    else if (pcS.numCascades > 0 && pcS.cascadeIndex < (uint)pcS.numCascades)
    {
        GpuDirectionalLightDescription dir = pcS.cascadeBuffer.Get().dir;
        lightVP = dir.cascades[pcS.cascadeIndex].lightVP;
    }
    else
    {
        Light light = pcS.bLights.Get().lights[0];
        lightVP = light.lightVP;
    }

    float4 clip = mul(lightVP, world);
    clip.z = max(clip.z, 0.0f);
    OUT.pos = clip;
    return OUT;
}
