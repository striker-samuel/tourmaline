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
    float4 color; // RGB color, A unused
    float4x4 transform; // use transform[3].xyz as world-space position
    int type; // 0 = point (assumed) //32
    float intensity; // we’ll cast to float
    int _pad[2];
    int _pad2[8];
};

#ifndef MAX_LIGHTS
#define MAX_LIGHTS 10
#endif

[[vk::binding(2,0)]] Texture2D materialTex[]; // sampled images
[[vk::binding(3,0)]] SamplerState materialSamp[]; // samplers
[[vk::binding(4, 0)]]
cbuffer Lights
{
    Light g_Lights[MAX_LIGHTS]; // or a struct containing them
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

// -----------------------------------------------------------------------------
// Helper: compute point-light contribution
// -----------------------------------------------------------------------------
float3 EvaluatePointLight(Light light, float3 worldPos, float3 N, float3 baseColor)
{
    // Light position from transform matrix
    float3 lightPos = light.transform[3].xyz;

    float3 toLight = lightPos - worldPos;
    float distSq = dot(toLight, toLight);
    distSq = max(distSq, 1e-4f);

    float3 L = toLight * rsqrt(distSq); // normalize

    float NdotL = saturate(dot(N, L)); // [0,1]
    float attenuation = 1.0f / distSq; // inverse-square

    float intensity = (float) light.intensity;
    float3 lightColor = light.color.rgb;

    return baseColor * lightColor * intensity * NdotL * attenuation;
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
    for (uint i = 0; i < 1; ++i)
    {
        Light light = g_Lights[i];

        // Assume type == 0 is point light
        if (light.type != 0)
            continue;

        litColor += EvaluatePointLight(light, IN.worldPos, N, baseColor);
    }

    OUT.outColor = float4(litColor, texSample.a);

    return OUT;
}
