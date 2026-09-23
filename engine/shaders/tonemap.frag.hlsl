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
    uint tonemapMode; // 0=Reinhard, 1=ACES, 2=Uncharted2, 3=GT7
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
    float2 jitterDelta; // debugView 1: (prevJitter - currJitter) UV, stripped
                        // so the motion visualizer shows TRUE motion
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

// ----- GT7 tonemapping (Polyphony Digital, SIGGRAPH 2025, MIT) -----
// Port of the SDR path of the official gt7_tone_mapping.cpp sample
// (gt7_tone_mapping.cpp v1.0 2025-08-10, Copyright (c) 2025
// Polyphony Digital Inc., MIT License):
// GTToneMappingCurveV2 (peak=2.5fb/250nits, alpha=0.25, mid=0.538,
// linearSection=0.444, toe=1.28) per-channel + ICtCp chroma preservation
// (blend 0.6, fade 0.98-1.16) + 1/2.5 SDR correction. Input is the
// exposure-scaled HDR (hdr * exposure, 1.0 == 100 nits); output is linear
// LDR 0..1 ready for the sRGB encode below. Applied in the working-space
// RGB (reference assumes Rec.2020; gamut mapping is approximate here).
float GT7Smooth(float edge0, float edge1, float x)
{
    float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// GTToneMappingCurveV2.fit for the SDR defaults (see constants above).
// k=(0.444-1)/(0.25-1)=0.7413333, kA=2.9633333, kB=-3.3733512,
// kC=-0.5395683, linearSection*peak=1.11.
float GT7CurveEval(float x)
{
    if (x <= 0.0f)
        return 0.0f;
    float shoulder = 2.9633333f + (-3.3733512f) * exp(x * (-0.5395683f));
    if (x < 1.11f)
    {
        float wLinear = GT7Smooth(0.0f, 0.538f, x);
        float toeMapped = 0.538f * pow(x / 0.538f, 1.28f);
        return (1.0f - wLinear) * toeMapped + wLinear * x;
    }
    return shoulder;
}

// ST-2084 PQ helpers rescaled to the GT framebuffer convention
// (1.0 fb == 100 nits). Mirrors eotfSt2084/inverseEotfSt2084.
float GT7InvPQ(float v)
{
    float y = saturate((v * 100.0f) / 10000.0f);
    float ym = pow(y, 0.1593017578125f);
    float num = 0.8359375f + 18.8515625f * ym;
    float den = 1.0f + 18.6875f * ym;
    return exp2(78.84375f * (log2(num) - log2(den)));
}
float GT7PQ(float n)
{
    n = saturate(n);
    float np = pow(n, 1.0f / 78.84375f);
    float l = max(np - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * np);
    return pow(l, 1.0f / 0.1593017578125f) * 100.0f; // 10000 nits / 100 fb
}
float3 GT7RgbToICtCp(float3 rgb)
{
    float l = (rgb.r * 1688.0f + rgb.g * 2146.0f + rgb.b * 262.0f) / 4096.0f;
    float m = (rgb.r * 683.0f + rgb.g * 2951.0f + rgb.b * 462.0f) / 4096.0f;
    float s = (rgb.r * 99.0f + rgb.g * 309.0f + rgb.b * 3688.0f) / 4096.0f;
    float lPQ = GT7InvPQ(l);
    float mPQ = GT7InvPQ(m);
    float sPQ = GT7InvPQ(s);
    return float3(
        (2048.0f * lPQ + 2048.0f * mPQ) / 4096.0f,
        (6610.0f * lPQ - 13613.0f * mPQ + 7003.0f * sPQ) / 4096.0f,
        (17933.0f * lPQ - 17390.0f * mPQ - 543.0f * sPQ) / 4096.0f);
}
float3 GT7ICtCpToRgb(float3 c)
{
    float l = c.x + 0.00860904f * c.y + 0.11103f * c.z;
    float m = c.x - 0.00860904f * c.y - 0.11103f * c.z;
    float s = c.x + 0.560031f * c.y - 0.320627f * c.z;
    float lLin = GT7PQ(l);
    float mLin = GT7PQ(m);
    float sLin = GT7PQ(s);
    return max(float3(
        3.43661f * lLin - 2.50645f * mLin + 0.0698454f * sLin,
        -0.79133f * lLin + 1.9836f * mLin - 0.192271f * sLin,
        -0.0259499f * lLin - 0.0989137f * mLin + 1.12486f * sLin), 0.0f);
}
float3 GT7Tonemap(float3 c)
{
    c = max(c, 0.0f);
    float3 ucs = GT7RgbToICtCp(c);
    float3 skewed = float3(GT7CurveEval(c.r), GT7CurveEval(c.g), GT7CurveEval(c.b));
    float3 skewedUcs = GT7RgbToICtCp(skewed);
    // I of grey 2.5fb (250 nits paper white) == 0.6025592.
    float chromaScale = 1.0f - GT7Smooth(0.98f, 1.16f, ucs.x / 0.6025592f);
    float3 scaledUcs = float3(skewedUcs.x, ucs.y * chromaScale, ucs.z * chromaScale);
    float3 scaledRgb = GT7ICtCpToRgb(scaledUcs);
    float3 blended = 0.4f * skewed + 0.6f * scaledRgb; // blendRatio 0.6
    return 0.4f * min(blended, 2.5f); // SDR correction 1/2.5, clamp to peak
}

float3 Tonemap(float3 c, uint mode)
{
    if (mode == 0) return Reinhard(c);
    if (mode == 2) return Uncharted2(c);
    if (mode == 3) return GT7Tonemap(c);
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
    // Replaces the scene composite entirely while enabled. The raw vectors
    // include the temporal-jitter delta (non-zero on a still camera with
    // T2x/DLSS active - required for their reprojection); strip it so the
    // view shows TRUE motion only.
    if (tp.debugView == 1 && tp.motionTextureIndex != 0xffffffff && tp.motionTextureIndex < 100000)
    {
        float2 v = textures[NonUniformResourceIndex(tp.motionTextureIndex)]
            .Sample(samplers[NonUniformResourceIndex(smpIdx)], IN.uv).rg;
        v = (v - tp.jitterDelta) * tp.motionDebugScale;
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
