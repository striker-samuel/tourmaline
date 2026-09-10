// Seed-depth VS, opaque phase (VS-only pipeline, back-culled like the
// prepass). See gfxseed.hlsli for the shared prologue.
#define GFXSEED_PASS
#include "common.hlsli"
#include "gfxseed.hlsli"

[[vk::push_constant]]
ConstantBuffer<SeedVsMainPush> pcSeed;

SeedVSOut mainVS(SeedVSIn IN)
{
    SeedVSOut OUT;
    OUT.pos = SeedClipPos(IN.position, IN.instanceID,
        pcSeed.draws, pcSeed.renderViews, pcSeed.renderViewIndex);
    OUT.color = float3(1.0f, 1.0f, 1.0f);
    OUT.uv = float2(0.0f, 0.0f);
    return OUT;
}
