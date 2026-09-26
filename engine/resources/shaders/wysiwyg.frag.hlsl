// WYSIWYG compose fragment: blits the hosted SceneRenderer target (the
// App's final LDR frame — sRGB surface-format image) into the editor's
// swap view. The sRGB view decodes to linear on sample and the editor's
// sRGB target re-encodes on store, so this is an identity passthrough.
// ENGINE-SHIPPED shader (engine/resources/shaders — editor concerns,
// distinct from the content scene set): no common.hlsli include (it
// drags the scene PushConstants block in — one push block per shader),
// so the bindless tables are declared directly with the engine's
// binding attributes (same layout as common.hlsli).
// C++ mirror: WysiwygGpuPush (editor_render.cpp, 16 bytes).
[[vk::binding(0, 0)]]
Texture2D textures[];
[[vk::binding(1, 0)]]
SamplerState samplers[];

struct WysiwygPush
{
    uint textureIndex;   // bindless slot of the hosted target view
    uint samplerIndex;   // linear sampler slot
    float2 pad;
};

[[vk::push_constant]]
ConstantBuffer<WysiwygPush> wp;

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    float3 rgb = textures[NonUniformResourceIndex(wp.textureIndex)]
        .Sample(samplers[NonUniformResourceIndex(wp.samplerIndex)], IN.uv)
        .rgb;
    OUT.outColor = float4(rgb, 1.0f);
    return OUT;
}
