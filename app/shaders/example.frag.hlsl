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
Texture2DArray gShadowMaps;

[[vk::binding(6, 0)]]
SamplerState gShadowSampler;

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



float4x4 GetLightViewProj(Light light)
{
    float4x4 lightView = GetLightView(light.position, light.direction);
    float4x4 lightProj = GetLightProj(light.outerAngle, 1.0f, 0.1f, 100.0f);
    lightProj[1][1] *= -1.0f;
            //lightProj[2][3] = -1;
    float4x4 lightViewProj = mul(lightProj, lightView); // proj * view
    
    return lightViewProj;

}


//float PCFShadow(float2 uv, float currentDepth, Texture2D map, SamplerState samp)
//{
//    float sum = 0;
//    float2 texel = float2(1.0 / SHADOW_RES, 1.0 / SHADOW_RES);

//    [unroll]
//    for (int x = 0; x < 2; ++x)
//    {
//        [unroll]
//        for (int y = 0; y < 2; ++y)
//        {
//            float d = map.Sample(samp, uv + float2(x, y) * texel).r;
//            sum += currentDepth > d ? 0.0 : 1.0;
//        }
//    }
//    return sum * 0.25;
//}


float GetShadowFactor(float3 worldPos, Light light)
{
    // Transform world position into the light's clip space
    float4 clip = mul(GetLightViewProj(light), float4(worldPos, 1.0));
    float3 ndc = clip.xyz / clip.w; // NDC: x,y in [-1,1], z in [0,1] (Vulkan-style)

    // If behind near / beyond far in clip space, treat as lit (no shadow)
    // Still valid for reversed Z: ndc.z in (0,1) is the valid range.
    if (ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;

    // NDC xy [-1,1] -> [0,1] UVs
    float2 uv = ndc.xy * 0.5f + 0.5f;
    // Outside the shadow map => lit
    //if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
    //    return 1.0f;

    // Sample depth from the shadow map array
    float depth = gShadowMaps.Sample(gShadowSampler, float3(uv, light.shadow_map_index)).r;

    // Reversed-Z comparison:
    //  - Standard Z: closer => smaller depth, test: (ndc.z - bias <= depth)
    //  - Reversed Z: closer => larger depth, test: (ndc.z + bias >= depth)
    const float bias = 0.001f;

    return (ndc.z + bias <= depth) ? 1.0f : 0.0f;
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
    //for (uint i = 0; i < MAX_LIGHTS; ++i)
    for (uint i = 0; i < 1; ++i)
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
    
    OUT.
        outColor = float4(litColor, texSample.a);

    return OUT;
}
