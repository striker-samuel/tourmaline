#define UI_PASS
#include "common.hlsli"

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/ui.h UiCompositePush (16 bytes).
struct UiCompositePush
{
    uint uiTextureIndex;
    uint samplerIndex;
    uint decodeOutput; // 1 = decode to linear for the sRGB swapchain store
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<UiCompositePush> up;

// Straight-alpha composite: the fixed-function blend state (srcAlpha /
// oneMinusSrcAlpha) applied by the pipeline does
//   dst.rgb = ui.rgb * ui.a + dst.rgb * (1 - ui.a)
// so the UI target can hold ImGui's straight-alpha output directly. The UI
// target is the UNORM sibling of the surface format, so the sampled bytes
// are already sRGB-encoded; decodeOutput hands linear to the sRGB
// swapchain, whose hardware store encode restores them.
PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    float4 ui = float4(0, 0, 0, 0);
    if (up.uiTextureIndex != 0xffffffff && up.uiTextureIndex < 100000)
        ui = textures[NonUniformResourceIndex(up.uiTextureIndex)].Sample(samplers[NonUniformResourceIndex(up.samplerIndex)], IN.uv);
    if (up.decodeOutput)
        ui.rgb = SrgbDecode(ui.rgb);
    OUT.outColor = ui;
    return OUT;
}
