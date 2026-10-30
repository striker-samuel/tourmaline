// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Flat UV view vertex shader: the mesh is projected INTO UV space —
// the clip position comes straight from the selected UV channel, so the
// rasterized result is the unwrapped shell seen straight-on with its
// texture content (uvflat0/uvflat1.frag). Z is a constant just below
// the reversed-Z near plane (1): the fill stays behind depth-tested UV
// wires drawn by the editor line pass at z = 1.
// UVFLAT_CHANNEL is defined by the including entry file
// (uvflat0.vert.hlsl = base TEXCOORD0, uvflat1.vert.hlsl = lightmap
// atlas UV, unpacked from chart TEXCOORD1 + chart id via the per-mesh
// chart tail).
#if UVFLAT_CHANNEL == 1
// Channel-1 (lightmap) flat view: the VS unpacks the atlas UV itself,
// so this entry declares its own push block (UVFLAT1_PASS excludes the
// scene pc from this stage — see common.hlsli). Layout mirrors the
// scene push for bytes 0..192 (model = 2D pan/zoom) with a per-mesh
// chart tail at 192 (the scene push's unused prevModel scratch — same
// overlay precedent as FilteredVsPush). The app pushes the full scene
// pc at 0, then the 16B tail at 192 per mesh. The fragment stage keeps
// the scene pc (materials, frame) — same split as the filtered pipe.
#define UVFLAT1_PASS
#endif
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "gfxfilter.hlsli"
#include "gpu_platform.hlsli"

#ifndef UVFLAT_CHANNEL
#define UVFLAT_CHANNEL 0
#endif

#if UVFLAT_CHANNEL == 1
struct UvFlatVsPush
{
    float4x4 model; // 0..64: 2D pan/zoom (mirrors scene pc.model)
    float4x4 _pad0; // 64..128
    float4x4 _pad1; // 128..192
    // Per-mesh chart tail (pushed at byte 192 per mesh; zeroed scratch
    // reads hasXforms == 0 and falls back to raw chart uvs).
    uint hasXforms;
    uint chartOffset;
    GPU_PTR_READ(GfxChartXforms) lmXforms;
};
GPU_PUSH(UvFlatVsPush, pcUV);
#endif

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    // Lightmaps are deprecated: StaticVertexFormat no longer binds the
    // second UV channel (TexCoord1/ChartId would be locations 5/6), so
    // this VS declares neither — the channel-1 (lightmap) flat view is
    // parked with it: every vertex collapses to uv 0 until the
    // gated-off LightmappedStaticVertexFormat option returns with a
    // chart-unpack VS variant.
    uint instanceID : SV_InstanceID;
};
struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
#if UVFLAT_CHANNEL == 1
    // Lightmaps deprecated (see VSIn): no chart UV rides this format, so
    // the parked channel-1 flat view collapses to uv 0 — the chart-tail
    // push fields stay declared for layout identity with the C++ mirror,
    // nothing reads them. Restore the unpack when the
    // LightmappedStaticVertexFormat option comes back.
    const float2 atlasUv = float2(0.0f, 0.0f);
    const float2 uvFlat = atlasUv;
    float uvZoom = pcUV.model[0][0];
    float2 uvPan = float2(pcUV.model[3][0], pcUV.model[3][1]);
#else
    const float2 uvFlat = IN.uv;
    float uvZoom = pc.model[0][0];
    float2 uvPan = float2(pc.model[3][0], pc.model[3][1]);
#endif
    // glTF image space: (0,0) top-left; Vulkan NDC: -y is up, so uv.y=0
    // maps to -1 (screen top) — the texture reads with image orientation.
    // Editor pane navigation (no recompile needed per view): model
    // carries the 2D UV transform (model[0][0] = zoom, model[3].xy = pan
    // in UV units, centered: uvT = (uv-0.5)*zoom + 0.5 + pan). Identity
    // (zoom 1, pan 0) reproduces the legacy mapping; passes that never
    // set a transform (lightmap PNG export) push a zeroed model, which
    // is detected below and treated as identity.
    if (uvZoom < 1e-6f || uvZoom > 64.0f)
    {
        uvZoom = 1.0f;
        uvPan = float2(0.0f, 0.0f);
    }
    const float2 uvT = (uvFlat - float2(0.5f, 0.5f)) * uvZoom +
        float2(0.5f, 0.5f) + uvPan;
    OUT.pos = float4(uvT.x * 2.0 - 1.0, uvT.y * 2.0 - 1.0, 0.9999, 1.0);
    OUT.color = IN.color;
    OUT.uv = IN.uv;
    OUT.worldPos = float3(uvFlat, 0.0);
    OUT.normal = float3(0.0, 0.0, 1.0);
    OUT.tangent = float4(1.0, 0.0, 0.0, 1.0);
#if UVFLAT_CHANNEL == 1
    OUT.lightmapUv = atlasUv;
#else
    OUT.lightmapUv = float2(0.0f, 0.0f);
#endif
    return OUT;
}
