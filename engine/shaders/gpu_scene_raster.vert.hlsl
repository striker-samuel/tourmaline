// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU-scene indexed raster VS. Indirect firstInstance is a GPU work slot,
// NOT a World::drawItems index. Validate the persistent generation and fetch
// the same material varying ABI as gfxmesh_lit.frag.hlsl (locations 0..10).
//
// Static opaque geometry only: skinning, authored lightmap streams,
// material instance userdata and motion are still fallback-only.
#define GFXFILTERED_PASS
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "gpu_scene.hlsli"
#include "gpu_platform.hlsli"

#define GPU_SCENE_RASTER_MAX_WORK 1048576u
#define GPU_SCENE_RASTER_MAX_INSTANCES 4194304u

// Deformed heap (5 words/vertex: pos3, normalW, tangentW) — a local mirror of
// GfxSkinDeformedHeap so this VS stays free of gfxfilter.hlsli's includes.
struct RasterSkinDeformedHeap { uint word[64 * 1024 * 1024]; };

struct RasterWorkRow { uint4 identity; uint4 detail; };
struct RasterWorkBuffer { RasterWorkRow rows[GPU_SCENE_RASTER_MAX_WORK]; };
struct RasterInstanceBuffer { GpuSceneInstanceRow rows[GPU_SCENE_RASTER_MAX_INSTANCES]; };
struct RasterViewData {
    float4x4 relativeViewProjection;
    DoubleFloat3 origin;
};
struct RasterVsPush {
    // Match the first 192B of the scene push, just like gfxfiltered.vert.
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    GPU_PTR_READ(RasterWorkBuffer) work;
    GPU_PTR_READ(RasterInstanceBuffer) instances;
    GPU_PTR_READ(RasterViewData) view;
    uint instanceSlots;
    uint workSlots;
    // Skinned tail (mirrors C++ RasterVsPush): the frame's deformed heap +
    // the skinned span's global base. skinValid = 1 only when the frame ran
    // the skinning pass.
    GPU_PTR_READ(RasterSkinDeformedHeap) skinDeformed;
    uint skinDeformedBase;
    uint skinValid;
};
GPU_PUSH(RasterVsPush, pcRV);

struct VSIn {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    uint instanceID : SV_InstanceID;
    uint vertexID : SV_VertexID;
};
struct VSOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    [[vk::location(9)]] nointerpolation uint userData : TEXCOORD8;
    [[vk::location(10)]] nointerpolation uint lightLayerMask : TEXCOORD9;
    [[vk::location(11)]] float3 cameraRelativePos : TEXCOORD10;
};
VSOut mainVS(VSIn IN) {
    VSOut OUT = (VSOut)0;
    OUT.pos = float4(2.0f, 2.0f, 2.0f, 1.0f);
    OUT.lmChunk = 0xffffffffu;
    OUT.meshMaterial = 0xffffffffu;
    OUT.lightLayerMask = 0xffffffffu;

    const uint workSlot = IN.instanceID;
    if (workSlot >= pcRV.workSlots || workSlot >= GPU_SCENE_RASTER_MAX_WORK)
        return OUT;
    const RasterWorkRow work = pcRV.work.Get().rows[workSlot];
    const uint slot = work.identity.x;
    const uint generation = work.identity.y;
    if (slot >= pcRV.instanceSlots || slot >= GPU_SCENE_RASTER_MAX_INSTANCES ||
        generation == 0u)
        return OUT;
    const GpuSceneInstanceRow row = pcRV.instances.Get().rows[slot];
    if (row.identity.y != 1u || row.identity.x != generation)
        return OUT;

    const GpuSceneInstance inst = row.value;
    const float3 a = inst.basisX.xyz;
    const float3 b = inst.basisY.xyz;
    const float3 c = inst.basisZ.xyz;
    // Skinned fetch: swap the bind-pose position/normal/tangent for the
    // frame's DEFORMED words (5/vertex at (vertexID - base) * 5). The
    // instance flag proves the mesh is skinned; a null BDA proves the
    // frame ran no skinning pass and the bind pose is the honest answer.
    const bool skinned = (inst.refs.w & GPU_SCENE_INSTANCE_SKINNED) != 0u;
    float3 position = IN.position;
    uint normalPacked = IN.normalPacked;
    uint tangentPacked = IN.tangentPacked;
    if (skinned && pcRV.skinValid != 0u) {
        const uint d = (IN.vertexID - pcRV.skinDeformedBase) * 5u;
        position = float3(
            asfloat(pcRV.skinDeformed.Get().word[d + 0u]),
            asfloat(pcRV.skinDeformed.Get().word[d + 1u]),
            asfloat(pcRV.skinDeformed.Get().word[d + 2u]));
        normalPacked = pcRV.skinDeformed.Get().word[d + 3u];
        tangentPacked = pcRV.skinDeformed.Get().word[d + 4u];
    }
    const float3 relative = DoubleFloatRelative(
        inst.worldPosition, pcRV.view.Get().origin) +
        a * position.x + b * position.y + c * position.z;

    OUT.pos = mul(pcRV.view.Get().relativeViewProjection, float4(relative, 1.0f));
    const DoubleFloat3 origin = pcRV.view.Get().origin;
    OUT.worldPos = relative + origin.high.xyz + origin.low.xyz;
    OUT.cameraRelativePos = relative;
    OUT.color = IN.color;
    OUT.uv = IN.uv;
    OUT.meshMaterial = inst.refs.z;
    OUT.lightmapUv = float2(0.0f, 0.0f);
    OUT.lmChunk = 0xffffffffu;
    OUT.ditherCloseup = (inst.refs.w & GPU_SCENE_INSTANCE_DITHER_CLOSEUP) != 0u ? 1u : 0u;
    OUT.userData = inst.userData;
    OUT.lightLayerMask = 0xffffffffu;

    const float3 normal = OctDecodeNormal(normalPacked);
    const float4 tangent = OctDecodeTangent(tangentPacked);
    const float3 coX = cross(b, c);
    const float3 coY = cross(c, a);
    const float3 coZ = cross(a, b);
    const float det = dot(a, coX);
    float3 n = abs(det) > 1.0e-12f
        ? (coX * normal.x + coY * normal.y + coZ * normal.z) / det
        : a * normal.x + b * normal.y + c * normal.z;
    OUT.normal = normalize(n);
    OUT.tangent = float4(
        normalize(a * tangent.x + b * tangent.y + c * tangent.z),
        tangent.w * (det < 0.0f ? -1.0f : 1.0f));
    return OUT;
}
