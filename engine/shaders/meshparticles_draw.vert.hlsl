// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// meshparticles_draw.vert.hlsl — the MESH-particle draw path (PR06):
// draws the GPU mesh-particle pool via DrawIndexedIndirect, one command
// per MESH bin — the PR03 chain's visible-index contract:
//
//   - INSTANCE FETCH: visible[SV_InstanceID] -> the SOURCE particle
//     slot -> the 96-byte mesh particle. SV_InstanceID includes the
//     command's firstInstance under SPIR-V, so bin b's draw walks
//     exactly [offsets[b], offsets[b] + counts[b]).
//   - model matrix = T(pos) * R(QUATERNION) * S(uniform) — the FULL
//     orientation (the spinning debris/leaf motion the yaw-only
//     placement record deliberately cannot carry). Uniform positive
//     scale: normals rotate by R without the adjugate, no mirroring.
//   - vertex color = the particle TINT (the spawn-time emitter color
//     with its per-channel jitter); the emissive scalar rides the
//     colorEmit.w field for a future self-lit term.
//
// Push: 32B tail at byte offset 192 (the main pass' unused prevModel
// scratch — the placement-instanced precedent). The 256B scene push
// stays intact for the fragment shader, so all existing fragment
// shaders work unchanged.
#define PLACEMENT_INSTANCED_PASS // the StaticLayout input contract
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "meshparticles.hlsli"

struct MeshParticlesVsPush
{
    // Preserve the scene push bytes this VS does not use (192B); the
    // tail below is what the app pushes at offset 192.
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    vk::BufferPointer<PlacementVisibleBuffer> visible;    // 192
    vk::BufferPointer<MeshParticleBuffer> particles;       // 200
    vk::BufferPointer<RenderViewBuffer> renderViews;       // 208
    uint renderViewIndex; // 216
    uint flags;           // 220 (reserved; 0)
};

[[vk::push_constant]]
ConstantBuffer<MeshParticlesVsPush> pcMP;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    // (the placement-instanced note applies: no TexCoord1/ChartId —
    // particle draws are dynamic, never lightmapped.)
    uint instanceID : SV_InstanceID;
};
struct VSOut
{
    float4 pos : SV_Position;

    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
};

// The quaternion rotation matrix lives in meshparticles.hlsli
// (MeshParticleQuatMatrix — shared with the cull's bound rotation).

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    // Indirect firstInstance is pre-added to SV_InstanceID by hardware:
    // idx is the position inside the GLOBAL visible list.
    const uint idx = IN.instanceID;
    const uint source = pcMP.visible.Get().v[idx];
    const MeshParticleGpu p = pcMP.particles.Get().p[source];

    // Compose T * R(quat) * S (uniform scale from the size envelope).
    const float s = p.velSize.w;
    const float3x3 R = MeshParticleQuatMatrix(p.rot);
    const float4 world = float4(mul(R, IN.position) * s + p.posLife.xyz, 1.0f);
    OUT.worldPos = world.xyz;
    const float4x4 viewProj =
        pcMP.renderViews.Get().views[pcMP.renderViewIndex].m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, world);

    OUT.color = p.colorEmit.rgb;
    OUT.uv = IN.uv;
    OUT.lightmapUv = float2(0.0f, 0.0f);
    OUT.lmChunk = 0xFFFFFFFFu;

    float3 normal = OctDecodeNormal(IN.normalPacked);
    float4 tangent = OctDecodeTangent(IN.tangentPacked);
    // Uniform positive scale: rotation only, no adjugate, no mirror fixup.
    OUT.normal = normalize(mul(R, normal));
    OUT.tangent = float4(normalize(mul(R, tangent.xyz)), tangent.w);

    return OUT;
}
