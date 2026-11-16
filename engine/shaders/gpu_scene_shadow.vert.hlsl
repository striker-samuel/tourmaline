// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Indexed-indirect shadow caster VS. firstInstance indexes a GPUScene work
// row; RenderView is selected by the separate per-cascade RasterViewData.
// Never reuse main-camera visibility for shadow casters.
#define GFXFILTERED_PASS
#define TM_SHADOW_CASTER_PASS 1
#include "common.hlsli"
#include "gpu_scene.hlsli"
#include "gpu_platform.hlsli"

#define GPU_SCENE_RASTER_MAX_WORK 1048576u
#define GPU_SCENE_RASTER_MAX_INSTANCES 4194304u
struct RasterWorkRow { uint4 identity; uint4 detail; };
struct RasterWorkBuffer { RasterWorkRow rows[GPU_SCENE_RASTER_MAX_WORK]; };
struct RasterInstanceBuffer { GpuSceneInstanceRow rows[GPU_SCENE_RASTER_MAX_INSTANCES]; };
struct RasterViewData { float4x4 relativeViewProjection; DoubleFloat3 origin; };
struct RasterVsPush {
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    GPU_PTR_READ(RasterWorkBuffer) work;
    GPU_PTR_READ(RasterInstanceBuffer) instances;
    GPU_PTR_READ(RasterViewData) view;
    uint instanceSlots;
    uint workSlots;
    uint64_t _unusedSkin;
    uint _unusedBase;
    uint _unusedValid;
};
GPU_PUSH(RasterVsPush, pcS);

struct VSIn {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    uint instanceID : SV_InstanceID;
};
struct VSOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
};
VSOut mainVS(VSIn IN) {
    VSOut OUT = (VSOut)0;
    OUT.pos = float4(2.0f, 2.0f, 2.0f, 1.0f);
    OUT.meshMaterial = 0xffffffffu;
    const uint workSlot = IN.instanceID;
    if (workSlot >= pcS.workSlots || workSlot >= GPU_SCENE_RASTER_MAX_WORK)
        return OUT;
    const RasterWorkRow work = pcS.work.Get().rows[workSlot];
    const uint slot = work.identity.x;
    const uint generation = work.identity.y;
    if (slot >= pcS.instanceSlots || slot >= GPU_SCENE_RASTER_MAX_INSTANCES ||
        generation == 0u)
        return OUT;
    const GpuSceneInstanceRow row = pcS.instances.Get().rows[slot];
    if (row.identity.y != 1u || row.identity.x != generation)
        return OUT;
    const GpuSceneInstance inst = row.value;
    // GPUScene's cull shader already excludes non-casters. Repeat the check
    // in the VS to protect against stale or corrupted work rows.
    if ((inst.refs.w & (1u << 5)) == 0u)
        return OUT;
    const float3 relative =
        DoubleFloatRelative(inst.worldPosition, pcS.view.Get().origin) +
        inst.basisX.xyz * IN.position.x +
        inst.basisY.xyz * IN.position.y +
        inst.basisZ.xyz * IN.position.z;
    float4 clip = mul(pcS.view.Get().relativeViewProjection, float4(relative, 1.0f));
    clip.z = max(clip.z, 0.0f); // Same near-plane convention as shadow.vert.
    OUT.pos = clip;
    const DoubleFloat3 origin = pcS.view.Get().origin;
    OUT.worldPos = relative + origin.high.xyz + origin.low.xyz;
    OUT.color = IN.color;
    OUT.uv = IN.uv;
    OUT.ditherCloseup =
        (inst.refs.w & GPU_SCENE_INSTANCE_DITHER_CLOSEUP) != 0u ? 1u : 0u;
    OUT.meshMaterial = inst.refs.z;
    return OUT;
}
