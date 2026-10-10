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
    // No FS in this pipeline (opaque seed, depth-only): the dither-closeup
    // outputs stay inert zeros (the MASKED twin is the one that fills them
    // for shadow.frag's screen-door).
    OUT.worldPos = float3(0.0f, 0.0f, 0.0f);
    OUT.ditherCloseup = 0u;
    return OUT;
}
