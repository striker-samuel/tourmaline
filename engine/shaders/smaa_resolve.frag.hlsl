#define SMAA_PASS
#include "common.hlsli"

#include "smaa_push.hlsli"

#define SMAA_PRESET_HIGH
#define SMAA_VULKAN_BINDLESS
#define SMAA_REPROJECTION 1
#define SMAA_INCLUDE_VS 0
#include "smaa.hlsli"

struct PSIn
{
    float2 uv : TEXCOORD0;
};

// T2x temporal resolve (shares the fullscreen vertex shader with the
// tonemapper). Combines this frame's neighborhood-blended subsample with
// the previous frame's, reprojected along the (negated) velocity buffer and
// attenuated by the velocity mismatch packed in the two outputs' alpha
// (SMAAResolvePS + CryEngine-style velocity weighting). The first T2x
// frame (historyValid == 0) passes the current subsample through - the
// history ping-pong slot was never written in that case. The histories hold
// gamma-encoded bytes; the swapchain is sRGB, so the blended result is
// decoded to linear (outputLinear) and the hardware store encode restores
// it.
float4 mainPS(PSIn IN) : SV_Target0
{
    Texture2D currentTex = textures[NonUniformResourceIndex(sp.currentTexIndex)];
    Texture2D previousTex = textures[NonUniformResourceIndex(sp.previousTexIndex)];
    Texture2D velocityTex = textures[NonUniformResourceIndex(sp.velocityTexIndex)];
    float4 result;
    if (sp.historyValid == 0u)
        result = SMAASamplePoint(currentTex, IN.uv);
    else
        result = SMAAResolvePS(IN.uv, currentTex, previousTex, velocityTex);
    if (sp.outputLinear)
        result.rgb = SrgbDecode(result.rgb);
    return result;
}
