#include "common.h"

[[vk::push_constant]]
struct PC_All
{
    float4x4 view;
    float4x4 proj;
    int material_instance_index;
    int3 _pad0; // 16B pad to keep 16-byte alignment

    float4 ambient;
    float4 diffuse;
    uint texAmbient;
    uint texDiffuse;
    uint texAlpha;
    uint _pad1; // keep size at 192 bytes total
} pc;


[[vk::binding(2,0)]] Texture2D materialTex[]; // sampled images
[[vk::binding(3,0)]] SamplerState materialSamp[]; // samplers
[[vk::binding(4, 0)]]
cbuffer Lights
{
    Light g_Lights[10]; // or a struct containing them
};

// iamgeViews to the shadowMap
[[vk::binding(5, 0)]]
Texture2DArray gShadowMaps[];

[[vk::binding(6, 0)]]
SamplerState gShadowSampler[];

struct PSIn
{
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1; // from VS
    float3 normal : TEXCOORD2; // from VS, in world space
};
struct PSOut
{
    float4 outColor : SV_Target;
};


float PCFShadow(float2 uv, float currentDepth, int layer, int shadowmap_index)
{
    float2 texelSize = 1.0 / float2(SHADOW_RES, SHADOW_RES);

    float sum = 0.0;
    int samples = 0;

    // 3x3 kernel around uv
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 offset = float2(x, y) * texelSize;
            float3 uvw = float3(uv + offset, layer);

            float d = gShadowMaps[shadowmap_index].Sample(gShadowSampler[shadowmap_index], uvw).r;

            // Standard Z: closer => smaller depth
            // Lit if receiver is not behind the occluder
            const float bias = 0.001f;
            sum += (currentDepth - bias <= d) ? 1.0f : 0.0f;
            samples++;
        }
    }

    return sum / samples; // 0..1 shadow factor
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    float4x4 lightView;
    float4x4 lightViewProj;

    // Diffuse texture lookup
    uint texIdx = pc.texDiffuse; // uniform per draw via push constants
    float4 texSample = materialTex[texIdx].Sample(materialSamp[0], IN.uv);

    // Base material color (vertex color * material diffuse * texture)
    float3 baseColor = IN.color * pc.diffuse.rgb * texSample.rgb;
    
    OUT.outColor = float4(baseColor.rgb, texSample.a);

    return OUT;
}
