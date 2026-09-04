#define SKY_PASS
#include "common.hlsli"
#include "environment.hlsli"

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = pc.frameUniforms.Get();
    EnvironmentFrameData env = frame.env;

    // Reconstruct the world-space view ray from the fullscreen UV.
    // clip z = 1 (far plane), unproject, perspective-divide, subtract camera.
    float2 ndc = IN.uv * 2.0f - 1.0f;
    float4 clip = float4(ndc, 1.0f, 1.0f);
    float4 worldH = mul(frame.invViewProj, clip);
    float3 worldPos = worldH.xyz / max(worldH.w, 1e-6f);
    float3 dir = worldPos - frame.cameraPos;
    float len = length(dir);
    dir = len > 1e-6f ? dir / len : float3(0.0f, 1.0f, 0.0f);

    float3 sky = EvaluateSkyColor(dir, env);

    // Blend the horizon band toward fog so distant geometry fading to fog
    // meets the dome seamlessly. Uses horizon-band weight, not depth.
    float fogMode = env.fogEndAndFlags.z;
    if (fogMode > 0.5f)
    {
        float3 fogHdr = env.fogColor.rgb * max(env.fogColor.w, 0.0f);
        float band = 1.0f - saturate(abs(dir.y) * 4.0f);
        sky = lerp(sky, fogHdr, band * 0.5f);
    }

    OUT.outColor = float4(sky, 1.0f);
    return OUT;
}
