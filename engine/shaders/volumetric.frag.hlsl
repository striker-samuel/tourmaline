#define VOLUMETRIC_PASS
#include "common.hlsli"
#include "environment.hlsli"
#include "sggi.hlsli"
#include "dof.hlsli"

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/volumetric.h VolumetricPush (96 bytes).
struct VolumetricPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    uint hdrTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    uint steps;
    float2 texelSize;       // output texel (1/outputExtent)
    float2 depthTexelSize;  // full-res depth texel (1/depthExtent)
    float maxDistance;
    float density;
    float intensity;
    float anisotropy;
    float sunScale;
    float giScale;
    uint useGi;
    uint useSun;
    float jitterStrength;
    float heightFalloff;
    float heightBase;
    float time;
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<VolumetricPush> vp;

// Interleaved gradient noise: march-origin dither against banding.
float VolIgn(float2 px)
{
    return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
}

// Henyey-Greenstein phase (normalized-ish, 1/(4pi) convention folded into
// artist intensity; g in [-0.9, 0.9], +forward).
float VolPhaseHg(float cosT, float g)
{
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5);
    return (1.0 - g2) / max(denom, 1e-4);
}

// Isotropic mean radiance of the SG probe field: integral over the sphere
// divided by 4pi. Integral of one unit SG lobe = 2pi/l*(1-e^-2l); with the
// fixed sharpness 4 the mean weight is (1-e^-8)/8 ~= 0.125 per lobe.
float3 VolGiMean(float3 amps[SG_GI_LOBES])
{
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        sum += amps[i];
    return sum * 0.124959f;
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = vp.frameUniforms.Get();
    Texture2D hdrTex = textures[NonUniformResourceIndex(vp.hdrTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(vp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(vp.samplerIndex)];

    float3 base = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);

    float density = max(vp.density, 0.0f);
    float intensity = max(vp.intensity, 0.0f);
    if (density <= 1e-6 || intensity <= 1e-6)
    {
        OUT.outColor = float4(base, 1.0f);
        return OUT;
    }

    // Full-res depth tap for this pixel (output == render resolution, so
    // IN.uv maps 1:1 onto depth texels; downstream samples us by UV).
    float2 depthSize = float2(1.0 / max(vp.depthTexelSize.x, 1e-9),
                              1.0 / max(vp.depthTexelSize.y, 1e-9));
    int2 depthPx = int2(clamp(IN.uv * depthSize - 0.5,
                              float2(0.0, 0.0), depthSize - 1.0));
    float ndcDepth = depthTex.Load(int3(depthPx, 0)).r;

    // Camera ray in world space (exact, flip/finite-far agnostic: unproject
    // through invViewProj like the depth helpers do via invProj).
    float2 ndcXY = IN.uv * 2.0 - 1.0;
    float4 farH = mul(frame.invViewProj, float4(ndcXY, 0.0, 1.0)); // reversed-Z far
    float3 worldFar = farH.xyz / max(farH.w, 1e-9);
    float3 camPos = frame.cameraPos;
    float3 rayDir = worldFar - camPos;
    float farLen = length(rayDir);
    rayDir = (farLen > 1e-6) ? rayDir / farLen : float3(0.0, 0.0, 1.0);

    float rayLen;
    if (DofIsBackground(ndcDepth))
    {
        rayLen = max(vp.maxDistance, 1.0);
    }
    else
    {
        float4 hitH = mul(frame.invViewProj, float4(ndcXY, ndcDepth, 1.0));
        float3 worldHit = hitH.xyz / max(hitH.w, 1e-9);
        rayLen = clamp(length(worldHit - camPos), 0.0, max(vp.maxDistance, 1.0));
    }
    if (rayLen <= 1e-3)
    {
        OUT.outColor = float4(base, 1.0f);
        return OUT;
    }

    uint nSteps = clamp(vp.steps, 4, 128);
    // Static IGN dither for the march origin (banding guard), hashed at the
    // native pixel grid so the grain is per-pixel fine. Deliberately NOT
    // time-animated: with no temporal accumulation the drift term only makes
    // the field swim/flicker. fullPxF matches the depth tap above.
    float2 fullPxF = IN.uv * depthSize - 0.5;
    float jitter = VolIgn(fullPxF) * clamp(vp.jitterStrength, 0.0, 1.0);

    float3 sunDir = normalize(frame.env.sunDirection.xyz + 1e-9);
    float3 sunHdr = frame.env.sunColor.rgb * max(frame.env.sunColor.w, 0.0f);
    float iblOn = saturate(frame.env.fogEndAndFlags.w);
    float giOn = (vp.useGi != 0 && frame.giEnabled != 0) ? 1.0 : 0.0;
    float sunOn = (vp.useSun != 0) ? 1.0 : 0.0;
    float cosSun = dot(rayDir, sunDir);
    float phase = VolPhaseHg(cosSun, vp.anisotropy) * 0.0795775; // /4pi
    float giStrength = max(frame.giIntensity, 0.0f) * max(vp.giScale, 0.0f);

    float3 throughput = float3(1.0, 1.0, 1.0);
    float3 scatter = float3(0.0, 0.0, 0.0);

    [loop]
    for (uint i = 0; i < 128; ++i)
    {
        if (i >= nSteps)
            break;
        // Near-weighted (quadratic) step distribution: segment i spans
        // [rayLen*(i/N)^2, rayLen*((i+1)/N)^2], so samples concentrate near
        // the camera where the transmittance weight ~1. Uniform steps do the
        // opposite: the far samples that decide total extinction jump a full
        // coarse step per dithered pixel, so neighbors alternate between
        // extinctions (black-pixel checker on long/dense sightlines) while
        // wasting samples where the weight has already decayed to ~0.
        float fj0 = float(i) / float(nSteps);
        float fj1 = float(i + 1u) / float(nSteps);
        float s0 = rayLen * fj0 * fj0;
        float s1 = rayLen * fj1 * fj1;
        float segW = max(s1 - s0, 1e-6);
        float t = s0 + jitter * segW;
        float3 wpos = camPos + rayDir * t;

        float sigma = density;
        if (vp.heightFalloff > 0.0)
            sigma *= exp(-max(wpos.y - vp.heightBase, 0.0) * vp.heightFalloff);

        // Ambient in-scatter: probe field where covered, sky-IBL fallback
        // where not (same discipline as default_lit -- never additive).
        float3 ambient = float3(0.0, 0.0, 0.0);
        if (giOn > 0.5)
        {
            float3 amps[SG_GI_LOBES];
            float dbgW = 0.0; uint dbgC = 0u; bool dbgF = false; uint dbgL = 0u;
            if (SgGiSample(frame, wpos, amps, dbgW, dbgC, dbgF, dbgL))
                ambient = VolGiMean(amps) * giStrength;
            else
                ambient = EvaluateIrradiance(float3(0.0, 1.0, 0.0), frame.env)
                    * (0.31831 * giStrength);
        }
        else
        {
            ambient = EvaluateIrradiance(float3(0.0, 1.0, 0.0), frame.env)
                * (0.31831 * giStrength) * iblOn;
        }

        float3 source = ambient;
        if (sunOn > 0.5)
            source += sunHdr * (phase * max(vp.sunScale, 0.0f));
        // Quarantine: a non-finite baked probe amplitude (NaN/Inf) must cost
        // one segment's scatter, never a poisoned pixel (NaN out stores NaN
        // -> downstream sanitize -> black; Inf -> white block).
        source = SanitizeHdr(source);

        float sigmaS = sigma * intensity;
        float3 seg = throughput * (sigmaS * source * segW);
        scatter += SanitizeHdr(seg);
        float trans = exp(-sigma * segW);
        throughput *= trans;

        if (throughput.r + throughput.g + throughput.b < 1e-3)
        {
            throughput = float3(0.0, 0.0, 0.0);
            break;
        }
    }

    float3 outColor = base * throughput + scatter;
    OUT.outColor = float4(SanitizeHdr(outColor), 1.0f);
    return OUT;
}
