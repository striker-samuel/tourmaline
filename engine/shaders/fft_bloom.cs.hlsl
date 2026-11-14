// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Optional FFT bloom quality filter. 32x32 complex FFTs convolve the
// accumulated half-resolution bloom in 16x16 output tiles (overlap-save).
// Two in-place radix-2 1D transforms per direction are performed in LDS.
// The analytic PSF is a normalized mixture of an isotropic Gaussian and
// two orthogonal anisotropic Gaussians. Its frequency response is real,
// so no per-frame kernel FFT or frequency-image storage is necessary.
//
// The 8-pixel halo bounds the useful PSF support. This is a LOCAL FFT
// convolution, not a full-frame spectral convolution; extremely long
// diffraction spikes require larger tiles/a separate full-frame pipeline.

#define BLOOM_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"

static const uint FFT_N = 32u;
static const uint FFT_TILE = 16u;
static const float FFT_PI = 3.14159265358979323846f;

struct FftBloomPush
{
    uint srcIndex;
    uint dstIndex;
    uint width;
    uint height;
};
GPU_PUSH(FftBloomPush, fp);

[[vk::binding(2, 0)]]
RWTexture2D<float4> fftImages[];

groupshared float4 fftReal[FFT_N * FFT_N];
groupshared float4 fftImag[FFT_N * FFT_N];

uint Reverse5(uint x)
{
    return ((x & 1u) << 4u) | ((x & 2u) << 2u) |
           (x & 4u) | ((x & 8u) >> 2u) | ((x & 16u) >> 4u);
}

// Bit-reverse a 1D axis in-place. Only one of each swap pair writes.
void BitReverseAxis(uint lane, bool vertical)
{
    [unroll]
    for (uint j = 0; j < 4u; ++j)
    {
        uint index = lane + j * 256u;
        uint x = index & 31u;
        uint y = index >> 5u;
        uint axis = vertical ? y : x;
        uint rev = Reverse5(axis);
        if (rev > axis)
        {
            uint other = vertical ? (rev * 32u + x) : (y * 32u + rev);
            float4 r = fftReal[index], i = fftImag[index];
            fftReal[index] = fftReal[other];
            fftImag[index] = fftImag[other];
            fftReal[other] = r;
            fftImag[other] = i;
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

// Iterative decimation-in-time FFT; input must be bit-reversed.
void TransformAxis(uint lane, bool vertical, bool inverse)
{
    [unroll]
    for (uint stage = 1u; stage <= 5u; ++stage)
    {
        uint span = 1u << stage;
        uint halfSpan = span >> 1u;
        [unroll]
        for (uint j = 0; j < 2u; ++j)
        {
            uint butterfly = lane + j * 256u; // 512 butterfly pairs
            uint line = butterfly >> 4u;
            uint k = butterfly & 15u;
            uint start = (k / halfSpan) * span + (k % halfSpan);
            uint other = start + halfSpan;
            uint a = vertical ? (start * 32u + line) : (line * 32u + start);
            uint b = vertical ? (other * 32u + line) : (line * 32u + other);
            float angle = (inverse ? 2.0f : -2.0f) * FFT_PI *
                          float(k % halfSpan) / float(span);
            float sn, cs;
            sincos(angle, sn, cs);
            float4 br = fftReal[b], bi = fftImag[b];
            float4 tr = br * cs - bi * sn;
            float4 ti = br * sn + bi * cs;
            float4 ar = fftReal[a], ai = fftImag[a];
            fftReal[a] = ar + tr;
            fftImag[a] = ai + ti;
            fftReal[b] = ar - tr;
            fftImag[b] = ai - ti;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

float PsfTransfer(uint2 frequency)
{
    float2 f = float2(frequency.x <= 16u ? int(frequency.x) : int(frequency.x) - 32,
                      frequency.y <= 16u ? int(frequency.y) : int(frequency.y) - 32) / 32.0f;
    float2 f2 = f * f;
    const float k = -2.0f * FFT_PI * FFT_PI;
    float core = exp(k * (1.6f * 1.6f) * (f2.x + f2.y));
    float horizontal = exp(k * (3.0f * 3.0f * f2.x + 0.65f * 0.65f * f2.y));
    float vertical = exp(k * (0.65f * 0.65f * f2.x + 3.0f * 3.0f * f2.y));
    return 0.65f * core + 0.175f * horizontal + 0.175f * vertical;
}

[numthreads(256, 1, 1)]
void mainCS(uint lane : SV_GroupIndex, uint3 group : SV_GroupID)
{
    Texture2D src = textures[fp.srcIndex];
    uint2 tileOrigin = group.xy * FFT_TILE;

    // Bit-reversed horizontal input order, with zero padding beyond edges.
    [unroll]
    for (uint j = 0u; j < 4u; ++j)
    {
        uint index = lane + j * 256u;
        uint2 pixel = uint2(index & 31u, index >> 5u);
        int2 p = int2(tileOrigin + pixel) - int2(8, 8);
        float3 sampleColor = 0.0f;
        if (p.x >= 0 && p.y >= 0 && p.x < int(fp.width) && p.y < int(fp.height))
            sampleColor = SanitizeHdr(src.Load(int3(p, 0)).rgb);
        fftReal[pixel.y * 32u + Reverse5(pixel.x)] = float4(sampleColor, 0.0f);
        fftImag[pixel.y * 32u + Reverse5(pixel.x)] = 0.0f;
    }
    GroupMemoryBarrierWithGroupSync();

    TransformAxis(lane, false, false);
    BitReverseAxis(lane, true);
    TransformAxis(lane, true, false);

    [unroll]
    for (uint j = 0u; j < 4u; ++j)
    {
        uint idx = lane + j * 256u;
        float transfer = PsfTransfer(uint2(idx & 31u, idx >> 5u));
        fftReal[idx] *= transfer;
        fftImag[idx] *= transfer;
    }
    GroupMemoryBarrierWithGroupSync();

    BitReverseAxis(lane, true);
    TransformAxis(lane, true, true);
    BitReverseAxis(lane, false);
    TransformAxis(lane, false, true);

    // 32x32 inverse normalization. Keep the center 16x16 samples.
    uint2 local = uint2(lane & 15u, lane >> 4u);
    uint2 dst = tileOrigin + local;
    if (dst.x < fp.width && dst.y < fp.height)
    {
        float3 result = max(fftReal[(local.y + 8u) * 32u + local.x + 8u].rgb / 1024.0f, 0.0f);
        fftImages[fp.dstIndex][dst] = float4(result, 1.0f);
    }
}
