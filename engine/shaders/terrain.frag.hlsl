// Tiled terrain FULL-detail fragment shader (near tiles).
//
// Lambert sun + EvaluateIrradiance sky ambient + env fog — the SAME env
// block the scene reads (FrameUniforms.env via the scene pc), so the
// terrain composites with the world's lighting/exposure. No shadows (the
// terrain is not in the shadow passes — visual-only), no textures.
#include "common.hlsli"
#include "environment.hlsli"

static const float PI = 3.14159265359f;

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 tangentBFlip : TEXCOORD2;
    [[vk::location(3)]] float4 color : COLOR1;
};

float4 mainPS(VSOut IN) : SV_Target0
{
    FrameUniforms frame = pc.frameUniforms.Get();
    EnvironmentFrameData env = frame.env;

    float3 N = normalize(IN.normal);
    float3 albedo = IN.color.rgb;

    // Direct sun (Lambert 1/PI, the scene convention) — unshadowed by
    // design (visual-only dressing; shadow coverage is future work).
    float3 L = normalize(env.sunDirection.xyz + 1e-9);
    float NdotL = saturate(dot(N, L));
    float3 sunE = env.sunColor.rgb * max(env.sunColor.w, 0.0) * NdotL;

    // Sky ambient (hemisphere blend of zenith/horizon/ground). The
    // environment ZONES own the sky where the map is on: the tint +
    // intensity blend per texel (EnvZoneIrradiance), so painting zones
    // paints the terrain's ambient.
    float4 zw = EnvZoneWeights(IN.worldPos.xz, frame);
    float3 skyE = EnvZoneIrradiance(N, zw, env, frame);

    float3 color = albedo * (sunE / PI + skyE);

    // Env fog (same factor/mode the scene pixels blend with). Zones own
    // the fog color + density: the zone blend replaces the env's.
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
