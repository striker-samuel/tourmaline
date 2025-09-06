#ifndef MAX_MATERIALS
#define MAX_MATERIALS 100
#endif

struct PSIn
{
    float3 fragColor : COLOR0; // layout(location=0) in
    float2 fragTexCoord : TEXCOORD0; // layout(location=1) in
};

struct PSOut
{
    float4 outColor : SV_Target; // layout(location=0) out
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    OUT.outColor = float4(IN.fragColor.x, IN.fragColor.y, IN.fragColor.z, 1.0f);

    return OUT;
}
