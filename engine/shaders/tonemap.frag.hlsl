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
    uint gammaEncode; // 1 = write encoded bytes (UNORM LDR target), 0 = linear into the sRGB swapchain
    uint aoTextureIndex; // half-res SSAO, 0xffffffff = disabled
    float aoIntensity;
    uint lutTextureIndex; // 1024x32 LUT strip (32^3), 0xffffffff = disabled
    float lutIntensity;
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

// Artistic grading: TPP-style 32x32x32 LUT stored as a 1024x32 strip (32
// tiles along x, blue = tile, red = column, green = row). The strip bytes
// are sRGB-encoded (authored in Photoshop) and the texture uses an _SRGB
// view, so sampling lands in linear space - the grading composites in
// linear, pre-encode. The sampler is linear/clamp (tp.samplerIndex).
float3 SampleLut(float3 c)
{
    c = saturate(c);
    float blue = c.b * 31.0f;
    float tile = floor(blue);
    float fracB = blue - tile;
    Texture2D lut = textures[NonUniformResourceIndex(tp.lutTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(tp.samplerIndex)];
    float2 uv0 = float2((tile * 32.0f + c.r * 31.0f + 0.5f) / 1024.0f,
                        (c.g * 31.0f + 0.5f) / 32.0f);
    float2 uv1 = float2(uv0.x + 32.0f / 1024.0f, uv0.y);
    float3 a = lut.Sample(smp, uv0).rgb;
    float3 b = lut.Sample(smp, uv1).rgb;
    return lerp(a, b, fracB);
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
        float3 rgb = HsvToRgb(float3(hue, 1.0, mag));
        OUT.outColor = float4(tp.gammaEncode ? SrgbEncode(rgb) : rgb, 1.0f);
        return OUT;
    }

    float3 hdr = float3(0,0,0);
    if (hdrIdx != 0xffffffff && hdrIdx < 100000)
        // Sanitize: a stale/foreign +inf texel in the RGBA16F target turns
        // Reinhard/ACES into inf/inf = NaN -> saturate(NaN) = 0 (black
        // blocks); same discipline as bloom_down's tap clamp.
        hdr = SanitizeHdr(textures[NonUniformResourceIndex(hdrIdx)].Sample(samplers[NonUniformResourceIndex(smpIdx)], IN.uv).rgb);

    // SSAO composite (half-res AO, bilinearly upscaled by the sampler).
    // Pre-bloom, pre-tonemap; disabled when index is 0xffffffff.
    // NOTE: this app consumes SSAO in the opaque lighting pass instead
    // (default_lit.frag.hlsl + ao.hlsli, depth-guided upsample); this path is
    // dormant here. The plain bilinear tap below has no depth authority —
    // it would leak AO across geometry/sky silhouettes if ever reused.
    if (tp.aoTextureIndex != 0xffffffff && tp.aoTextureIndex < 100000 && tp.aoIntensity > 0.0f)
    {
        float ao = textures[NonUniformResourceIndex(tp.aoTextureIndex)].Sample(samplers[NonUniformResourceIndex(smpIdx)], IN.uv).r;
        hdr *= lerp(1.0f, saturate(ao), saturate(tp.aoIntensity));
    }

    // Bloom composite (HDR add, pre-tonemap). Disabled when index is 0xffffffff.
    if (tp.bloomTextureIndex != 0xffffffff && tp.bloomTextureIndex < 100000 && tp.bloomIntensity > 0.0f)
    {
        float3 bloom = textures[NonUniformResourceIndex(tp.bloomTextureIndex)].Sample(samplers[NonUniformResourceIndex(smpIdx)], IN.uv).rgb;
        hdr += bloom * tp.bloomIntensity;
    }

    float3 mapped = Tonemap(hdr * exposure, mode);
    if (tp.lutTextureIndex != 0xffffffff && tp.lutTextureIndex < 100000 && tp.lutIntensity > 0.0f)
        mapped = lerp(mapped, SampleLut(mapped), saturate(tp.lutIntensity));
    // sRGB swapchain: store linear, the hardware encodes. Only the UNORM
    // SMAA ldr target (gammaEncode) needs the curve applied here.
    float3 ldr = tp.gammaEncode ? SrgbEncode(mapped) : saturate(mapped);
    OUT.outColor = float4(ldr, 1.0f);
    return OUT;
}
