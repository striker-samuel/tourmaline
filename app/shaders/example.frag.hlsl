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

struct PSIn
{
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
};
struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    // Index is uniform per draw (push constants), so nonuniform isn’t required.
    uint texIdx = pc.texDiffuse;

    OUT.outColor = materialTex[texIdx].Sample(materialSamp[0], IN.uv);
    // If you really want per-texture samplers too, use materialSamp[pc.texAmbient] or similar.

    return OUT;
}
