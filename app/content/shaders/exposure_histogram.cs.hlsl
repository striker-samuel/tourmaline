// Auto-exposure histogram builder (log2 luminance, 64 bins).
//
// One dispatch per frame over the scene HDR (pre-exposure, so the metered
// values are exposure-independent). Each thread samples one sampleStep-
// spaced texel via .Load (no sampler needed), bins its log2 luminance,
// and accumulates into groupshared; the first 64 threads flush nonzero
// bins to the device histogram buffer with global atomics.
//
// The CPU zeroes the buffer each frame before this dispatch (transfer
// write; the rendergraph promotes it to Compute R/W), downloads the
// 256-byte result after Execute, and damps exposure toward
// keyValue * 2^evBias / percentileLum (see ExposureMeter::Adapt).
//
// Bins outside [minLogLum, maxLogLum] clamp to the edge bins; black
// (lum <= 1e-6), NaN and +/-inf texels are skipped (uncounted).

#define EXPOSURE_PASS
#include "common.hlsli"

#define EXPOSURE_BINS 64

struct ExposureHistogramBuffer
{
    uint bins[EXPOSURE_BINS];
};

// Must match Engine/Rendergraph/exposure.h ExposureHistogramPush (40 bytes).
struct ExposureHistogramPush
{
    vk::BufferPointer<ExposureHistogramBuffer> histogram;
    uint hdrTextureIndex;
    uint width;
    uint height;
    float minLogLum;
    float maxLogLum;
    uint binCount;
    uint sampleStep;
    uint pad;
};
// NOTE: no static_assert here – this DXC (HLSL 2018 mode) rejects it at
// global scope. Size is checked on the C++ side (40 bytes).

[[vk::push_constant]]
ConstantBuffer<ExposureHistogramPush> pcExp;

groupshared uint tileBins[EXPOSURE_BINS];

float ExposureLuma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(16, 16, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    uint linearTid = threadId.y * 16u + threadId.x;
    uint bins = min(pcExp.binCount, (uint)EXPOSURE_BINS);
    uint step = max(pcExp.sampleStep, 1u);

    // Zero the groupshared tally (64 bins, 256 threads -> ~1 iter).
    for (uint i = linearTid; i < EXPOSURE_BINS; i += 256u)
        tileBins[i] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint px = groupId.x * 16u * step + threadId.x * step;
    uint py = groupId.y * 16u * step + threadId.y * step;
    if (px < pcExp.width && py < pcExp.height &&
        pcExp.hdrTextureIndex != 0xffffffff)
    {
        float3 hdr = textures[NonUniformResourceIndex(pcExp.hdrTextureIndex)]
            .Load(int3((int)px, (int)py, 0)).rgb;
        float lum = ExposureLuma(hdr);
        // Skip black / NaN / inf: isfinite is false for NaN and +/-inf.
        if (isfinite(lum) && lum > 1e-6f)
        {
            float logLum = log2(lum);
            float t = (logLum - pcExp.minLogLum) /
                max(pcExp.maxLogLum - pcExp.minLogLum, 1e-6f);
            uint bin = (uint)(saturate(t) * (float)(bins - 1u));
            InterlockedAdd(tileBins[min(bin, bins - 1u)], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Flush nonzero bins to the device buffer (<= 64 global atomics/group).
    for (uint b = linearTid; b < bins; b += 256u)
    {
        uint n = tileBins[b];
        if (n != 0)
            InterlockedAdd(pcExp.histogram.Get().bins[b], n);
    }
}
