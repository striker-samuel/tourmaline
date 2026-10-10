// WYSIWYG compose fragment: blits the hosted SceneRenderer target (the
// App's final LDR frame — sRGB surface-format image) into the editor's
// swap view. The sRGB view decodes to linear on sample and the editor's
// sRGB target re-encodes on store, so this is an identity passthrough.
// ENGINE-SHIPPED shader (engine/resources/shaders — editor concerns,
// distinct from the content scene set): no common.hlsli include (it
// drags the scene PushConstants block in — one push block per shader),
// so the bindless tables are declared directly with the engine's
// binding attributes (same layout as common.hlsli).
// C++ mirror: WysiwygGpuPush (editor_render.cpp, 24 bytes).
[[vk::binding(0, 0)]]
Texture2D textures[];
[[vk::binding(1, 0)]]
SamplerState samplers[];

struct WysiwygPush
{
    uint textureIndex;   // bindless slot of the hosted target view
    uint samplerIndex;   // linear sampler slot
    float2 uvScale;      // aspect-preserving cover-crop (1,1) steady-state
    float2 uvOffset;     // centered sub-region offset (0,0) steady-state
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
    // Aspect-preserving cover-crop (see RecordWysiwygPass): identity in
    // the steady state (target extent == viewport rect); during an extent
    // drift the centered sub-region keeps geometry undistorted while the
    // debounced relaunch lands.
    float2 uv = IN.uv * wp.uvScale + wp.uvOffset;
    float3 rgb = textures[NonUniformResourceIndex(wp.textureIndex)]
        .Sample(samplers[NonUniformResourceIndex(wp.samplerIndex)], uv)
        .rgb;
    OUT.outColor = float4(rgb, 1.0f);
    return OUT;
}
