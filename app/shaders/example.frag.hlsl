#include "shader_utils.h"

#ifndef MAX_LIGHTS
#define MAX_LIGHTS 10
#endif

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

struct Light
{
    float4 color; // 16B

    float3 position; // 12B
    int flags;
    //float _pad0; // 4B = pad to 16B

    float3 direction; // 12B
    float intensity; // 4B
    
    int type; // 4B
    float innerAngle;
    float outerAngle;
    int shadow_map_index;
    float4x4 lightVP;
};


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


// Shadow map resolution (must match your actual shadow map size)
static const float SHADOW_RES = 2048.0; // or whatever you use

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

float GetShadowFactor(float3 worldPos, Light light)
{
    // Transform world position into the light's clip space
    float4 clip = mul(light.lightVP, float4(worldPos, 1.0));
    float3 ndc = clip.xyz / clip.w; // NDC: x,y in [-1,1], z in [0,1] (Vulkan-style)

    // If behind near / beyond far in clip space, treat as lit (no shadow)
    // Still valid for reversed Z: ndc.z in (0,1) is the valid range.
    if (ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;

    // NDC xy [-1,1] -> [0,1] UVs
    float2 uv = ndc.xy * 0.5f + 0.5f;
    // Outside the shadow map => lit
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;
    
    // Sample depth from the shadow map array
    float depth = gShadowMaps[light.shadow_map_index].Sample(gShadowSampler[light.shadow_map_index], float3(uv, light.shadow_map_index)).r;

    const float bias = 0.001f;
    if (ndc.z < 0.0f || ndc.z > 1.0f)
        return 1.0f;
    
    float currentDepth = ndc.z;
    return PCFShadow(uv, currentDepth, 0, light.shadow_map_index);

    //return (ndc.z - bias <= depth) ? 1.0f : 0.0f;
}

float3 EvaluateSpotLight(
    Light light,
    float3 worldPos,
    float3 N,
    float3 baseColor
    )
{
    // Vector from point to light
    float3 toLight = light.position - worldPos;
    float distSq = max(dot(toLight, toLight), 1e-4f);
    float3 L = toLight * rsqrt(distSq); // normalize

    // Lambert term
    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f)
        return 0.0.xxx;

    // Inverse square attenuation
    float attenuation = 1.0f / distSq;

    // Make sure light.direction is treated as normalized
    float3 lightDir = normalize(light.direction);

    // Cosine of angle between cone axis and point
    // (cone axis points *out* of the light, so use -lightDir)
    float spotCos = dot(L, -lightDir);

    // Assume innerAngle / outerAngle are in radians. They indeed are.
    float innerCos = cos(light.innerAngle);
    float outerCos = cos(light.outerAngle);

    // Avoid divide-by-zero if inner == outer
    float denom = max(innerCos - outerCos, 1e-4f);
    float spotFactor = saturate((spotCos - outerCos) / denom);
    // If you want softer edge:
    // float spotFactor = saturate(smoothstep(outerCos, innerCos, spotCos));

    // Shadow factor (1 = lit, 0 = fully shadowed)
    float shadow = 1.0;
    if (light.shadow_map_index >= 0)
    {
        shadow = GetShadowFactor(worldPos, light);
    }

    float3 lightColor = light.color.rgb * light.intensity;
    
    return baseColor * lightColor * NdotL * attenuation * spotFactor * shadow;
    
}

// Pure lambertian
float3 EvaluatePointLight(Light light, float3 worldPos, float3 N, float3 baseColor)
{
    // Use explicit position stored in the struct
    float3 lightPos = light.position;

    // Vector to light
    float3 toLight = lightPos - worldPos;
    float distSq = max(dot(toLight, toLight), 1e-4f);

    // Direction to light
    float3 L = toLight * rsqrt(distSq);

    // Lambert term
    float NdotL = saturate(dot(N, L));

    // Inverse-square attenuation
    float attenuation = 1.0f / distSq;

    // Light parameters
    float3 lightColor = light.color.rgb;
    float intensity = light.intensity;

    // Final contribution
    return baseColor * lightColor * intensity * NdotL * attenuation;
    
    // NormalsDebug
    //return (dot(N, L) > 0) ? float3(1, 0, 0) : float3(0, 0, 1);

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
    
    // Ambient term
    float3 ambient = pc.ambient.rgb * texSample.rgb;

    // Normalized normal
    float3 N = normalize(IN.normal);

    //float shadow = ShadowFactor(IN.worldPos);
    // Accumulate lighting
    float3 litColor = ambient;

    [loop]
    for (uint i = 0; i < MAX_LIGHTS; ++i)
    {
        Light light = g_Lights[i];

        if (light.type == 0)
        {
            litColor += EvaluatePointLight(light, IN.worldPos, N, baseColor);
        }
        else
        {
            litColor += EvaluateSpotLight(light, IN.worldPos, N, baseColor);

        }
    }
    
    //float depth = gShadowMaps.Sample(gShadowSampler, float3(IN.uv, g_Lights[0].shadow_map_index)).r;
    //depth = (1 - depth) * 20;
    
    OUT.
        outColor = float4(litColor.rgb, texSample.a);

    return OUT;
}
