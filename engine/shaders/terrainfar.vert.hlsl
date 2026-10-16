// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Tiled terrain FAR vertex shader (L3 blocks, TerrainFarVertexFormat).
//
// VSIn location 0 = packed half4 grid (ix, iz, shade, skirtFlag) — the
// article's Terrain IV 64-bit layout. World XZ reconstructs from the block
// id (firstInstance via SV_InstanceID — placement_instanced precedent) as
// origin + grid * 16 m; height + normal sample the R16 height atlas (TRUE
// 16-bit heights, so hardware linear filtering interpolates correctly).
// Skirt verts drop by kFarSkirtDepth.
//
// Push: ONE block (DXC single-push rule) — the scene mirror + terrain tail
// in terrain_push.hlsli (TERRAIN_NOPC excludes the scene pc from this
// stage). C++ tail mirror: TerrainBlockPush in Rendergraph/terrain.h.
#define TERRAIN_NOPC
#include "common.hlsli"
#include "environment.hlsli"
#include "terrain_push.hlsli"

struct VSIn
{
    [[vk::location(0)]] float4 packedGrid : POSITION;
    // Block id via firstInstance (RecordBlocks far phase passes blockIdx;
    // SV_InstanceID already includes it — placement_instanced precedent).
    uint blockIdx : SV_InstanceID;
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 tangentBFlip : TEXCOORD2;
    [[vk::location(3)]] float4 color : COLOR1;
};

static const float kFarBlockSize = 256.0f;
static const float kFarStep = 16.0f; // kBlockSize / kFarQuads
// Skirt drop, meters — MUST equal kSkirtDepth in
// Rendergraph/terrain_blocks.h (the bake side). No shared constant crosses
// the HLSL/C++ boundary here; both literals are pinned by TerrainBlocksTests
// (skirt-drop) on the bake side and by the far-phase screenshot here.
static const float kFarSkirtDepth = 3.0f;

float FarHeight01(float2 uv)
{
    uint slot = NonUniformResourceIndex(pcT.atlasSlot);
    return textures[slot].SampleLevel(samplers[2], uv, 0).r;
}

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pcT.frameUniforms.Get();

    uint bx = IN.blockIdx % 16u;
    uint bz = IN.blockIdx / 16u;
    float2 blockOrigin = float2(pcT.originX, pcT.originZ)
        + float2((float)bx, (float)bz) * kFarBlockSize;

    float2 gridXZ = blockOrigin + IN.packedGrid.xy * kFarStep;
    float2 uv = (gridXZ - float2(pcT.originX, pcT.originZ))
        * pcT.invWorldSize;
    float texelUv = pcT.invWorldSize; // 1 m texels: worldSize/4096 = 1

    float hC = FarHeight01(uv);
    float hR = FarHeight01(uv + float2(texelUv, 0.0f));
    float hL = FarHeight01(uv - float2(texelUv, 0.0f));
    float hU = FarHeight01(uv + float2(0.0f, texelUv));
    float hD = FarHeight01(uv - float2(0.0f, texelUv));

    float worldY = pcT.baseY + hC * pcT.range;
    if (IN.packedGrid.w > 0.5f)
        worldY -= kFarSkirtDepth;
    float3 worldPos = float3(gridXZ.x, worldY, gridXZ.y);

    // Central-difference normal in world units (1 m taps).
    float dx = ((pcT.baseY + hR * pcT.range)
        - (pcT.baseY + hL * pcT.range)) * 0.5f;
    float dz = ((pcT.baseY + hU * pcT.range)
        - (pcT.baseY + hD * pcT.range)) * 0.5f;
    float3 N = normalize(float3(-dx, 1.0f, -dz));

    // Albedo: mask brightness (baked shade) + slope rock, same weights as
    // the near bake.
    float3 albedo = lerp(float3(0.16f, 0.20f, 0.11f),
        float3(0.55f, 0.56f, 0.50f), IN.packedGrid.z);
    float slope = 1.0f - N.y;
    float rockT = saturate(slope * 3.2f);
    albedo = albedo * (1.0f - rockT) + float3(0.36f, 0.33f, 0.30f) * rockT;

    OUT.worldPos = worldPos;

    float4x4 viewProj = frame.viewProj;
    if (pcT.mainRenderView < MAX_RENDER_VIEWS)
        viewProj = pcT.renderViews.Get().views[pcT.mainRenderView]
            .m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, float4(worldPos, 1.0));

    OUT.normal = N;
    OUT.tangentBFlip = float4(1.0f, 0.0f, 0.0f, -1.0f);
    OUT.color = float4(albedo, 1.0f);
    return OUT;
}
