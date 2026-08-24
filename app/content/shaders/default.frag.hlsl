#include "light.hlsli"
#include "common.hlsli"

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

// Here because symbols need to be defined before literal copy paste of include!
#include "shadows.hlsli"

//float3 EvaluateSpotLight(
//    Light light,
//    float3 worldPos,
//    float3 N,
//    float3 baseColor
//    )
//{
//    // Vector from point to light
//    float3 toLight = light.position - worldPos;
//    float distSq = max(dot(toLight, toLight), 1e-4f);
//    float3 L = toLight * rsqrt(distSq); // normalize

//    // Lambert term
//    float NdotL = saturate(dot(N, L));
//    if (NdotL <= 0.0f)
//        return 0.0.xxx;

//    // Inverse square attenuation
//    float attenuation = 1.0f / distSq;

//    // Make sure light.direction is treated as normalized
//    float3 lightDir = normalize(light.direction);

//    // Cosine of angle between cone axis and point
//    // (cone axis points *out* of the light, so use -lightDir)
//    float spotCos = dot(L, -lightDir);

//    // Assume innerAngle / outerAngle are in radians. They indeed are.
//    float innerCos = cos(light.innerAngle);
//    float outerCos = cos(light.outerAngle);

//    // Avoid divide-by-zero if inner == outer
//    float denom = max(innerCos - outerCos, 1e-4f);
//    float spotFactor = saturate((spotCos - outerCos) / denom);
//    // If you want softer edge:
//    // float spotFactor = saturate(smoothstep(outerCos, innerCos, spotCos));

//    // Shadow factor (1 = lit, 0 = fully shadowed)
//    float shadow = 1.0;
//    if (light.shadow_map_index >= 0)
//    {
//        shadow = GetShadowFactor(gShadowMaps, gShadowSamplers, worldPos, light);
//    }

//    float3 lightColor = light.color.rgb * light.intensity;
    
//    return baseColor * lightColor * NdotL * attenuation * spotFactor * shadow;
    
//}

//// Pure lambertian
//float3 EvaluatePointLight(Light light, float3 worldPos, float3 N, float3 baseColor)
//{
//    // Use explicit position stored in the struct
//    float3 lightPos = light.position;

//    // Vector to light
//    float3 toLight = lightPos - worldPos;
//    float distSq = max(dot(toLight, toLight), 1e-4f);

//    // Direction to light
//    float3 L = toLight * rsqrt(distSq);

//    // Lambert term
//    float NdotL = saturate(dot(N, L));

//    // Inverse-square attenuation
//    float attenuation = 1.0f / distSq;

//    // Light parameters
//    float3 lightColor = light.color.rgb;
//    float intensity = light.intensity;

//    // Final contribution
//    return baseColor * lightColor * intensity * NdotL * attenuation;
    
//    // NormalsDebug
//    //return (dot(N, L) > 0) ? float3(1, 0, 0) : float3(0, 0, 1);

//}


float3 EvaluateDirectLight(
    Light light,
    float3 worldPos,
    float3 N,
    float3 baseColor
)
{
    // Direction *from the point toward the light*
    // If light.direction points "forward" from the light, use -direction.
    float3 L = normalize(-light.direction);

    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f)
        return 0.0.xxx;

    float3 lightColor = light.color.rgb * light.intensity;

    float shadow = 1.0f;
    if (light.shadow_map_index >= 0)
    {
        //shadow = GetShadowFactorODB(gShadowMaps, gShadowSamplers, worldPos, light, N);
        shadow = GetShadowFactor(1, worldPos, light);
    }

    return baseColor * lightColor * NdotL * shadow;
}


PSOut mainPS(PSIn IN)
{
    
    PSOut OUT;
    float4x4 lightView;
    float4x4 lightViewProj;

    MaterialValues values = pc.bMats.Get().mats[pc.materialId];
    //Light lights[] = pc.bLights.Get().lights;
    uint texIdx = values.tex.x;
    
    // using #0 sampler always hmmmm fix this! 
    float4 texSample = textures[texIdx].Sample(samplers[0], IN.uv);
    // Base material color (vertex color * material diffuse * texture)
    float3 baseColor = IN.color * values.diffuse.rgb * texSample.rgb;
    
    // Ambient term
    float3 ambient = values.ambient.rgb * texSample.rgb;
    // Normalized normal
    float3 N = normalize(IN.normal);

    //float shadowFactor = GetShadowFactor(1, IN.worldPos, pc.bLights.Get().lights[0]);
    float3 litColor = EvaluateDirectLight(pc.bLights.Get().lights[0],
    IN.worldPos,
    N,
    ambient);

    //[loop]
    ////for (uint i = 0; i < MAX_LIGHTS; ++i)
    //for (uint i = 0; i < 1; ++i)
    //{
    //    Light light = g_Lights[i];

    //    if (light.type == 0)
    //    {
    //        litColor += EvaluatePointLight(light, IN.worldPos, N, baseColor);
    //    }
    //    else if (light.type == 1)
    //    {
    //        litColor += EvaluateSpotLight(light, IN.worldPos, N, baseColor);

    //    }
    //    else if (light.type == 2)
    //    {
    //        litColor += EvaluateDirectLight(light, IN.worldPos, N, baseColor);
    //    }
    //    else
    //    {
    //        continue;
    //    }
    //}

    //float shadowFactor = 1.0f;
    //Light light = pc.bLights.Get().lights[0];
    //if (light.shadow_map_index >= 0)
    //{
    //    shadow = GetShadowFactor(
    //    pc.comparisonSamplerId,
    //    IN.worldPos,
    //    light
    //);
    //}
    //depth = (1 - depth) * 20;
    
    OUT.outColor = float4(litColor.rgb, texSample.a);

    //float3 N = normalize(IN.normal);
    //float3 L = normalize(-g_Lights[0].direction);
    //float ndl = dot(N, L); // NOTE: not saturate yet
    //OUT.outColor = float4(ndl.xxx * 0.5f + 0.5f, 1);
    //return OUT;

    
    return OUT;
}
