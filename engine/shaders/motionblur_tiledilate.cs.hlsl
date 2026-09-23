// Motion blur tile dilation, phase B (5x5 neighborhood max).
//
// One thread per tile: Loads the raw per-tile max velocities (phase A,
// motionblur_tiles.cs.hlsl) over the 5x5 tile neighborhood and writes
// the max-length vector to the dilated tile map. 16px tiles x 2 rings =
// ~+-40px reach, matching the default 24px blur clamp (larger radii
// still streak, just with the closer rings' velocities).
//
// The gather (motionblur.frag.hlsl) point-Loads this map per pixel and
// adopts the tile velocity when it beats the pixel's own — the CoD:AW
// tile-dilation trick that makes background around moving silhouettes
// streak instead of popping sharp.
//
// C++ mirror: smvk::MotionBlurTileDilatePush in motionblur.h (16 bytes).

#define MOTIONBLUR_TILEDILATE_PASS
#include "common.hlsli"

// Bindless storage image array (set 0, binding 2) — see
// motionblur_tiles.cs.hlsl / VulkanResourceManager::UpdateDescriptors.
[[vk::binding(2, 0)]]
RWTexture2D<float4> motionBlurStorage[];

// Must match Engine/Rendergraph/motionblur.h MotionBlurTileDilatePush
// (16 bytes).
struct MotionBlurTileDilatePush
{
    uint tileMaxTextureIndex; // raw tile map (read via textures[])
    uint tileVelTextureIndex; // dilated tile map (write via storage[])
    uint tileCountX;
    uint tileCountY;
};

[[vk::push_constant]]
ConstantBuffer<MotionBlurTileDilatePush> pcDilate;

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pcDilate.tileCountX || tid.y >= pcDilate.tileCountY)
        return;

    Texture2D tileMax = textures[NonUniformResourceIndex(pcDilate.tileMaxTextureIndex)];
    const int2 maxTc = int2((int)pcDilate.tileCountX, (int)pcDilate.tileCountY) - 1;

    float2 bestVel = float2(0.0f, 0.0f);
    float bestLen2 = -1.0f;
    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 tc = clamp(int2((int)tid.x, (int)tid.y) + int2(dx, dy),
                int2(0, 0), maxTc);
            float2 v = tileMax.Load(int3(tc, 0)).xy;
            float l2 = dot(v, v);
            if (l2 > bestLen2)
            {
                bestLen2 = l2;
                bestVel = v;
            }
        }
    }

    motionBlurStorage[NonUniformResourceIndex(pcDilate.tileVelTextureIndex)]
        [tid.xy] = float4(bestVel, 0.0f, 0.0f);
}
