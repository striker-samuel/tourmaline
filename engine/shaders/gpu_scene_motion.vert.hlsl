// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU-scene batched motion-vector VS. Indirect firstInstance is a GPU work
// slot (the same work/instance/indirect streams as the lit indexed raster
// VS). Emits the current clip position (SV_Position) and the previous-frame
// clip position (prevClip), plus a per-instance material id for the MASK
// alpha test. The previous transform is validated against the current
// instance generation so a recycled slot never ghosts motion.
#define GFXFILTERED_PASS
#include "common.hlsli"
#include "gpu_scene.hlsli"
#include "double_float.hlsli"
#include "gpu_platform.hlsli"

#define GPU_SCENE_RASTER_MAX_WORK 1048576u
#define GPU_SCENE_RASTER_MAX_INSTANCES 4194304u

struct RasterWorkRow { uint4 identity; uint4 detail; };
struct RasterWorkBuffer { RasterWorkRow rows[GPU_SCENE_RASTER_MAX_WORK]; };
struct RasterInstanceBuffer { GpuSceneInstanceRow rows[GPU_SCENE_RASTER_MAX_INSTANCES]; };
struct RasterViewData {
    float4x4 relativeViewProjection;
    DoubleFloat3 origin;
};
// Deformed heap (5 words/vertex: pos3, normalW, tangentW) — the same local
// mirror as gpu_scene_raster.vert.hlsl so the motion VS stays free of
// gfxfilter.hlsli's includes.
struct RasterSkinDeformedHeap { uint word[64 * 1024 * 1024]; };
struct GpuScenePrevTransform {
    DoubleFloat3 worldPosition;
    float4 basisX;
    float4 basisY;
    float4 basisZ;
    uint4 generation; // x = instance generation at snapshot, yzw pad
};
struct PrevTransformBuffer { GpuScenePrevTransform rows[GPU_SCENE_RASTER_MAX_INSTANCES]; };
struct RasterMotionVsPush {
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    GPU_PTR_READ(RasterWorkBuffer) work;
    GPU_PTR_READ(RasterInstanceBuffer) instances;
    GPU_PTR_READ(RasterViewData) view;
    uint instanceSlots;
    uint workSlots;
    GPU_PTR_READ(PrevTransformBuffer) prevTransforms;
    GPU_PTR_READ(RasterViewData) prevView;
    uint prevValid;
    uint prevSlots;
    // Skinned tail (mirrors the raster VS + RasterMotionVsPush C++ side): the
    // frame's deformed heap + base for the CURRENT pose, and the previous
    // parity half + validity for the skeletal-motion leg.
    GPU_PTR_READ(RasterSkinDeformedHeap) skinDeformed;
    uint skinDeformedBase;
    uint skinValid;
    GPU_PTR_READ(RasterSkinDeformedHeap) skinDeformedPrev;
    uint skinPrevValid;
};
GPU_PUSH(RasterMotionVsPush, pcMV);

struct VSIn {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    uint instanceID : SV_InstanceID;
    uint vertexID : SV_VertexID;
};
struct VSOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 prevClip : PREVCLIP;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] nointerpolation uint materialId : TEXCOORD1;
};

VSOut mainVS(VSIn IN) {
    VSOut OUT = (VSOut)0;
    // Degenerate: all three vertices collapse to a single clipped point,
    // so an invalid work row produces no pixels (harmless empty command).
    OUT.pos = float4(2.0f, 2.0f, 2.0f, 1.0f);
    OUT.prevClip = float4(2.0f, 2.0f, 2.0f, 1.0f);
    OUT.uv = IN.uv;
    OUT.materialId = 0xffffffffu;

    const uint workSlot = IN.instanceID;
    if (workSlot >= pcMV.workSlots || workSlot >= GPU_SCENE_RASTER_MAX_WORK)
        return OUT;
    const RasterWorkRow work = pcMV.work.Get().rows[workSlot];
    const uint slot = work.identity.x;
    const uint generation = work.identity.y;
    if (slot >= pcMV.instanceSlots || slot >= GPU_SCENE_RASTER_MAX_INSTANCES ||
        generation == 0u)
        return OUT;
    const GpuSceneInstanceRow row = pcMV.instances.Get().rows[slot];
    if (row.identity.y != 1u || row.identity.x != generation)
        return OUT;

    const GpuSceneInstance inst = row.value;
    const float3 a = inst.basisX.xyz;
    const float3 b = inst.basisY.xyz;
    const float3 c = inst.basisZ.xyz;
    // Skinned fetch (mirrors gpu_scene_raster.vert.hlsl): swap the bind-pose
    // position for the frame's DEFORMED words. A null BDA / unknown base means
    // the frame ran no skinning pass and the bind pose is the honest answer.
    const bool skinned = (inst.refs.w & GPU_SCENE_INSTANCE_SKINNED) != 0u;
    float3 position = IN.position;
    if (skinned && pcMV.skinValid != 0u) {
        const uint d = (IN.vertexID - pcMV.skinDeformedBase) * 5u;
        position = float3(
            asfloat(pcMV.skinDeformed.Get().word[d + 0u]),
            asfloat(pcMV.skinDeformed.Get().word[d + 1u]),
            asfloat(pcMV.skinDeformed.Get().word[d + 2u]));
    }
    // Previous deformed position (skeletal motion): the previous parity half
    // of the SAME base when valid, else fall back to `position` (current pose,
    // or bind pose when the frame skinned nothing) — rigid + camera motion
    // preserved, unavailable skeletal motion suppressed.
    float3 prevPosition = position;
    if (skinned && pcMV.skinPrevValid != 0u) {
        const uint pd = (IN.vertexID - pcMV.skinDeformedBase) * 5u;
        prevPosition = float3(
            asfloat(pcMV.skinDeformedPrev.Get().word[pd + 0u]),
            asfloat(pcMV.skinDeformedPrev.Get().word[pd + 1u]),
            asfloat(pcMV.skinDeformedPrev.Get().word[pd + 2u]));
    }
    const float3 relative = DoubleFloatRelative(
        inst.worldPosition, pcMV.view.Get().origin) +
        a * position.x + b * position.y + c * position.z;

    OUT.pos = mul(pcMV.view.Get().relativeViewProjection, float4(relative, 1.0f));
    // Default zero motion (first frame / no history): prevClip == pos.
    OUT.prevClip = OUT.pos;
    OUT.materialId = inst.refs.z;

    // Previous transform, generation-validated. A slot whose generation
    // changed since the snapshot (retired/recycled) keeps zero motion
    // rather than ghosting the previous owner's transform. slot is also
    // bounded by prevSlots (the snapshot's instance count): streaming can
    // grow the current instance set beyond it, and reading a newly-streamed
    // slot past the prev buffer's end would fault the GPU.
    if (pcMV.prevValid != 0u && slot < pcMV.prevSlots) {
        const GpuScenePrevTransform prev = pcMV.prevTransforms.Get().rows[slot];
        if (prev.generation.x == generation) {
            const float3 pa = prev.basisX.xyz;
            const float3 pb = prev.basisY.xyz;
            const float3 pc_ = prev.basisZ.xyz;
            const float3 prevRelative = DoubleFloatRelative(
                prev.worldPosition, pcMV.prevView.Get().origin) +
                pa * prevPosition.x + pb * prevPosition.y + pc_ * prevPosition.z;
            OUT.prevClip = mul(
                pcMV.prevView.Get().relativeViewProjection, float4(prevRelative, 1.0f));
        }
    }
    return OUT;
}
