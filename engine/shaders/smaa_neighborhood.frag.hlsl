#define SMAA_PASS
#include "common.hlsli"

#include "smaa_push.hlsli"

#define SMAA_PRESET_HIGH
#define SMAA_VULKAN_BINDLESS
#define SMAA_REPROJECTION 1
#define SMAA_INCLUDE_VS 0
// The motion vectors include the T2x/DLSS jitter (required for the
// resolve's exact reprojection). Strip its known per-frame delta from the
// velocity magnitude packed into alpha - the resolve's history attenuation
// must react to TRUE motion only (see smaa.hlsli).
#define SMAA_VELOCITY_JITTER_DELTA float2(sp.jitterDeltaX, sp.jitterDeltaY)
#include "smaa.hlsli"

struct PSIn
{
    float2 texcoord : TEXCOORD0;
    float4 offset : TEXCOORD1;
};

// Blends the pixel with its chosen neighbor (exploits bilinear filtering)
// and packs the antialiased velocity magnitude into the alpha channel
// (sqrt(5*|v|)) for the temporal resolve. The target is history[parity] in
// T2x mode (RGBA, alpha preserved) or the swapchain in SMAA 1x mode
// (alpha ignored there). The chain works in gamma space; when the target is
// the sRGB swapchain (outputLinear) the result is decoded to linear first
// so the hardware store encode restores the same bytes.
float4 mainPS(PSIn IN) : SV_Target0
{
    Texture2D colorTex = textures[NonUniformResourceIndex(sp.colorTexIndex)];
    Texture2D blendTex = textures[NonUniformResourceIndex(sp.blendTexIndex)];
    Texture2D velocityTex = textures[NonUniformResourceIndex(sp.velocityTexIndex)];
    float4 result = SMAANeighborhoodBlendingPS(IN.texcoord, IN.offset,
        colorTex, blendTex, velocityTex);
    if (sp.outputLinear)
        result.rgb = SrgbDecode(result.rgb);
    return result;
}
