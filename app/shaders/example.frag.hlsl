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
    float _pad0; // 4B = pad to 16B

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

float3 EvaluateSpotLight(Light light, float3 worldPos, float3 N, float3 baseColor)
{
    float3 toLight = light.position - worldPos;
    float distSq = max(dot(toLight, toLight), 1e-4f);
    float3 L = toLight * rsqrt(distSq);

    float NdotL = saturate(dot(N, L));

    float attenuation = 1.0f / distSq;

    float spotCos = dot(L, -light.direction);

    float inner = cos(light.innerAngle);
    float outer = cos(light.outerAngle);

    // Compute spot factor (0..1)
    float spotFactor = saturate((spotCos - outer) / (inner - outer));

    // Optional: smoothstep for softer edge
    // float spotFactor = saturate(smoothstep(outer, inner, spotCos));

    float3 lightColor = light.color.rgb * light.intensity;

    return baseColor * lightColor * NdotL * attenuation * spotFactor;
}


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
    return baseColor * lightColor * intensity * /*NdotL*/1 * attenuation;
}


PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    // Diffuse texture lookup
    uint texIdx = pc.texDiffuse; // uniform per draw via push constants
    float4 texSample = materialTex[texIdx].Sample(materialSamp[0], IN.uv);

    // Base material color (vertex color * material diffuse * texture)
    float3 baseColor = IN.color * pc.diffuse.rgb * texSample.rgb;
    
    // Ambient term
    float3 ambient = pc.ambient.rgb * texSample.rgb;

    // Normalized normal
    float3 N = normalize(IN.normal);

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
            if (light.shadow_map_index != -1)
            {
                //Sample into shadowmap with id.
                // Alter color base on shadow. 
            }

        }
        

    }
    

    OUT.outColor = float4(litColor, texSample.a);

    return OUT;
}
