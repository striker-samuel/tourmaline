#define TONEMAP_PASS
#include "common.hlsli"

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

struct TonemapPush
{
    uint hdrTextureIndex;
    uint samplerIndex;
    float exposure;
    uint tonemapMode; // 0=Reinhard, 1=ACES, 2=Uncharted2
    uint bloomTextureIndex; // 0xffffffff = disabled
    float bloomIntensity;
    uint debugView; // 0=scene, 1=motion vectors
    uint motionTextureIndex; // 0xffffffff = disabled
    float motionDebugScale;
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<TonemapPush> tp;

// ----- helpers -----
float3 Reinhard(float3 c)
{
    return c / (c + 1.0f);
}

// ACES approx (Hill) – handles HDR > 10k lux
float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

float3 Uncharted2Tonemap(float3 x)
{
    float A = 0.15f; float B = 0.50f; float C = 0.10f;
    float D = 0.20f; float E = 0.02f; float F = 0.30f;
    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}
float3 Uncharted2(float3 c)
{
    float3 curr = Uncharted2Tonemap(c * 2.0f);
    float3 whiteScale = 1.0f / Uncharted2Tonemap(float3(11.2f,11.2f,11.2f));
    return curr * whiteScale;
}

float3 Tonemap(float3 c, uint mode)
{
    if (mode == 0) return Reinhard(c);
    if (mode == 2) return Uncharted2(c);
    return ACESFilm(c); // default 1 = ACES
}

float3 HsvToRgb(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    uint hdrIdx = tp.hdrTextureIndex;
    uint smpIdx = tp.samplerIndex;
    float exposure = tp.exposure;
    uint mode = tp.tonemapMode;

    // Motion vector debug view: direction -> hue, magnitude -> intensity.
    // Replaces the scene composite entirely while enabled.
    if (tp.debugView == 1 && tp.motionTextureIndex != 0xffffffff && tp.motionTextureIndex < 100000)
    {
        float2 v = textures[NonUniformResourceIndex(tp.motionTextureIndex)]
            .Sample(samplers[NonUniformResourceIndex(smpIdx)], IN.uv).rg;
        v *= tp.motionDebugScale;
        float mag = saturate(length(v));
        float hue = atan2(v.y, v.x) * 0.15915494 + 0.5; // /2pi + 0.5
        OUT.outColor = float4(HsvToRgb(float3(hue, 1.0, mag)), 1.0);
        return OUT;
    }

    float3 hdr = float3(0,0,0);
    if (hdrIdx != 0xffffffff && hdrIdx < 100000)
        hdr = textures[NonUniformResourceIndex(hdrIdx)].Sample(samplers[NonUniformResourceIndex(smpIdx)], IN.uv).rgb;

    // Bloom composite (HDR add, pre-tonemap). Disabled when index is 0xffffffff.
    if (tp.bloomTextureIndex != 0xffffffff && tp.bloomTextureIndex < 100000 && tp.bloomIntensity > 0.0f)
    {
        float3 bloom = textures[NonUniformResourceIndex(tp.bloomTextureIndex)].Sample(samplers[NonUniformResourceIndex(smpIdx)], IN.uv).rgb;
        hdr += bloom * tp.bloomIntensity;
    }

    float3 mapped = Tonemap(hdr * exposure, mode);
    // Manual gamma – swapchain is UNORM (B8G8R8A8_SRGB would auto-convert, but we assume UNORM path)
    float3 ldr = pow(saturate(mapped), 1.0f/2.2f);
    OUT.outColor = float4(ldr, 1.0f);
    return OUT;
}
