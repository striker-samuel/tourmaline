// Seed-depth VS, MASK cutout phase (pairs with shadow.frag alpha-test).
// Same VSOut subset as shadow.vert so it links with shadow.frag.
// See gfxseed.hlsli for the shared prologue.
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
    OUT.color = IN.color.rgb;
    OUT.uv = IN.uv;
    return OUT;
}
