// Tiled terrain LOWER-detail fragment shader (far tiles).
//
// Cheap shading for the far LOD band: flat-up normal sun + the same env
// ambient/fog as the full path. No normal attribute arrives (the low
// pipeline binds POSITION + COLOR only), so sun NdotL uses the +Y
// up-axis — at the LOD distance the terrain reads as distant ground
// anyway, and the format exists precisely to skip the normal fetch.
#include "common.hlsli"
#include "environment.hlsli"

static const float PI = 3.14159265359f;

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR1;
};

float4 mainPS(VSOut IN) : SV_Target0
{
    FrameUniforms frame = pc.frameUniforms.Get();
    EnvironmentFrameData env = frame.env;

    float3 albedo = IN.color.rgb;

    // Flat-up sun + sky (the low band's approximation of the full path).
    // The environment ZONES still own the sky here (EnvZoneIrradiance
    // with the flat-up normal): far tiles keep the painted look.
    float3 L = normalize(env.sunDirection.xyz + 1e-9);
    float NdotL = saturate(L.y);
    float3 sunE = env.sunColor.rgb * max(env.sunColor.w, 0.0) * NdotL;
    float4 zw = EnvZoneWeights(IN.worldPos.xz, frame);
    float3 skyE = EnvZoneIrradiance(float3(0.0, 1.0, 0.0), zw, env, frame);

    float3 color = albedo * (sunE / PI + skyE);

    float viewDepth = length(IN.worldPos - frame.cameraPos.xyz);
    float fogHdrIntensity = env.fogColor.w;
    float3 zoneFogHdr = env.fogColor.rgb * fogHdrIntensity;
    float zoneFogDensity = env.fogDensityHeight.x;
    if (frame.envZoneMapIndex != 0xffffffffu)
        EnvZoneFog(zw, frame, zoneFogHdr, zoneFogDensity);
    float fog = ComputeFogFactorDensity(viewDepth, IN.worldPos.y, env,
        zoneFogDensity);
    color = lerp(color, zoneFogHdr, fog);

    return float4(color, 1.0);
}
