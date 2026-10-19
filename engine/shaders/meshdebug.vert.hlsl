// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Meshlet debug view: flat per-cluster color (see GfxRenderer debug UI).
// Standalone push (shares the global [0,256) range): model + viewProj +
// color. C++ mirror: engine::MeshDebugPush (144 bytes) + the 16B skin
// tail at byte 192 (engine::MeshDebugSkinTail): SKINNED meshes show the
// ANIMATED pose here — the vertex fetch swaps to the frame's deformed
// heap (the same Northlight swap the filtered/seed legs run), so the
// Meshlets/Lods debug views judge meshlet shapes ON the posed character,
// not the bind pose. skinned is only set when the frame carries a live
// deformed heap (the MMU-fault rule; TOURMALINE_KD_NOSKIN=1 pins the
// bind pose back for A/B).

// The frame's deformed heap (5 words/vertex: pos3, normalW, tangentW) —
// a debug-LOCAL mirror of GfxSkinDeformedHeap (gfxfilter.hlsli), NOT an
// include: gfxfilter.hlsli expects its includer's declarations
// (MaterialsBuffer) and this debug VS is standalone. Same generous
// declared capacity (the BDA-pointer pattern — loads stay in bounds).
struct MeshDebugDeformedHeap { uint word[64 * 1024 * 1024]; };

struct MeshDebugPush
{
    float4x4 model;
    float4x4 viewProj;
    float4 color;
    // Bytes 144..192: padding up to the shared VS-tail scratch area.
    float4 _pad0;
    float4 _pad1;
    float4 _pad2;
    // Skin tail (bytes 192..208, mirrors C++ MeshDebugSkinTail at
    // kMeshDebugSkinTailOffset): deformed heap BDA + the skinned span's
    // global base + the swap flag.
    vk::BufferPointer<MeshDebugDeformedHeap> deformed;
    uint deformedBase;
    uint skinned;
};

[[vk::push_constant]]
ConstantBuffer<MeshDebugPush> pcD;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    // Global heap vertex id (the debug draws share the static index
    // monoblock, so the fixed-function fetch and the deformed-heap
    // storage fetch address the same vertex): the SKINNED swap's index.
    uint vertexID : SV_VertexID;
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    float3 p = IN.position;
    // SKINNED fetch swap (the debug-view form of SeedSkinPos): the draw
    // is a skinned mesh and the frame's deformed heap is live, so take
    // the position from the deformed words (5 words/vertex at
    // (vertexID - deformedBase) * 5; vertexID is the global heap id the
    // debug draws share with every other leg). Index the BDA IN PLACE
    // (never bind the heap to a local - the NV pipeline compiler wedges;
    // see gfxfiltered.vert.hlsl).
    if (pcD.skinned != 0u)
    {
        const uint d = (IN.vertexID - pcD.deformedBase) * 5;
        p = float3(asfloat(pcD.deformed.Get().word[d + 0]),
            asfloat(pcD.deformed.Get().word[d + 1]),
            asfloat(pcD.deformed.Get().word[d + 2]));
    }
    float4 world = mul(pcD.model, float4(p, 1.0f));
    OUT.pos = mul(pcD.viewProj, world);
    OUT.color = pcD.color;
    return OUT;
}
